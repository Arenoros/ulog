#!/usr/bin/env python3
"""Fetch + diff ulog-bench results from GitHub Actions.

Subcommands
-----------
    list    [--limit N] [--branch B]
    fetch   <sha|HEAD|latest> [--force]
    show    [<sha|HEAD|latest>]
    compare <base-sha|HEAD~1|...> <head-sha|HEAD|latest> [--threshold PCT]
    trends  [--limit N] [--no-open]
    csv     [--limit N] [--output PATH]

The bench workflow uploads an artifact named ``ulog-bench-<sha>`` that
contains ``bench.json`` (Google Benchmark format). This script drives
``gh`` to locate the run for a given commit, downloads the artifact to
``.bench-cache/<sha>.json``, and prints a table / per-benchmark diff.

No third-party deps. Requires ``gh`` CLI on PATH + an authenticated
GitHub session (``gh auth status``).
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
import tempfile
import webbrowser
from pathlib import Path
from typing import Any

CACHE_DIR = Path(".bench-cache")
ARTIFACT_PREFIX = "ulog-bench-"
WORKFLOW_JOB_NAME = "bench"


# --------------------------------------------------------------------- gh
def _gh(*args: str, check: bool = True) -> str:
    """Run `gh` and return stdout (text). Raise on non-zero if check."""
    try:
        r = subprocess.run(
            ["gh", *args],
            check=check,
            capture_output=True,
            text=True,
            encoding="utf-8",
        )
    except FileNotFoundError:
        sys.exit("error: `gh` CLI not found on PATH")
    if r.returncode != 0 and check:
        sys.exit(f"error: gh {' '.join(args)}\n{r.stderr.strip()}")
    return r.stdout


def _repo_slug() -> str:
    """owner/name of the current repo (honours `gh repo set-default`)."""
    out = _gh("repo", "view", "--json", "nameWithOwner", "-q", ".nameWithOwner").strip()
    if not out:
        sys.exit("error: cannot detect repo (run inside a GitHub-linked clone)")
    return out


# --------------------------------------------------------------------- git
def _resolve_sha(ref: str) -> str:
    """Resolve an arbitrary git ref to a full SHA using the local repo."""
    if ref in ("latest",):
        return ref  # handled via API later
    r = subprocess.run(
        ["git", "rev-parse", ref],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        sys.exit(f"error: cannot resolve ref '{ref}': {r.stderr.strip()}")
    return r.stdout.strip()


# --------------------------------------------------------- bench JSON helpers
def _median_or_real(b: dict[str, Any]) -> float:
    """Pick a comparable time for a benchmark entry.

    Google Benchmark emits one entry per iteration plus optional
    aggregates (``mean``, ``median``, ``stddev``). Prefer median when we
    have it; otherwise the first iteration's ``real_time``.
    """
    return float(b.get("real_time", 0.0))


def _index_bench(js: dict[str, Any]) -> dict[str, dict[str, Any]]:
    """name -> best representative entry (median aggregate if present)."""
    by_name: dict[str, dict[str, Any]] = {}
    for b in js.get("benchmarks", []):
        name = b.get("run_name") or b.get("name")
        if not name:
            continue
        agg = b.get("aggregate_name")  # 'mean' | 'median' | 'stddev' | None
        if agg == "median":
            by_name[name] = b  # overwrite: prefer median
        elif name not in by_name and agg in (None, "", "iteration"):
            by_name[name] = b
    return by_name


# --------------------------------------------------------------- run lookup
def _find_run_id_for_sha(repo: str, sha: str) -> str | None:
    """Return workflow-run id that produced the bench artifact for sha."""
    # gh run list --json only returns the most recent N (default 20) runs;
    # query the REST API directly against head_sha for robustness.
    out = _gh(
        "api",
        f"/repos/{repo}/actions/runs?head_sha={sha}&per_page=50",
        "-q",
        ".workflow_runs[] | "
        'select(.name == "ulog-ci") | '
        "[.id, .conclusion, .status] | @tsv",
    )
    # Prefer a completed+successful run; fall back to any completed run.
    successful: str | None = None
    any_done: str | None = None
    for line in out.strip().splitlines():
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        rid, concl, status = parts[0], parts[1], parts[2]
        if status != "completed":
            continue
        any_done = any_done or rid
        if concl == "success":
            successful = successful or rid
    return successful or any_done


def _latest_bench_run(repo: str, branch: str = "main") -> tuple[str, str] | None:
    """Return (run_id, head_sha) of the most recent bench run on branch."""
    out = _gh(
        "api",
        f"/repos/{repo}/actions/runs?branch={branch}&per_page=20",
        "-q",
        ".workflow_runs[] | "
        'select(.name == "ulog-ci" and .status == "completed") | '
        "[.id, .head_sha, .conclusion] | @tsv",
    )
    for line in out.strip().splitlines():
        parts = line.split("\t")
        if len(parts) >= 2:
            return parts[0], parts[1]
    return None


def _download_artifact(repo: str, run_id: str, sha: str, dest: Path) -> None:
    """Download `ulog-bench-<sha>/bench.json` to dest."""
    name = f"{ARTIFACT_PREFIX}{sha}"
    with tempfile.TemporaryDirectory() as tmp:
        tmpd = Path(tmp)
        # `gh run download` extracts the artifact zip in-place under
        # a subdir named after the artifact. `-R` picks the repo so the
        # command works from any clone.
        _gh(
            "run",
            "download",
            run_id,
            "-R",
            repo,
            "-n",
            name,
            "-D",
            str(tmpd),
        )
        src = tmpd / "bench.json"
        if not src.exists():
            # Some versions drop into a subdir; search one level.
            cand = list(tmpd.rglob("bench.json"))
            if not cand:
                sys.exit(f"error: bench.json not found in artifact '{name}'")
            src = cand[0]
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(src), dest)


# ---------------------------------------------------------------- commands
def cmd_list(args: argparse.Namespace) -> int:
    repo = _repo_slug()
    branch = args.branch
    out = _gh(
        "api",
        f"/repos/{repo}/actions/runs?branch={branch}&per_page={args.limit}",
        "-q",
        ".workflow_runs[] | "
        'select(.name == "ulog-ci") | '
        "[.head_sha[0:12], .conclusion // .status, .created_at, .html_url] | @tsv",
    )
    print(f"{'sha':<14}{'status':<12}{'created':<22}url")
    for line in out.strip().splitlines():
        parts = line.split("\t")
        if len(parts) == 4:
            print(f"{parts[0]:<14}{parts[1]:<12}{parts[2]:<22}{parts[3]}")
    return 0


def _ensure_cached(ref: str, force: bool = False) -> tuple[str, Path]:
    """Fetch bench.json for `ref` into cache; return (sha, path)."""
    repo = _repo_slug()
    if ref == "latest":
        found = _latest_bench_run(repo)
        if not found:
            sys.exit("error: no completed bench runs found on main")
        run_id, sha = found
    else:
        sha = _resolve_sha(ref)
        run_id = _find_run_id_for_sha(repo, sha) or ""
    dest = CACHE_DIR / f"{sha}.json"
    if dest.exists() and not force:
        return sha, dest
    if not run_id:
        sys.exit(f"error: no ulog-ci run found for sha {sha[:12]}")
    print(f"downloading {ARTIFACT_PREFIX}{sha[:12]} (run {run_id}) ...")
    _download_artifact(repo, run_id, sha, dest)
    return sha, dest


def cmd_fetch(args: argparse.Namespace) -> int:
    sha, path = _ensure_cached(args.ref, force=args.force)
    print(f"{sha[:12]} -> {path}")
    return 0


def cmd_show(args: argparse.Namespace) -> int:
    ref = args.ref or "HEAD"
    sha, path = _ensure_cached(ref)
    js = json.loads(path.read_text(encoding="utf-8"))
    ctx = js.get("context", {})
    print(f"# {sha[:12]}  {ctx.get('date', '')}  {ctx.get('host_name', '')}")
    print(f"# cpus={ctx.get('num_cpus', '?')} "
          f"mhz={ctx.get('mhz_per_cpu', '?')} "
          f"lib={ctx.get('library_build_type', '?')}")
    idx = _index_bench(js)
    name_w = max((len(n) for n in idx), default=20)
    print(f"{'name':<{name_w}}  {'real_time':>14}  {'cpu_time':>14}  {'iters':>10}  unit")
    for name, b in sorted(idx.items()):
        print(
            f"{name:<{name_w}}  "
            f"{b.get('real_time', 0):>14.2f}  "
            f"{b.get('cpu_time', 0):>14.2f}  "
            f"{b.get('iterations', 0):>10}  "
            f"{b.get('time_unit', '?')}"
        )
    return 0


def cmd_compare(args: argparse.Namespace) -> int:
    base_sha, base_path = _ensure_cached(args.base)
    head_sha, head_path = _ensure_cached(args.head)
    base_js = json.loads(base_path.read_text(encoding="utf-8"))
    head_js = json.loads(head_path.read_text(encoding="utf-8"))
    base_idx = _index_bench(base_js)
    head_idx = _index_bench(head_js)

    names = sorted(set(base_idx) | set(head_idx))
    name_w = max((len(n) for n in names), default=20)
    print(f"base = {base_sha[:12]}")
    print(f"head = {head_sha[:12]}")
    print(f"{'name':<{name_w}}  {'base':>14}  {'head':>14}  {'delta%':>8}  unit")

    regressions: list[tuple[str, float]] = []
    for name in names:
        b = base_idx.get(name)
        h = head_idx.get(name)
        if not b or not h:
            status = "MISSING_HEAD" if b else "NEW"
            base_v = _median_or_real(b) if b else 0.0
            head_v = _median_or_real(h) if h else 0.0
            print(
                f"{name:<{name_w}}  {base_v:>14.2f}  {head_v:>14.2f}  "
                f"{'--':>8}  {status}"
            )
            continue
        bv = _median_or_real(b)
        hv = _median_or_real(h)
        delta = ((hv - bv) / bv * 100.0) if bv else 0.0
        unit = h.get("time_unit", "?")
        mark = ""
        if delta > args.threshold:
            mark = " REGRESSION"
            regressions.append((name, delta))
        elif delta < -args.threshold:
            mark = " IMPROVED"
        print(
            f"{name:<{name_w}}  {bv:>14.2f}  {hv:>14.2f}  "
            f"{delta:>+8.2f}  {unit}{mark}"
        )

    if regressions:
        print()
        print(f"regressions >{args.threshold:+.1f}%:")
        for n, d in sorted(regressions, key=lambda x: -x[1]):
            print(f"  {d:+7.2f}%  {n}")
        return 1
    return 0


# ---------------------------------------------------------------- trends
_HTML_TEMPLATE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<title>ulog bench trends</title>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4"></script>
<style>
  body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
         max-width: 1280px; margin: 2em auto; padding: 0 1em; color: #222; }
  h1 { margin-bottom: 0.2em; }
  h2 { margin-top: 2.5em; border-bottom: 1px solid #eee; padding-bottom: 0.3em; }
  .sub { color: #666; font-size: 0.9em; margin-bottom: 1.5em; }
  .controls { margin: 1em 0; display: flex; gap: 1.5em; align-items: center;
              flex-wrap: wrap; font-size: 0.9em; }
  .controls label { cursor: pointer; }
  .controls button { padding: 4px 10px; cursor: pointer; border: 1px solid #ccc;
                     background: #f7f7f7; border-radius: 3px; }
  .controls button:hover { background: #eee; }
  .chart { margin: 1.5em 0; border: 1px solid #e5e5e5; border-radius: 6px;
           padding: 1em 1.2em; }
  #combined-wrap { padding-bottom: 1.5em; }
  #combined-canvas { max-height: 620px; }

  #legend { display: flex; flex-wrap: wrap; gap: 4px 10px;
            margin-top: 1em; font-family: ui-monospace, Menlo, monospace;
            font-size: 0.82em; }
  #legend .item { display: inline-flex; align-items: center; gap: 6px;
                  cursor: pointer; padding: 2px 7px; border-radius: 3px;
                  user-select: none; line-height: 1.4;
                  transition: background .1s, opacity .15s, font-weight .05s; }
  #legend .item:hover,
  #legend .item.hovered { background: #eef3fb; font-weight: 600; }
  #legend .item.dimmed { opacity: 0.35; }
  #legend .item.dimmed:hover,
  #legend .item.dimmed.hovered { opacity: 0.55; }
  #legend .swatch { width: 11px; height: 11px; border-radius: 2px;
                    flex-shrink: 0; }
</style>
</head>
<body>
<h1>ulog bench trends</h1>
<p class="sub">__SUBTITLE__</p>

<div id="combined-wrap" class="chart">
  <div class="controls">
    <span><strong>Y scale:</strong></span>
    <label><input type="radio" name="mode" value="norm" checked> normalized (% of first)</label>
    <label><input type="radio" name="mode" value="log"> absolute, log</label>
    <label><input type="radio" name="mode" value="linear"> absolute, linear</label>
    <span style="margin-left:1em"><strong>Metric:</strong></span>
    <label><input type="radio" name="metric" value="real_time" checked> real_time</label>
    <label><input type="radio" name="metric" value="cpu_time"> cpu_time</label>
    <span style="margin-left:1em"></span>
    <button id="btn-all">all on</button>
    <button id="btn-none">all off</button>
    <span style="color:#666">(click legend item to dim, hover line / item to focus)</span>
  </div>
  <canvas id="combined-canvas"></canvas>
  <div id="legend"></div>
</div>

<script>
const SERIES = __DATA__;
const NAMES = Object.keys(SERIES);

function colorFor(i, n) {
  // Evenly spaced hues; skip the yellow band by offsetting.
  const hue = (i * 360 / n + 20) % 360;
  return `hsl(${hue}, 65%, 45%)`;
}

// Union of shas across every series, chronologically sorted.
function buildLabels(series) {
  const shaDate = new Map();
  for (const pts of Object.values(series)) {
    for (const p of pts) shaDate.set(p.sha, p.date);
  }
  return [...shaDate.entries()]
    .sort((a, b) => a[1] < b[1] ? -1 : (a[1] > b[1] ? 1 : 0))
    .map(e => e[0]);
}
const LABELS = buildLabels(SERIES);

const COLORS = NAMES.map((_, i) => colorFor(i, NAMES.length));

function hslWithAlpha(hsl, a) {
  // 'hsl(H, S%, L%)' -> 'hsla(H, S%, L%, a)'
  return hsl.replace('hsl(', 'hsla(').replace(')', ', ' + a + ')');
}

function datasetsFor(mode, metric) {
  return NAMES.map((name, i) => {
    const pts = SERIES[name];
    const bySha = Object.fromEntries(pts.map(p => [p.sha, p]));
    const baseline = pts[0] ? pts[0][metric] : 1;
    const data = LABELS.map(sha => {
      const p = bySha[sha];
      if (!p) return null;
      const v = p[metric];
      if (mode === 'norm') return baseline ? (v / baseline * 100) : null;
      return v;
    });
    return {
      label: name,
      data,
      borderColor: COLORS[i],
      backgroundColor: COLORS[i],
      tension: 0.2,
      pointRadius: 2.5,
      borderWidth: 1.5,
      spanGaps: true,
    };
  });
}

const ctx = document.getElementById('combined-canvas').getContext('2d');
let hoveredIdx = null;

let combined = new Chart(ctx, {
  type: 'line',
  data: { labels: LABELS, datasets: datasetsFor('norm', 'real_time') },
  options: {
    responsive: true,
    interaction: { mode: 'nearest', intersect: false, axis: 'xy' },
    onHover: (_, elements) => {
      const idx = (elements && elements.length) ? elements[0].datasetIndex : null;
      if (idx !== hoveredIdx) setHover(idx);
    },
    plugins: {
      legend: { display: false },
      tooltip: {
        callbacks: {
          title: (items) => items[0].label,
          label: (item) => {
            const v = item.parsed.y;
            if (v == null) return item.dataset.label + ': —';
            const suffix = currentMode() === 'norm' ? '%' :
              (SERIES[item.dataset.label][0]?.unit || '');
            return item.dataset.label + ': ' + v.toFixed(2) + ' ' + suffix;
          },
        },
      },
    },
    scales: {
      x: { title: { display: true, text: 'commit (chronological)' } },
      y: {
        type: 'linear',
        title: { display: true, text: '% of first commit (lower = better)' },
      },
    },
  },
});

// ---- custom HTML legend --------------------------------------------
const legendRoot = document.getElementById('legend');
const legendItems = NAMES.map((name, i) => {
  const item = document.createElement('span');
  item.className = 'item';
  item.dataset.idx = i;
  item.innerHTML = '<span class="swatch" style="background:' + COLORS[i] +
                   '"></span>' + name;
  item.addEventListener('click', () => {
    item.classList.toggle('dimmed');
    applyStyles();
  });
  item.addEventListener('mouseenter', () => setHover(i));
  item.addEventListener('mouseleave', () => setHover(null));
  legendRoot.appendChild(item);
  return item;
});

function isDimmed(i) { return legendItems[i].classList.contains('dimmed'); }

function setHover(i) {
  hoveredIdx = i;
  legendItems.forEach((el, k) => el.classList.toggle('hovered', k === i));
  applyStyles();
}

function applyStyles() {
  combined.data.datasets.forEach((ds, i) => {
    const dim = isDimmed(i);
    const focus = (hoveredIdx != null) && (hoveredIdx === i);
    const defocus = (hoveredIdx != null) && (hoveredIdx !== i);
    let alpha = 1.0;
    if (dim) alpha = 0.12;
    else if (defocus) alpha = 0.30;
    ds.borderColor = alpha === 1.0 ? COLORS[i] : hslWithAlpha(COLORS[i], alpha);
    ds.backgroundColor = ds.borderColor;
    ds.borderWidth = focus ? 3 : 1.5;
    ds.pointRadius = dim ? 0 : (focus ? 4 : 2.5);
    ds.order = focus ? 0 : (dim ? 999 : 100);  // focused on top, dimmed behind
  });
  combined.update('none');
}

function currentMode()   { return document.querySelector('input[name=mode]:checked').value; }
function currentMetric() { return document.querySelector('input[name=metric]:checked').value; }

function rebuild() {
  const mode = currentMode();
  const metric = currentMetric();
  combined.data.datasets = datasetsFor(mode, metric);
  combined.options.scales.y.type = (mode === 'log') ? 'logarithmic' : 'linear';
  combined.options.scales.y.title.text =
    mode === 'norm' ? '% of first commit (lower = better)' :
    (metric + ' (mixed units, lower = better)');
  applyStyles();  // re-apply dim/hover after dataset swap
}

for (const el of document.querySelectorAll('input[name=mode], input[name=metric]')) {
  el.addEventListener('change', rebuild);
}
document.getElementById('btn-all').onclick = () => {
  legendItems.forEach(it => it.classList.remove('dimmed'));
  applyStyles();
};
document.getElementById('btn-none').onclick = () => {
  legendItems.forEach(it => it.classList.add('dimmed'));
  applyStyles();
};
</script>
</body>
</html>
"""


def _list_bench_artifacts(repo: str, limit: int) -> list[tuple[str, str, str]]:
    """Return [(sha, run_id, created_at)] for non-expired bench artifacts.

    Walks /actions/artifacts until `limit` unique SHAs are collected or
    the listing is exhausted. Artifact names follow ``ulog-bench-<sha>``.
    """
    seen: dict[str, tuple[str, str]] = {}
    page = 1
    per_page = 100
    # Cap pagination to avoid unbounded scans on a large repo.
    while len(seen) < limit and page <= 10:
        out = _gh(
            "api",
            f"/repos/{repo}/actions/artifacts?per_page={per_page}&page={page}",
            "-q",
            ".artifacts[] | "
            f'select(.name | startswith("{ARTIFACT_PREFIX}")) | '
            "select(.expired == false) | "
            "[.name, (.workflow_run.id | tostring), .created_at] | @tsv",
        )
        rows = [ln for ln in out.strip().splitlines() if ln]
        if not rows:
            break
        for line in rows:
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            name, run_id, created = parts
            sha = name[len(ARTIFACT_PREFIX):]
            # Keep the freshest artifact per sha (listing is newest-first).
            if sha not in seen:
                seen[sha] = (run_id, created)
        page += 1
    items = [(sha, rid, ts) for sha, (rid, ts) in seen.items()]
    # Sort chronological ascending.
    items.sort(key=lambda x: x[2])
    return items[-limit:]


def cmd_trends(args: argparse.Namespace) -> int:
    repo = _repo_slug()
    items = _list_bench_artifacts(repo, args.limit)
    if not items:
        sys.exit("error: no ulog-bench-* artifacts found (check retention / auth)")

    # bench_name -> chronological list of points
    series: dict[str, list[dict[str, Any]]] = {}
    for sha, run_id, created in items:
        path = CACHE_DIR / f"{sha}.json"
        if not path.exists():
            print(f"downloading {ARTIFACT_PREFIX}{sha[:12]} ...")
            try:
                _download_artifact(repo, run_id, sha, path)
            except SystemExit as e:
                print(f"  skip {sha[:12]}: {e}")
                continue
        try:
            js = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            print(f"  skip {sha[:12]}: bench.json not parseable")
            continue
        idx = _index_bench(js)
        for name, b in idx.items():
            series.setdefault(name, []).append({
                "sha": sha[:12],
                "date": created,
                "real_time": round(float(b.get("real_time", 0.0)), 4),
                "cpu_time": round(float(b.get("cpu_time", 0.0)), 4),
                "iters": int(b.get("iterations", 0)),
                "unit": b.get("time_unit", ""),
            })

    if not series:
        sys.exit("error: no benchmarks parsed from downloaded artifacts")

    # Sort each series by date for correct x-axis order.
    for pts in series.values():
        pts.sort(key=lambda p: p["date"])

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    out_path = CACHE_DIR / "trends.html"
    subtitle = (
        f"{len(items)} commits, {len(series)} benchmarks. "
        f"Hover a point for sha / date / iters."
    )
    data_json = json.dumps(series, ensure_ascii=False).replace("</", "<\\/")
    html = _HTML_TEMPLATE.replace("__SUBTITLE__", subtitle).replace(
        "__DATA__", data_json
    )
    out_path.write_text(html, encoding="utf-8")
    print(f"wrote {out_path}  ({len(items)} commits, {len(series)} benches)")

    if not args.no_open:
        webbrowser.open(out_path.resolve().as_uri())
    return 0


# ---------------------------------------------------------------- csv
# Emitted columns. Long format — one row per (commit, benchmark) — is the
# friendliest shape for downstream agent / pandas / DuckDB analysis.
_CSV_FIELDS = [
    "sha", "short_sha", "artifact_date",
    "bench_name", "run_type", "aggregate_name",
    "real_time", "cpu_time", "time_unit", "iterations",
    "bytes_per_second", "items_per_second",
    "host_name", "num_cpus", "mhz_per_cpu", "library_build_type",
]


def cmd_csv(args: argparse.Namespace) -> int:
    repo = _repo_slug()
    items = _list_bench_artifacts(repo, args.limit)
    if not items:
        sys.exit("error: no ulog-bench-* artifacts found (check retention / auth)")

    rows: list[dict[str, Any]] = []
    for sha, run_id, created in items:
        path = CACHE_DIR / f"{sha}.json"
        if not path.exists():
            print(f"downloading {ARTIFACT_PREFIX}{sha[:12]} ...")
            try:
                _download_artifact(repo, run_id, sha, path)
            except SystemExit as e:
                print(f"  skip {sha[:12]}: {e}")
                continue
        try:
            js = json.loads(path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            print(f"  skip {sha[:12]}: bench.json not parseable")
            continue
        ctx = js.get("context", {})
        idx = _index_bench(js)
        for name, b in idx.items():
            rows.append({
                "sha": sha,
                "short_sha": sha[:12],
                "artifact_date": created,
                "bench_name": name,
                "run_type": b.get("run_type", ""),
                "aggregate_name": b.get("aggregate_name", ""),
                "real_time": b.get("real_time", ""),
                "cpu_time": b.get("cpu_time", ""),
                "time_unit": b.get("time_unit", ""),
                "iterations": b.get("iterations", ""),
                "bytes_per_second": b.get("bytes_per_second", ""),
                "items_per_second": b.get("items_per_second", ""),
                "host_name": ctx.get("host_name", ""),
                "num_cpus": ctx.get("num_cpus", ""),
                "mhz_per_cpu": ctx.get("mhz_per_cpu", ""),
                "library_build_type": ctx.get("library_build_type", ""),
            })

    if not rows:
        sys.exit("error: no benchmark rows collected")

    # Sort chronologically then by bench name for stable diffs.
    rows.sort(key=lambda r: (r["artifact_date"], r["bench_name"]))

    out = Path(args.output) if args.output else (CACHE_DIR / "bench.csv")
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", encoding="utf-8", newline="") as f:
        w = csv.DictWriter(f, fieldnames=_CSV_FIELDS)
        w.writeheader()
        w.writerows(rows)

    unique_benches = len({r["bench_name"] for r in rows})
    print(
        f"wrote {out}  "
        f"({len(rows)} rows, {len(items)} commits, {unique_benches} benches)"
    )
    return 0


# ---------------------------------------------------------------- main
def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="list recent ulog-ci runs")
    pl.add_argument("--limit", type=int, default=20)
    pl.add_argument("--branch", default="main")
    pl.set_defaults(func=cmd_list)

    pf = sub.add_parser("fetch", help="download bench.json for a commit")
    pf.add_argument("ref", help="git ref, SHA, or 'latest'")
    pf.add_argument("--force", action="store_true", help="re-download if cached")
    pf.set_defaults(func=cmd_fetch)

    ps = sub.add_parser("show", help="print bench table for a commit")
    ps.add_argument("ref", nargs="?", default="HEAD")
    ps.set_defaults(func=cmd_show)

    pc = sub.add_parser("compare", help="diff two bench runs")
    pc.add_argument("base", help="baseline ref ('HEAD~1', SHA, 'latest')")
    pc.add_argument("head", help="target ref ('HEAD', SHA, 'latest')")
    pc.add_argument(
        "--threshold",
        type=float,
        default=5.0,
        help="regression threshold in %% (default 5.0)",
    )
    pc.set_defaults(func=cmd_compare)

    pt = sub.add_parser("trends", help="HTML charts of all bench runs")
    pt.add_argument(
        "--limit",
        type=int,
        default=50,
        help="max commits to include (default 50)",
    )
    pt.add_argument(
        "--no-open",
        action="store_true",
        help="do not auto-open the HTML in a browser",
    )
    pt.set_defaults(func=cmd_trends)

    pcsv = sub.add_parser("csv", help="export bench data to CSV (long format)")
    pcsv.add_argument(
        "--limit",
        type=int,
        default=200,
        help="max commits to include (default 200)",
    )
    pcsv.add_argument(
        "--output",
        "-o",
        default=None,
        help="output path (default .bench-cache/bench.csv)",
    )
    pcsv.set_defaults(func=cmd_csv)

    args = p.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
