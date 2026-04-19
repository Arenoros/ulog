# Review 25 — Phase 35 (Google Benchmark in CI)

Scope: close the BACKLOG "Google Benchmark в CI" item — build and run `ulog-bench` on every CI run, upload the JSON snapshot as a retained artifact. The "optionally compare with baseline" half of the BACKLOG line is intentionally deferred; the artifact-per-run is the prerequisite it depends on.

## Changes

### `.github/workflows/ci.yml`

- New top-level job `bench` (peer of the matrix-based `build` job). Runs on a pinned `ubuntu-22.04` / gcc / Release target so at least one axis of the trend lines is stable; other platforms are free to be noisier.
- Steps:
  - Checkout, install Conan 2.14.0, force the v2 Conan Center remote (same pattern as the `build` job so a single cache logic works).
  - `actions/cache@v4` keyed identically to the Release-gcc-cpp17 row in `build`. The bench job can warm-start from the matrix cache and vice versa, so the extra job doesn't double the cold-cache cost.
  - Install Ninja, `conan install`, CMake configure with `-DULOG_BUILD_BENCH=ON -DULOG_BUILD_TESTS=OFF -DULOG_BUILD_EXAMPLES=OFF` — only the benchmark binary builds.
  - `cmake --build build --target ulog-bench` — narrow target, skips tests + examples + wider library consumers.
  - Run `ulog-bench` with `--benchmark_out=bench.json --benchmark_out_format=json --benchmark_min_time=0.3s`. Console output stays tabular so the CI log is readable; the JSON snapshot is the artifact.
  - `actions/upload-artifact@v4` with `name: ulog-bench-${{ github.sha }}`, 90-day retention. SHA-suffixed name means every run gets a unique, traceable artifact that lines up with the commit it measured.

### Min-time choice

`--benchmark_min_time=0.3s` is under the Google Benchmark default (0.5s) to keep the job under a couple of minutes. We have roughly a dozen benchmarks, each running at ~0.3 s plus warm-up and metadata, so end-to-end the bench job lands in the 1-2 minute range — well inside the matrix runtime budget.

### No baseline comparison

`benchmark_compare` tools expect a named baseline file on disk, which means either (a) committing a baseline into the repo (hand-maintained and lies when the runner changes), or (b) pulling the prior run's artifact via the GH Actions API (permission + retention dance). Neither is a phase-35 problem — this phase gets the raw data into CI so the follow-up can build on it.

## Results

- **YAML validates** (no indentation drift; keyed the cache to match the existing `build.cache` entry so the restore-keys chain from the matrix rows applies).
- **Local smoke test**: `./build/bench/ulog-bench --benchmark_out=/tmp/_bench.json --benchmark_out_format=json --benchmark_min_time=0.05s --benchmark_filter='BM_SyncLoggerThroughput'` produced a well-formed JSON file (context block + benchmarks array), console output intact.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Shared-runner variance.** GH Actions Linux runners are 2-vCPU shared instances with hypervisor noise — absolute numbers will wobble 10-20% between runs. The artifact is useful for trend-tracking, not for regression-gating. Any future compare-with-baseline job must set a generous threshold or pin to a self-hosted runner.
- **The bench cache is keyed to the Release-gcc-cpp17 slice.** A new conanfile.txt bump would cause a cold rebuild of `fmt`, `boost`, `benchmark`, `gtest` for this job; same cost as the matrix row. Not reducible until we share a single Conan cache across jobs within a workflow (possible via a combined `cache` action step, deferred).
- **No `sudo apt-get install cpufreq-set`** or `cpupower frequency-set --governor performance`. Shared-runner kernels don't expose those knobs in a consistent way; there's nothing to tune.
- **No artifact retention rotation.** 90 days is a reasonable default for post-mortem / trend review; we'll revisit if storage becomes a concern.

### Test coverage
- The CI job itself is the test surface — verification happens once the workflow runs on a real pull request. The bench binary's regression story already lives in the local benchmarks that run on every developer's machine.

### Deferred to BACKLOG
- Artifact comparison job: fetch the prior run's `ulog-bench-*.json`, run `benchmark_compare.py`, post a PR comment if any benchmark regressed by more than N%. Needs either a stored baseline per branch or a GH Actions permission to read workflow artifacts from the base commit.
- Self-hosted bench runner with a pinned CPU governor, once the number of bench-sensitive changes justifies it.

## Acceptance: ✅ commit + advance
