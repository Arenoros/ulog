# Review 28 — Phase 38 (per-sink format overrides)

Scope: add `AddSink(SinkPtr, Format)` overload on `SyncLogger` / `AsyncLogger`, letting a single logger fan out each record into multiple concurrent formats (e.g. TSKV to a file + JSON to a remote collector from the same `LOG_*` call).

## Changes

### API

- `TextLoggerBase` owns an append-only list of active formats (base at index 0; each distinct override extends the list). `RegisterSinkFormat(std::optional<Format>)` returns the stable index for a sink; duplicate formats dedupe to the existing index.
- `LogHelper::Impl` materializes one formatter per active format — primary in the inline scratch (base format), extras heap-allocated. A local `FanoutFormatter` broadcasts every `AddTag` / `SetTraceContext` call from `TagWriter` / tracing hooks to all targets. `SetText` mirrors through a one-shot loop in the dtor.
- New `LoggerBase::LogMulti(level, LogItemList)` virtual. Default implementation routes the first item to the legacy `Log(unique_ptr)` so non-text loggers keep working untouched. `LogItemList` is `boost::container::small_vector<unique_ptr<LoggerItemBase>, 1>` — single-format fast path allocates nothing extra.
- `AsyncLogger` queue element now carries `LogItemList` rather than a single `unique_ptr`. Worker drain walks sinks per record and routes by `format_idx`.

### Plumbing points

- `src/log_helper.cpp:90–120` — Impl ctor snapshots `GetActiveFormats()` once, materializes primary + extras, wires the fanout sink for multi-format writers. Single-format path is unchanged.
- `src/sync_logger.cpp:30–60` / `src/async_logger.cpp:170–200` — sink entries now carry `format_idx`; dispatch picks `items[format_idx]`.
- `include/ulog/impl/logger_base.hpp:150–190` — `TextLoggerBase` registration API + cached format list under `formats_mu_`.

### Tests

11 tests in `tests/per_sink_format_test.cpp` — TSKV+JSON mixed, same-format dedup, three distinct formats (sync + async batched 5-record), LogExtra tag fan-out, async multi-format, single-format regression, `Format::kRaw` override, per-sink level gate combined with format override, logger-level filter dropping the whole record, duplicate override dedup.

## Findings (self-review pass)

### Addressed

- **[CRITICAL] Race between `GetActiveFormatCount()` and `GetActiveFormats()` in LogHelper.** Previously two separate reads; a concurrent `AddSink` could insert a format between them leaving the LogHelper with fewer formatters than the sink list later demands. Fix: snapshot once via `GetActiveFormats()` and branch on `formats.size() > 1` (src/log_helper.cpp:101–120).
- **[IMPORTANT] Silent fallback to `items[0]` when `format_idx` overshoots.** The original `idx = entry.format_idx < items.size() ? entry.format_idx : 0` masked the "sink registered after record materialization" race by writing the wrong-format payload. Replaced with explicit skip (`if (entry.format_idx >= items.size()) continue;`) in both sync and async dispatch. Sinks added mid-log are invisible to in-flight records — documented in the LogHelper ctor comment.
- **[TEST GAPS]** added:
  - `SyncRawFormatOverride` — verifies kRaw override strips header fields while base TSKV keeps them.
  - `AsyncThreeDistinctFormatsBatched` — 5 records through moodycamel bulk dequeue with 3 distinct formats, asserts per-record per-format content.
  - `PerSinkLevelGateWithFormatOverride` — sink.SetLevel(kError) filters INFO while TSKV sink keeps both.
  - `LoggerLevelFilterSkipsEntireRecord` — logger-level `kError` gate drops whole record for every sink (no fanout path entered).
  - `DuplicateOverrideFormatReusesIndex` — two sinks with identical override share the single extra formatter, payloads byte-equal.

### Open / acceptable

- **Nullptr extracted item (OOM upstream)** — silently dropped via `if (!text) continue;` in both dispatchers. Explicit comment now present; no counter surfaced. Rationale: the only way to get a null item is formatter-side allocation failure, which is already squelched by the outer `try { ... } catch (...)` in LogHelper's dtor. A future observability knob could expose a drop counter (parallel to the existing `GetDroppedCount()` for queue overflow) — out of scope here.
- **`FanoutFormatter::ExtractLoggerItem` returns nullptr** — intentional: LogHelper extracts directly from primary + extras. Commented in the class body.
- **`SyncLogger::Log` is a thin wrapper over `LogMulti`** — kept for binary/source compat with subclasses that implement `Log` but not `LogMulti`. The default `LogMulti` in `LoggerBase` routes to `Log` via move of `items[0]` — non-text loggers (NullLogger) need no changes.
- **Mid-log `AddSink` index stability** — sink added *after* a record's `LogHelper` snapshot is invisible to that record. LogMulti's out-of-range skip is the intended handling. Documented in the ctor comment.

## Test coverage status

130/130 tests pass (11 new + 119 prior). No regressions.

- [x] Mixed formats on same logger (sync)
- [x] Mixed formats batched (async)
- [x] Dedup: same format override as base → index 0
- [x] Dedup: two sinks sharing a non-base override → single extra formatter
- [x] `Format::kRaw` override
- [x] Per-sink level gate + format override interact correctly
- [x] Logger-level filter drops record with multi-format configured
- [x] LogExtra tags fan out to every format
- [x] Three distinct formats in one record
- [x] Single-format regression path unchanged
