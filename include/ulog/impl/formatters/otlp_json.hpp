#pragma once

/// @file ulog/impl/formatters/otlp_json.hpp
/// @brief OTLP (OpenTelemetry) `LogRecord` JSON emitter — one record per line.
///
/// Output layout (per line) matches the JSON encoding of
/// `opentelemetry.proto.logs.v1.LogRecord`:
///
/// ```
/// {
///   "timeUnixNano": "1745000000000000000",
///   "severityNumber": 9,
///   "severityText": "INFO",
///   "body": {"stringValue": "message text"},
///   "attributes": [
///     {"key":"user_id","value":{"intValue":"42"}},
///     {"key":"latency_ms","value":{"doubleValue":13.7}},
///     {"key":"ok","value":{"boolValue":true}},
///     {"key":"endpoint","value":{"stringValue":"/api"}}
///   ],
///   "traceId": "0123456789abcdef0123456789abcdef",   // when provided
///   "spanId": "0123456789abcdef"                      // when provided
/// }
/// ```
///
/// Suitable for:
///  * `otel-collector` with the `filelog` receiver + `json_parser` → `otlp`
///    exporter.
///  * Any backend that ingests OTLP-over-HTTP-JSON body line by line
///    (e.g. via a thin HTTP forwarder).
///
/// The emitter does not attempt to build the full `ExportLogsServiceRequest`
/// envelope (ResourceLogs → ScopeLogs → LogRecord) — that batching lives
/// in the downstream transport. One LogRecord per line keeps `Reopen` + tail
/// semantics identical to TSKV/JSON file sinks.

#include <chrono>
#include <memory>
#include <string>
#include <string_view>

#include <ulog/detail/small_string.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/level.hpp>
#include <ulog/log_helper.hpp>

namespace ulog::impl::formatters {

class OtlpJsonFormatter final : public Base {
public:
    OtlpJsonFormatter(Level level,
                      const LogRecordLocation& location,
                      std::chrono::system_clock::time_point tp);

    void AddTag(std::string_view key, std::string_view value) override;
    void AddJsonTag(std::string_view key, const JsonString& value) override;
    void AddTagInt64(std::string_view key, std::int64_t value) override;
    void AddTagUInt64(std::string_view key, std::uint64_t value) override;
    void AddTagDouble(std::string_view key, double value) override;
    void AddTagBool(std::string_view key, bool value) override;
    void SetTraceContext(std::string_view trace_id_hex,
                         std::string_view span_id_hex) override;
    void SetText(std::string_view text) override;
    std::unique_ptr<LoggerItemBase> ExtractLoggerItem() override;

private:
    /// Emits a `{"key": ..., "value": {"<kind>":<raw>}}` entry into the
    /// attributes array. `raw` is literal JSON (string is already quoted).
    void EmitAttribute(std::string_view key, std::string_view kind,
                       std::string_view raw_value);
    /// Convenience for string-typed attributes (quotes + escapes).
    void EmitStringAttribute(std::string_view key, std::string_view value);

    std::unique_ptr<TextLogItem> item_{std::make_unique<TextLogItem>()};
    /// Free-form message body. Short messages stay on the stack via
    /// SmallString's SSO slab; longer ones spill to heap.
    detail::SmallString<64> body_text_;
    /// `trace_id` / `span_id` are first-class top-level fields in the OTLP
    /// LogRecord schema. Populated by `SetTraceContext` (invoked from the
    /// tracing hook) and emitted at `ExtractLoggerItem` time so that
    /// backends (Tempo/Jaeger/Grafana) can correlate logs with traces.
    /// Values are expected to be hex-encoded per the OTLP spec; the
    /// formatter passes them through verbatim.
    std::string trace_id_;
    std::string span_id_;
    bool first_attr_{true};
    bool finalized_{false};
};

}  // namespace ulog::impl::formatters
