# Review 12 — Phase 22 (typed tag API + OTLP JSON formatter)

Scope: preserve native types from `LogExtra` all the way to the formatter boundary, and ship a concrete `Format::kOtlpJson` emitter that writes one `opentelemetry.proto.logs.v1.LogRecord` per line.

## Changes

### Typed tag API

- `impl::formatters::Base` grows four virtuals with stringifying defaults:
  - `AddTagInt64 / AddTagUInt64 / AddTagDouble / AddTagBool`.
  - Default implementations call `AddTag(key, fmt::format("{}", v))` — text formatters that haven't overridden keep emitting the stringified form, no behavioral change.
- `impl::TagWriter::PutLogExtra` now inspects the `LogExtra::Value` variant and routes to the correct overload:
  - `std::string` → `AddTag`
  - `JsonString` → `AddJsonTag`
  - `bool` → `AddTagBool`
  - `float / double` → `AddTagDouble`
  - signed integrals → `AddTagInt64`
  - unsigned integrals → `AddTagUInt64`
- `JsonFormatter` overrides all four typed overloads so numeric and boolean tags emit as JSON primitives instead of quoted strings. No other formatter changes — they continue to stringify.

### `Format::kOtlpJson` + `OtlpJsonFormatter`

- New `Format::kOtlpJson` enum value + string parser entry (`"otlp_json"`).
- `OtlpJsonFormatter` streams a single JSON object per record matching the OTLP `LogRecord` schema:
  - `timeUnixNano` (quoted int64 — precision-safe for JS consumers).
  - `severityNumber` (OTLP enum: TRACE=1, DEBUG=5, INFO=9, WARN=13, ERROR=17, FATAL=21) + `severityText` (uppercase label).
  - `attributes` array of `{"key","value":{"<kind>":<raw>}}` entries. `kind` is `stringValue` / `intValue` / `doubleValue` / `boolValue` per the typed overload.
  - `body.stringValue` for the free-form text.
  - Array elision: the `attributes` array opens only on the first tag emission; records without any tags skip it entirely.
- OTLP spec encodes int64 as a JSON string, doubles as JSON number, booleans as JSON literal — matches spec precisely.
- `JsonString` (user-supplied raw JSON) funnels through `stringValue` with escape — OTLP `AnyValue` has no raw-JSON slot; consumers that want to post-parse the string can.
- The module field (call site) is emitted as a string attribute (`"module"`) rather than a top-level field, matching OTLP's attribute-centric model.

### Test coverage

- `FormatterJson.TypedTagsEmitAsNativeJsonTypes` — int, double, bool, string all land as the right JSON type (unquoted for numeric, `true`/`false` for bool, quoted string otherwise).
- `FormatterOtlpJson.EmitsSchemaShape` — end-to-end OTLP record with all four `AnyValue` kinds; asserts every expected key/value pair is present with the correct nesting.
- `FormatterOtlpJson.NoAttributesElidesTheArray` — minimal record without user tags still parses and keeps body + severity.
- Existing `FormatterJson.YaDeployVariantRenamesCoreFields` updated to the unquoted integer expectation (`"user_id":7`).

### Example

- `examples/otlp_json/main.cpp` — initializes the default logger with `Format::kOtlpJson`, emits a couple of `LOG_INFO/ERROR` records to `otlp_example.jsonl`. Output verified to be schema-correct:

```json
{"timeUnixNano":"1776559018500469200","severityNumber":9,"severityText":"INFO","attributes":[{"key":"module","value":{"stringValue":"main ( examples\\otlp_json\\main.cpp:15 )"}},{"key":"user_id","value":{"intValue":"42"}},{"key":"latency_ms","value":{"doubleValue":13.5}},{"key":"ok","value":{"boolValue":true}},{"key":"endpoint","value":{"stringValue":"/api/login"}}],"body":{"stringValue":"user logged in"}}
```

### Integration

This output feeds directly into:
- **otel-collector** `filelog` receiver with the `json_parser` + `otlp` exporter — zero in-process HTTP/gRPC dependency on ulog's side.
- Any OTLP-over-HTTP-JSON forwarder that posts the line body to `POST /v1/logs`.

No opentelemetry-cpp / protobuf / grpc dependency needed in the application binary.

## Results

- **Build**: green (MSVC 14.44).
- **Tests**: **70/70 passing** (up from 67; +2 OTLP tests, +1 JSON typed-tag test).
- **Examples**: new `ulog-example-otlp` runs clean, output schema-correct.
- **Perf**: no regression on existing benches (only path change is an extra virtual dispatch for `LogExtra` values, amortized by the enclosing `fmt::format` that previously ran unconditionally).

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **No `trace_id` / `span_id` top-level fields.** They're first-class in the OTLP LogRecord schema. Current emitter just passes them through as string attributes if the tracing hook injects them. Can be lifted to top-level when the formatter recognizes those specific attribute keys — BACKLOG.
- **No `ExportLogsServiceRequest` envelope.** We emit bare LogRecords, not the full `resourceLogs[].scopeLogs[].logRecords[]` tree. This is intentional — matches what otel-collector's `filelog+json_parser` expects. A `OtlpBatchSink` that wraps batches in the envelope for direct `POST /v1/logs` is a separate BACKLOG item (needs HTTP client or `opentelemetry-cpp` dep).
- **JsonString passthrough is lossy for OTLP consumers** that would rather receive the structured value. Acceptable because ulog's `JsonString` is an opaque "pre-serialized blob" — round-tripping it requires re-parsing on the consumer.
- **Severity mapping** hard-codes the OTLP enum values. If the OTLP spec revises them (unlikely — stable since 0.20), edit the switch.

### Deferred to BACKLOG
- `trace_id` / `span_id` top-level extraction (treat `trace_id`/`span_id` attribute keys specially).
- `OtlpBatchSink` that wraps records in `ExportLogsServiceRequest` + optionally POSTs via HTTP.
- `OtlpGrpcSink` via `opentelemetry-cpp` — heavy dep, gated behind `ULOG_WITH_OPENTELEMETRY`.
- `SmallString<>`-backed `text_` in `JsonFormatter` / `OtlpJsonFormatter` for inline short-text records.

## Acceptance: ✅ commit + advance
