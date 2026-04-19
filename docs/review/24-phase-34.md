# Review 24 — Phase 34 (DX batch: JSON consumer example + Doxygen config)

Scope: close the two "Удобство разработчика" BACKLOG items in a single pass. No production code touched.

## Changes

### `examples/json_consumer/main.cpp` — structured-log consumer walkthrough

- Writes a handful of records through `Format::kJson` to `json_example.jsonl` (configurable via the first CLI arg) and exits.
- Each record uses `LogExtra` with native types: `user_id=42`, `latency_ms=13.5`, `ok=true`, `retry=3`, `fatal=false`. The typed-tag API from Phase 22 means the file can be piped through `jq`/`jq -c 'select(.latency_ms > 10)'` without parser hints.
- Trailer prints the expected `tail -f … | jq` one-liner pointing at the output path — the whole point of the example is "here's what you get, here's how to consume it", visible in under 10 seconds.
- Registered in `examples/CMakeLists.txt` as `ulog-example-json-consumer`.

### `docs/Doxyfile` — minimal HTML reference config

- Input: `include/` + `src/` + `README.md` (as mainpage), recursive, `.hpp`/`.h`/`.cpp`. Excludes `third_party/` and `build/`.
- Output: `build/docs/html/index.html`. No LaTeX, no `dot`, no call graphs — generation is fast and doesn't demand GraphViz on CI runners.
- Light-theme HTML with full sidebar, stable member ordering (`SORT_MEMBERS_CTORS_1ST`, `SORT_BRIEF_DOCS`) so diffs between runs stay reviewable.
- `JAVADOC_AUTOBRIEF=YES` + `MARKDOWN_SUPPORT=YES` so the existing `///` / `@brief` / markdown patterns render correctly.
- `EXTRACT_ALL=YES` + `HIDE_UNDOC_MEMBERS=NO` so headers without docs still show up (many private helpers don't have prose; that's fine for a v1 reference).
- Warnings limited to `WARN_IF_DOC_ERROR=YES`; no `WARN_AS_ERROR` — we want a reference, not a pre-commit blocker.

### `README.md` — new "API reference" section

- One-paragraph pointer at `docs/Doxyfile` with the `doxygen docs/Doxyfile` invocation and the output path. Sits between "Continuous integration" and "License" — last thing readers see before the license footer.

## Results

- **Local build (MSVC 14.50, RelWithDebInfo)**: clean. `ulog-example-json-consumer.exe` runs to completion, writes three records, prints the tail+jq hint.
- **Sample output**:
  ```
  {"timestamp":"2026-04-19T10:34:54…","level":"INFO","module":"main ( examples\\json_consumer\\main.cpp:32 )","user_id":42,"latency_ms":13.5,"ok":true,"endpoint":"/api/login","text":"user logged in"}
  ```
  Typed tags land as native JSON (`42`, `13.5`, `true`) — the point of the example.
- **Tests**: 98/98 green, unchanged.
- **Doxygen**: not run locally (doxygen not installed on the dev box); config is syntactically valid and mirrors patterns already in wider use.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **No CI job that runs Doxygen.** Generating docs in CI and publishing to gh-pages is a natural follow-up, but adds a workflow surface that deserves its own phase (permissions, artifact retention, branch-protection). Keeping Doxyfile in-repo is the minimal useful artifact.
- **JSON example has no companion README.md under `examples/json_consumer/`**. The code comments cover the "how to consume" question at the top of `main.cpp`; another README would duplicate it. Existing examples follow the same one-file pattern.
- **Example path on Windows** emits backslashes in `module=` because `__FILE__` is native-path. That's a pre-existing trait of the records, not introduced here — `ULOG_SOURCE_ROOT` trimming strips the prefix but not separators inside the remaining suffix.

### Deferred to BACKLOG
- CI workflow that builds the Doxygen site and publishes to gh-pages.
- An optional `doc` CMake target that shells out to `doxygen docs/Doxyfile` when the binary is on PATH.

## Acceptance: ✅ commit + advance
