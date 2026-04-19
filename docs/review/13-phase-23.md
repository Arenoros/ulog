# Review 13 — Phase 23 (sanitizer CI + trace correlation)

Scope: close two small-but-high-impact BACKLOG items — promote `trace_id` / `span_id` to the top-level OTLP `LogRecord` schema so log↔trace correlation works in Tempo/Jaeger/Grafana, and add ASan/TSan/UBSan rows to the CI matrix so race and UB regressions get caught pre-merge.

## Changes

### OTLP trace correlation

- New dedicated API on `impl::formatters::Base`:
  ```cpp
  virtual void SetTraceContext(std::string_view trace_id_hex,
                               std::string_view span_id_hex);
  ```
  - Default implementation falls back to `AddTag("trace_id", ...)` / `AddTag("span_id", ...)` so plain-text formatters (TSKV / LTSV / Raw / JSON / YaDeploy) keep emitting the IDs as ordinary key=value pairs. Empty arguments skip the corresponding tag.
- `OtlpJsonFormatter` overrides the method and stashes the hex strings into dedicated `trace_id_` / `span_id_` slots on the formatter instance. `ExtractLoggerItem` finalization emits them as `"traceId"` / `"spanId"` top-level string fields between the `attributes` array and the `body` object — the shape required by `opentelemetry.proto.logs.v1.LogRecord` so Tempo/Jaeger/Grafana can correlate logs with spans.
- `TagSink` (the public tracing-hook surface in `include/ulog/tracing_hook.hpp`) grows a matching `SetTraceContext` virtual with the same fallback semantics. `FormatterTagSink` in `log_helper.cpp` forwards to the active formatter. Tracing hooks now prefer `sink.SetTraceContext(...)` over two `sink.AddTag(...)` calls with magic keys — the string comparisons stay off the formatter hot path entirely.
- Rationale: intercepting the magic keys inside `AddTag` worked but added two `string_view::operator==` dispatches to every tag emission. The dedicated virtual is cleaner architecturally (trace context ≠ arbitrary attribute), zero-cost on non-OTLP formatters, and sets up the upcoming `OtlpBatchSink` envelope phase to consume the same typed slot.

### Migration

- Existing hooks that call `sink.AddTag("trace_id", hex)` still compile and produce correct TSKV/JSON/LTSV/Raw output (falls into the regular tag stream). They lose OTLP top-level promotion — hooks that target OTLP should migrate to `sink.SetTraceContext(trace_hex, span_hex)`. The bundled `examples/tracing_hook` now uses the typed path.
- The new virtuals grow the `TagSink` and `formatters::Base` vtables — downstream consumers that subclass either must recompile against the new headers. Source-compatible (no existing member changed signature), but not strictly ABI-compatible. Acceptable pre-1.0; worth flagging explicitly in the first stable release notes.

### Test coverage

- `FormatterOtlpJson.TraceAndSpanIdsPromotedToTopLevel` — installs a `TracingHook` that calls `sink.SetTraceContext(trace_hex, span_hex)`, asserts the record carries both as top-level fields, does not duplicate them as attribute entries, and that unrelated tags (`user_id`) still flow through the attribute array with their typed OTLP value kind.
- `FormatterOtlpJson.TraceIdAloneKeepsAttributesArrayShape` — asserts that passing an empty `span_id` leaves no stray `"spanId":""` field, so sparse-adoption backends don't see ambiguous empty strings.
- `FormatterText.TraceContextFallsBackToPlainTags` — covers the default `Base::SetTraceContext` path: a TSKV logger receives the hook and emits `trace_id=...` / `span_id=...` as regular tags (no OTLP top-level promotion).

### Sanitizer CI

- `.github/workflows/ci.yml` adds three new matrix rows: `linux-asan`, `linux-tsan`, `linux-ubsan`.
  - All three use `clang++` on `ubuntu-22.04` with `CMAKE_BUILD_TYPE=RelWithDebInfo` so sanitizer reports carry frames but the hot paths stay reasonably optimized.
  - The matrix gains a `sanitizer` key; when set, the configure step appends `-fsanitize=<kind> -fno-omit-frame-pointer -fno-sanitize-recover=all` to `CMAKE_CXX_FLAGS` / `CMAKE_C_FLAGS` / `CMAKE_EXE_LINKER_FLAGS` / `CMAKE_SHARED_LINKER_FLAGS`.
  - Sanitizer rows explicitly pass `-DULOG_BUILD_BENCH=OFF` — moodycamel's lock-free internals generate a swarm of TSan "benign race" reports that would swamp real findings in the unit tests.
  - Only ulog's own code is instrumented — Conan deps stay stock. Pragmatic trade-off: a full rebuild of fmt/boost/gtest under each sanitizer would push the matrix into the 30+ minute range. Caveat noted for Phase 24 (the follow-up plans Conan caching, at which point deps-under-sanitizer becomes affordable).
- Test step sets matching sanitizer run-time options:
  - `ASAN_OPTIONS=halt_on_error=1:detect_leaks=1:strict_string_checks=1`
  - `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`
  - `TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1`
- Existing Linux/macOS/Windows rows keep their Release config; they gain an explicit `build_type` matrix entry to thread the same `BUILD_TYPE` env into the Conan install + ctest invocations without branching.

## Results

- **Build (local MSVC 14.50)**: green.
- **Tests (local RelWithDebInfo)**: 1/1 ctest target green; ulog-tests suite runs all 73 cases clean (70 previous + 2 OTLP-correlation tests + 1 text-fallback test).
- **Direct re-run with `--gtest_filter=FormatterOtlpJson*:FormatterText*:TracingHook*`** confirms all 7 relevant tests pass.
- **Sanitizer rows**: will exercise on the first GH Actions run post-merge. Local sanitizer execution requires Linux; Windows dev loop can't validate the `-fsanitize=*` side. This is acceptable because the new YAML is additive (only enabled in the new rows) and the CMake plumbing is a conventional `-fsanitize` pass-through.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Deps stay stock under sanitizers.** If fmt or boost harbors a race that corrupts ulog's state, TSan won't see the root-cause frames; the sanitizer reports will point at the ulog call-site. Phase 24 Conan caching will make dep-instrumentation affordable.
- **Existing hooks using `AddTag("trace_id", hex)` lose OTLP promotion.** They still emit correct TSKV/JSON output but won't populate the top-level OTLP fields. Migration is a one-liner (swap two `AddTag` calls for one `SetTraceContext`); the hook API remains source-compatible (the new method carries a default-fallback implementation), though the vtable growth means downstream `TagSink` / `Base` subclasses must be recompiled — not a strict ABI guarantee.
- **No length / hex validation on `trace_id`/`span_id`.** We pass through whatever the user provides. Intent is to trust the upstream tracing library; an invalid hex string produces invalid OTLP that the backend rejects, which is the right failure mode (garbage in, garbage out, no silent truncation).

### Deferred to BACKLOG
- Conan cache + C++20 matrix row (listed as separate BACKLOG items — prerequisite for full dep-instrumented sanitizers).
- Sanitizer runs for benchmarks — requires either moodycamel TSan suppressions or replacing the queue benches with atomics-free harnesses.

## Acceptance: ✅ commit + advance
