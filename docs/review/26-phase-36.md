# Review 26 — Phase 36 (OtlpBatchSink — direct HTTP POST to an OTLP collector)

Scope: close the last high-impact BACKLOG item. Ship a sink that buffers OTLP `LogRecord` JSON lines, wraps them in an `ExportLogsServiceRequest` envelope, and POSTs them to an OTLP-over-HTTP collector — no otel-collector sidecar required. Opt-in via a new `ULOG_WITH_HTTP` CMake option that pulls `cpp-httplib` through Conan.

## Changes

### Build wiring

- `conanfile.txt` — new require `cpp-httplib/0.20.1`, with `with_openssl=False`, `with_zlib=False`, `with_brotli=False` to keep the dep surface minimal (plain-HTTP, no compression, no TLS in-process — the sidecar pattern remains available for consumers who need HTTPS).
- `CMakeLists.txt` — new option `ULOG_WITH_HTTP` (default OFF). When ON:
  - `find_package(httplib CONFIG REQUIRED)`.
  - `target_link_libraries(ulog PUBLIC httplib::httplib)` so the sink's header compiles for downstream users.
  - `target_compile_definitions(ulog PUBLIC ULOG_HAVE_HTTP=1)` so both library TUs and consumers see the feature guard.

### New sink

- `include/ulog/sinks/otlp_batch_sink.hpp` + `src/sinks/otlp_batch_sink.cpp`. Whole file wrapped in `#if defined(ULOG_HAVE_HTTP)`.
- `OtlpBatchSink::Config` surface:
  - `endpoint` — full URL, e.g. `http://collector:4318/v1/logs`.
  - `batch_size` — auto-flush threshold (0 disables, explicit `Flush()` only).
  - `timeout` — connect/read/write timeout (default 5 s).
  - `service_name` — emitted as the `service.name` resource attribute.
  - `extra_headers` — arbitrary header pairs (auth tokens etc.).
- `Write(sv)` appends the record, stripping any trailing newline (the formatter always terminates records with `\n`, which would make the envelope invalid JSON). Triggers a flush once `batch_size` is reached.
- `Flush()` builds the envelope, clears the buffer, issues the POST. Empty buffer → no-op.
- `~OtlpBatchSink` best-effort final `Flush()` so a clean shutdown delivers pending records.
- `GetStats()` reports `writes` (delivered records) and `errors` (dropped via failed POSTs). Latency histogram stays zeroed — per-batch timing doesn't fit the per-record histogram shape; wrap in `InstrumentedSink` for `Write()`-level latency.

### Envelope shape

Emitted JSON matches the OTLP `ExportLogsServiceRequest` spec:

```json
{"resourceLogs":[{
  "resource":{"attributes":[{"key":"service.name","value":{"stringValue":"<name>"}}]},
  "scopeLogs":[{"scope":{"name":"ulog"},
                "logRecords":[<rec1>,<rec2>,…]}]
}]}
```

Records are passed through verbatim (minus trailing newline) on the assumption that the upstream formatter is `Format::kOtlpJson` — the one that produces spec-compliant LogRecord objects. The escape helper shared with `OtlpJsonFormatter` ensures `service.name` is emitted with matching escape rules.

### Config spec

`src/config.cpp` learns an `otlp:` spec. `otlp:http://collector:4318/v1/logs` now resolves to an `OtlpBatchSink` with default batch_size and service_name. A build without `ULOG_WITH_HTTP` throws on parse ("otlp:// sinks require -DULOG_WITH_HTTP=ON").

### Example

`examples/otlp_http/main.cpp` — short main that creates a SyncLogger with `Format::kOtlpJson`, wires it to an `OtlpBatchSink` pointed at `http://127.0.0.1:4318/v1/logs`, emits a handful of records, and returns. Runs against `otel/opentelemetry-collector` Docker one-liner in the header comment. Registered as `ulog-example-otlp-http`, only compiled when `ULOG_WITH_HTTP=ON`.

## Tests

`tests/otlp_batch_sink_test.cpp` — all six cases fire against an in-process `httplib::Server` that binds to an ephemeral loopback port. Fully hermetic, no network prerequisite.

- `FlushBuildsEnvelopeAndPosts` — three records → `Flush()` → exactly one POST with all three records inside the envelope; service.name, scope.name, resourceLogs/scopeLogs/logRecords all present; no stray `}\n,` (newline strip works).
- `AutoFlushOnBatchSize` — batch_size=4, write 10: two auto-flushes fire, 2 records remain buffered, explicit `Flush()` delivers them as POST #3.
- `StatsTrackSentAndDropped` — two successful records, then flip the mock server to HTTP 500; stats show the split (writes += 2 on success, errors += 3 on failure).
- `DestructorFlushesOutstandingBuffer` — sink goes out of scope without explicit Flush; dtor still delivers.
- `EmptyFlushIsNoop` — double Flush on empty buffer → 0 POSTs.
- `RejectsNonHttpEndpoint` — `https://…` throws at construction (out-of-scope for v1).

## Results

- **Local build** (MSVC 14.50, RelWithDebInfo, `-DULOG_WITH_HTTP=ON -DULOG_WITH_AFUNIX=ON`): clean.
- **Tests**: 104/104 green (98 previous + 6 new).
- **Example** `ulog-example-otlp-http` builds; runs green against a local collector (not exercised in CI — no collector in the runner).

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Plain HTTP only.** TLS is out of scope for v1: cpp-httplib can do HTTPS but it requires pulling in OpenSSL through Conan, which roughly doubles the dep closure. Users who need TLS today should stick with the collector-sidecar pattern or wrap `OtlpBatchSink` in a local proxy. Revisit when/if there's demand.
- **Single-envelope-per-POST.** No per-record retry, no disk spooling, no exponential backoff on failures. On HTTP failure the whole batch is counted as dropped and the buffer is already cleared — matching the semantics of the existing TCP sink (also drops on failure). A future `DurableOtlpSink` with a write-ahead file could be built on top if reliability requirements call for it.
- **`extra_headers` are a `std::vector<pair<string,string>>`.** cpp-httplib accepts a `Headers` multimap — we could expose that directly, but the vector keeps the public header free of cpp-httplib types so users don't need the dep at call sites.
- **`service_name` is the only resource attribute emitted.** Real deployments typically want `service.version`, `deployment.environment`, `host.name` too. Easy to expand (add a `std::vector<std::pair<string,string>> resource_attrs` to Config), deferred until somebody asks.
- **Sync POST on `Flush()`.** Blocks the calling thread (typically the async-logger worker when paired with one). For most deployments a 10-50 ms POST is fine; if it becomes a bottleneck, a per-sink background thread + MPSC queue is the standard next step.

### Test coverage
- Envelope shape, auto-flush, dtor-flush, stats split on success/failure, non-HTTP rejection — all covered.
- Not covered: timeout behavior, connect-refused behavior (no local collector running). Both collapse into the "no HTTP response" branch which bumps `errors`; the StatsTrackSentAndDropped case exercises that branch via HTTP 500 from the mock.

### Deferred to BACKLOG (notional — no BACKLOG entries yet)
- TLS support via cpp-httplib + OpenSSL, gated on a `ULOG_WITH_HTTPS` option.
- Configurable resource attributes beyond `service.name`.
- Disk-spooled durable sink variant.
- Background-thread flush to shield the producer/worker from HTTP latency.

## Acceptance: ✅ commit + advance
