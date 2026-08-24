# Offline baseline corpus

Issue: [#3](https://github.com/Arenoros/ulog/issues/3)

The committed corpus is diagnostic evidence captured from the exact userver
revision in [the migration baseline](baseline.md). Regular Ulog builds validate
this evidence without locating, building, or executing userver. Capture is a
manual migration workflow and is never part of CMake configuration, Conan
dependency resolution, installation, or CI.

## Artifact layout

- `docs/migration/corpus/*.json` contains committed, versioned corpus files.
- `scripts/baseline_corpus.py capture` verifies a checkout and seals probe output.
- `scripts/baseline_corpus.py validate` performs the regular offline check.
- [The capability manifest](capability-manifest.md) is the source of valid feature
  and deliberate-difference IDs.

## Capture workflow

Build a purpose-specific probe against the pinned baseline on a host supported
by that revision, then invoke it through the capture tool:

```text
python scripts/baseline_corpus.py capture \
  --baseline-source /absolute/path/to/userver \
  --output docs/migration/corpus/baseline-v1.json \
  --force \
  -- /absolute/path/to/baseline-probe [probe arguments]
```

`--baseline-source` has no default and the tool never clones or fetches. Before
running the probe it requires:

- `HEAD` to equal `72e07f717ae46a17822776df21ebd73dbc4ce728`;
- no tracked modifications or untracked, non-ignored files;
- an explicit probe command; and
- `--force` before replacing existing evidence.

After the probe exits, the tool checks the revision and worktree again. It also
requires the probe's build-time baseline attestation to match the same repository
and revision. The generated corpus is validated against the canonical capability
manifest before it can replace committed evidence. Without `--force`, final
publication fails atomically if another process creates the output while capture
is running.

The probe runs with the checkout as its working directory and with `LC_ALL=C`,
`LANG=C`, and `TZ=UTC`. It must write one strict UTF-8 JSON object to stdout.
Build probes out of tree. In particular, do not configure the baseline directly
in the verified checkout because its build can generate source-tree files; use a
temporary `git archive` snapshot when a probe needs to build userver itself.

The pinned baseline does not support a native Windows build. Real capture must
therefore run on a supported Linux or macOS host (or an equivalent VM). The
committed-corpus validator remains platform-independent and runs on Windows.

## Probe envelope

Probe schema version 1 is independent of the committed schema. Each case names
the preserved feature IDs, any relevant `DEF-*` or `DIFF-*` IDs, its platform
scope, the observed value, and only the volatile occurrences that must change.
The top-level `baseline` values must be generated into the probe at build time
from the same source snapshot used for its include and link paths. Do not infer
them from the capture process's working directory or copy them manually: that
would allow an accidentally stale probe to misidentify its observations.

```json
{
  "probe_schema_version": 1,
  "baseline": {
    "repository": "userver",
    "revision": "72e07f717ae46a17822776df21ebd73dbc4ce728"
  },
  "cases": [
    {
      "id": "tskv-basic",
      "feature_ids": ["FMT-002", "VAL-006"],
      "difference_ids": [],
      "platform": "portable",
      "observed": {
        "kind": "utf8",
        "format": "tskv",
        "value": "tskv\ttimestamp=2026-08-24T09:30:12.123456\ttext=test\n"
      },
      "normalization": [
        {
          "kind": "timestamp_local",
          "path": "/value",
          "value": "2026-08-24T09:30:12.123456",
          "occurrence": 0
        }
      ]
    }
  ]
}
```

Case IDs use lowercase hyphenated words. `feature_ids` is non-empty;
`difference_ids` may be empty. The platform is one of `portable`, `windows`,
`linux`, or `macos`. The normalization list is exhaustive for the case: every
volatile timestamp, source path, process ID, thread ID, and platform occurrence
must have a rule. An empty list is an explicit assertion by the probe author that
the observed payload is already deterministic.

## Deterministic normalization

Normalization is fail-closed and occurrence-targeted. `path` is a non-root JSON
Pointer selecting one observed string. `occurrence` is the zero-based,
non-overlapping occurrence of the exact supplied value in that string. The tool
rejects a missing occurrence, an invalid pointer, or overlapping edits. It never
searches unrelated fields, so a process ID or platform word in user message text
is not changed accidentally.

The version 1 replacements are:

| Kind | Required input | Committed replacement |
| --- | --- | --- |
| `timestamp_local` | `YYYY-MM-DDTHH:MM:SS.ffffff` | `2000-01-02T03:04:05.123456` |
| `timestamp_utc` | Same form with final `Z` | `2000-01-02T03:04:05.123456Z` |
| `source_path` | Non-empty exact path occurrence | `<source-path>` |
| `process_id` | Non-empty exact ID occurrence | `<process-id>` |
| `thread_id` | Non-empty exact ID occurrence | `<thread-id>` |
| `platform` | Non-empty exact platform occurrence | `<platform>` |

The fixed timestamp retains the six-microsecond and UTC-suffix contracts rather
than erasing their shape. Platform-specific behavior belongs in a separate case
whose `platform` field names that platform; normalization must not disguise a
real behavioral difference.

## Committed schema and integrity

Committed schema version 1 has exactly four top-level fields:
`schema_version`, `provenance`, `cases`, and `integrity`. Provenance records the
baseline document, repository, full revision, capture-tool version, and
normalization-profile version. Cases are sorted by ID; both ID lists are unique
and sorted, and every ID must exist in the capability manifest. Corpus filenames
must end in lowercase `.json`, ensuring case-sensitive and case-insensitive hosts
validate the same fixture set.

Integrity uses `sha256` and canonicalization `ulog-json-v1`:

1. Parse strict UTF-8 JSON without a BOM, duplicate keys, floating JSON numbers,
   `NaN`, or infinities.
2. Copy the document and remove only `integrity.value`; keep the algorithm and
   canonicalization fields in the signed material.
3. Serialize UTF-8 JSON with object keys sorted, array order preserved, no ASCII
   escaping, and compact `,`/`:` separators.
4. Store the lowercase 64-digit SHA-256 value.

This detects accidental fixture edits. It is not intended to authenticate a
maliciously rewritten corpus whose digest was deliberately recomputed.

## Regular validation

The normal check uses committed files only:

```text
python scripts/baseline_corpus.py validate \
  --corpus-root docs/migration/corpus \
  --manifest docs/migration/capability-manifest.md
```

CTest exposes the same operation as `ulog.corpus`. An empty corpus, unexpected
file type, schema drift, provenance drift, unknown ID, or integrity mismatch is
a failure. The test does not invoke Git and succeeds when no external checkout
or capture dependency is available.
