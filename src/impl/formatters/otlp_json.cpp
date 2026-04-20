#include <ulog/impl/formatters/otlp_json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <fmt/format.h>

namespace ulog::impl::formatters {

namespace {

/// Append escaped JSON string body (no quotes) — same rules as JsonFormatter.
template <typename Sink>
void AppendJsonEscaped(Sink& out, std::string_view value) {
    for (unsigned char c : value) {
        switch (c) {
            case '"':  out += '\\'; out += '"';  break;
            case '\\': out += '\\'; out += '\\'; break;
            case '\b': out += '\\'; out += 'b';  break;
            case '\f': out += '\\'; out += 'f';  break;
            case '\n': out += '\\'; out += 'n';  break;
            case '\r': out += '\\'; out += 'r';  break;
            case '\t': out += '\\'; out += 't';  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    const auto n = std::snprintf(buf, sizeof(buf), "\\u%04x",
                                                 static_cast<unsigned>(c));
                    if (n > 0) out += std::string_view(buf, static_cast<std::size_t>(n));
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

/// Maps ulog Level to OTLP SeverityNumber (opentelemetry-proto enum).
int SeverityNumber(Level level) noexcept {
    switch (level) {
        case Level::kTrace:    return 1;   // TRACE
        case Level::kDebug:    return 5;   // DEBUG
        case Level::kInfo:     return 9;   // INFO
        case Level::kWarning:  return 13;  // WARN
        case Level::kError:    return 17;  // ERROR
        case Level::kCritical: return 21;  // FATAL
        case Level::kNone:     return 0;   // UNSPECIFIED
    }
    return 0;
}

/// UTC time_point → nanoseconds-since-epoch string. OTLP/JSON requires
/// int64-valued fields to be JSON strings so JavaScript consumers don't
/// lose precision (52-bit mantissa). Emit as quoted digits.
std::string TimeUnixNanoString(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto ns = duration_cast<nanoseconds>(tp.time_since_epoch()).count();
    return fmt::format("{}", ns);
}

}  // namespace

OtlpJsonFormatter::OtlpJsonFormatter(Level level,
                                     const LogRecordLocation& location,
                                     std::chrono::system_clock::time_point tp) {
    auto& b = item_->payload;
    b += '{';

    b += "\"timeUnixNano\":\"";
    b += TimeUnixNanoString(tp);
    b += '"';

    b += ",\"severityNumber\":";
    b += fmt::format("{}", SeverityNumber(level));

    b += ",\"severityText\":\"";
    AppendJsonEscaped(b, ToUpperCaseString(level));
    b += '"';

    if (location.has_value()) {
        detail::SmallString<128> module_buf;
        module_buf += location.function_name();
        module_buf += " ( ";
        module_buf += location.file_name();
        module_buf += ':';
        module_buf += location.line_string();
        module_buf += " )";
        EmitStringAttribute("module", module_buf.view());
    }
}

void OtlpJsonFormatter::EmitAttribute(std::string_view key, std::string_view kind,
                                      std::string_view raw_value) {
    auto& b = item_->payload;
    if (first_attr_) {
        b += ",\"attributes\":[";
        first_attr_ = false;
    } else {
        b += ',';
    }
    b += "{\"key\":\"";
    AppendJsonEscaped(b, key);
    b += "\",\"value\":{\"";
    b += kind;
    b += "\":";
    b += raw_value;
    b += "}}";
}

void OtlpJsonFormatter::EmitStringAttribute(std::string_view key, std::string_view value) {
    // Build the quoted+escaped JSON string into a scratch string and pass
    // as raw_value to EmitAttribute so the escape rules stay in one place.
    std::string quoted;
    quoted.reserve(value.size() + 2);
    quoted += '"';
    AppendJsonEscaped(quoted, value);
    quoted += '"';
    EmitAttribute(key, "stringValue", quoted);
}

void OtlpJsonFormatter::AddTag(std::string_view key, std::string_view value) {
    EmitStringAttribute(key, value);
}

void OtlpJsonFormatter::SetTraceContext(std::string_view trace_id_hex,
                                        std::string_view span_id_hex) {
    // OTLP LogRecord carries `traceId` / `spanId` as top-level hex strings
    // (not as attributes) — see opentelemetry.proto.logs.v1. Stash into
    // dedicated slots and emit at finalization so Tempo/Jaeger can
    // correlate logs with spans.
    if (!trace_id_hex.empty()) trace_id_.assign(trace_id_hex.data(), trace_id_hex.size());
    if (!span_id_hex.empty())  span_id_.assign(span_id_hex.data(),  span_id_hex.size());
}

void OtlpJsonFormatter::AddJsonTag(std::string_view key, const JsonString& value) {
    // OTLP `AnyValue` has no raw-JSON variant; emit as a string attribute
    // carrying the serialized JSON text so consumers can post-parse.
    EmitStringAttribute(key, value.View());
}

void OtlpJsonFormatter::AddTagInt64(std::string_view key, std::int64_t value) {
    // OTLP int64 is wire-typed but JSON-encoded as string for precision.
    const auto raw = fmt::format("\"{}\"", value);
    EmitAttribute(key, "intValue", raw);
}

void OtlpJsonFormatter::AddTagUInt64(std::string_view key, std::uint64_t value) {
    const auto raw = fmt::format("\"{}\"", value);
    EmitAttribute(key, "intValue", raw);
}

void OtlpJsonFormatter::AddTagDouble(std::string_view key, double value) {
    const auto raw = fmt::format("{}", value);
    EmitAttribute(key, "doubleValue", raw);
}

void OtlpJsonFormatter::AddTagBool(std::string_view key, bool value) {
    EmitAttribute(key, "boolValue", value ? "true" : "false");
}

void OtlpJsonFormatter::SetText(std::string_view text) {
    body_text_.assign(text);
}

std::unique_ptr<LoggerItemBase> OtlpJsonFormatter::ExtractLoggerItem() {
    if (!item_) return nullptr;
    if (!finalized_) {
        auto& b = item_->payload;
        if (!first_attr_) b += ']';  // close the attributes array
        if (!trace_id_.empty()) {
            b += ",\"traceId\":\"";
            AppendJsonEscaped(b, trace_id_);
            b += '"';
        }
        if (!span_id_.empty()) {
            b += ",\"spanId\":\"";
            AppendJsonEscaped(b, span_id_);
            b += '"';
        }
        b += ",\"body\":{\"stringValue\":\"";
        AppendJsonEscaped(b, body_text_);
        b += "\"}";
        b += "}\n";
        finalized_ = true;
    }
    return std::move(item_);
}

}  // namespace ulog::impl::formatters
