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

#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/level.hpp>

namespace ulog::impl::formatters {

class OtlpJsonFormatter final : public Base {
public:
    OtlpJsonFormatter(Level level,
                      std::string_view module_function,
                      std::string_view module_file,
                      int module_line,
                      std::chrono::system_clock::time_point tp);

    void AddTag(std::string_view key, std::string_view value) override;
    void AddJsonTag(std::string_view key, const JsonString& value) override;
    void AddTagInt64(std::string_view key, std::int64_t value) override;
    void AddTagUInt64(std::string_view key, std::uint64_t value) override;
    void AddTagDouble(std::string_view key, double value) override;
    void AddTagBool(std::string_view key, bool value) override;
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
    std::string body_text_;
    bool first_attr_{true};
    bool finalized_{false};
};

}  // namespace ulog::impl::formatters
