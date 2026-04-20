# Review 29 — Phase 39 (structured sink API)

Scope: parallel sink interface that receives a raw log record (level, text, tags as native-typed variants, module location, trace/span ids) instead of a pre-rendered text payload. Enables sinks that emit to natively-structured downstream systems (OTLP protobuf, cloud logging APIs, fluent-bit native input) without re-parsing string intermediates.

## Design

- **`sinks::StructuredSink`** — new interface parallel to `BaseSink`. `Write(const sinks::LogRecord&)` consumes the record; level gate / `Flush` / `Reopen` mirror `BaseSink`. Independent hierarchy — does not inherit `BaseSink`.
- **`sinks::LogRecord`** — owning struct (strings are `std::string`, tags a vector). Survives the async queue round-trip without view-lifetime concerns.
- **`sinks::TagValue`** — `std::variant<std::string, int64_t, uint64_t, double, bool, JsonString>`. Preserves the typed form the caller supplied via `AddTagInt64` / `AddTagBool` / `AddJsonTag`.
- **LogHelper decides the path at ctor** — `LoggerBase::HasTextSinks()` / `HasStructuredSinks()` gate whether to materialize a text formatter, a `TagRecorder`, or both. Zero overhead when no structured sink is attached (fast path unchanged).
- **`TagRecorder` (private, in `log_helper.cpp`)** — a `formatters::Base` subclass that captures tags/text/trace-context into a `sinks::LogRecord`. Joins the existing fanout alongside text formatters when the logger has mixed sink types.
- **Unified dispatch** — `LogMulti(level, items, structured)` is the one entry point LogHelper calls, so `AsyncLogger` packs both halves into a single `QueueRecord` and preserves one-record-one-enqueue semantics. Worker fans out text items by `format_idx` and hands the `sinks::LogRecord` to every structured sink.

## Changes

- `include/ulog/sinks/structured_sink.hpp` — new public interface.
- `include/ulog/impl/logger_base.hpp` — `LogMulti` extended with `unique_ptr<sinks::LogRecord>` parameter (defaulted), `LogStructured` virtual, `HasTextSinks` / `HasStructuredSinks` queries.
- `include/ulog/sync_logger.hpp` + `src/sync_logger.cpp` — `AddStructuredSink`, separate `struct_sinks_` vector with its own mutex, `LogStructured` override, `Flush` now touches structured sinks too.
- `include/ulog/async_logger.hpp` + `src/async_logger.cpp` — `QueueRecord::structured`, `AddStructuredSink`, worker fan-out for structured path, `drain_reopens` / `drain_flushes` propagate to structured sinks.
- `src/log_helper.cpp` — `TagRecorder` class, `Impl` ctor branches on sink types, fanout wires formatter(s) + recorder when both present, dtor builds items + record and routes through `LogMulti`.
- `tests/structured_sink_test.cpp` — 14 tests covering the feature surface.

## Findings (self-review)

### Addressed

- **[IMPORTANT] `AsyncLogger::Flush` did not flush structured sinks.** The worker's `drain_flushes` + `drain_reopens` lambdas only touched text sinks. Now both snapshots are taken and both sink types are flushed / reopened. Regression test `AsyncFlushDrainsStructuredSinkFlushCount` asserts `Flush()` on a structured sink fires at least once after drain.
- **[IMPORTANT] `emit_location = false` was not honoured on the structured record.** The text formatter's location-field suppression is driven by `TextLoggerBase::GetEmitLocation()`; LogHelper now reads that same flag before seeding `module_function` / `module_file` / `module_line` on the `sinks::LogRecord`. Tests `EmitLocationFalseSuppressesModuleFields` / `EmitLocationTrueKeepsModuleFields` pin both shapes.
- **[TEST GAPS] closed:**
  - `MultipleStructuredSinksAllReceive` — three distinct sinks each get the record.
  - `JsonStringTagPreserved` — `std::variant<JsonString>` round-trips.
  - `ThrowingSinkDoesNotAffectSiblings` — one sink throws, another still fires.
  - `AsyncFlushWaitsForStructuredRecords` — 100 records, all drain before `Flush()` returns.
  - `AsyncFlushDrainsStructuredSinkFlushCount` — above.

### Open / acceptable

- **Mid-log `AddStructuredSink` race.** A sink added between `LogHelper` ctor and its dtor is invisible to the in-flight record — the same semantics as `AddSink` for text sinks. The LogHelper ctor snapshots `HasStructuredSinks()` once; there is no follow-up check. Documented at `src/log_helper.cpp` (ctor comment) and is consistent with the existing per-sink-format race handling.
- **`LogStructured` exists separately from `LogMulti`.** Some subclasses may override only `LogMulti`; the default `LogMulti` routes `structured` to `LogStructured`. SyncLogger / AsyncLogger override `LogMulti` AND `LogStructured`. If a future custom logger overrides only `LogMulti` and forgets to handle the structured half, records drop silently — mitigated by the sole-entry-point conventtion LogHelper uses (`LogMulti` always gets the structured piece when any structured sink is attached).
- **Null-target path in LogHelper dtor.** When a logger has no sinks at all (`want_text == false && want_structured == false`), `hook_target` stays null and `FormatterTagSink(nullptr)` no-ops the tracing / enricher calls. Intentional; `TagWriter` already guards on its null formatter pointer.
- **Member declaration order in `LogHelper::Impl`.** `recorder` precedes `fanout`, so the destruction order (reverse) tears fanout down first. The non-owning pointers inside fanout point at targets (`formatter`, `extras`, `recorder`) that all outlive it — safe.

### Remaining test gaps (not blockers)

- Multiple structured sinks with per-sink `SetLevel` filter (partial — only per-sink-gate for a single sink is covered).
- `AddStructuredSink` + `AddSink(sink, Format::kRaw)` interactions (kRaw produces no module; unclear whether it should suppress structured location too — current: no, emit_location governs both independently).

## Test coverage status

157 tests pass (14 new + 143 prior). No regressions in the 143 pre-existing tests.
