#include <ulog/impl/formatters/json.hpp>

#include <cstdint>
#include <iterator>
#include <string>

#include <fmt/format.h>

#include <ulog/detail/timestamp.hpp>

namespace ulog::impl::formatters {

namespace {

/// Appends `value` escaped as a JSON string body (no quotes).
void AppendJsonEscaped(std::string& out, std::string_view value) {
    out.reserve(out.size() + value.size());
    for (unsigned char c : value) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    fmt::format_to(std::back_inserter(out), "\\u{:04x}", static_cast<unsigned>(c));
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
}

}  // namespace

JsonFormatter::JsonFormatter(Level level,
                             std::string_view module_function,
                             std::string_view module_file,
                             int module_line,
                             std::chrono::system_clock::time_point tp,
                             Variant variant)
    : variant_(variant) {
    fields_.push_back({"timestamp", detail::FormatTimestampUtc(tp), false});
    fields_.push_back({"level", std::string(ToUpperCaseString(level)), false});
    if (!module_function.empty() || !module_file.empty()) {
        fields_.push_back({"module",
                           fmt::format("{} ( {}:{} )", module_function, module_file, module_line),
                           false});
    }
}

void JsonFormatter::AddTag(std::string_view key, std::string_view value) {
    fields_.push_back({std::string(key), std::string(value), false});
}

void JsonFormatter::AddJsonTag(std::string_view key, const JsonString& value) {
    fields_.push_back({std::string(key), value.View(), true});
}

void JsonFormatter::SetText(std::string_view text) { text_.assign(text.data(), text.size()); }

std::string_view JsonFormatter::TranslateKey(std::string_view key) const noexcept {
    if (variant_ != Variant::kYaDeploy) return key;
    // Yandex.Deploy expects Logstash-style reserved names. Map ulog's
    // canonical fields onto `@timestamp`, `_level`, `_message`; leave
    // user-supplied tags untouched.
    if (key == "timestamp") return "@timestamp";
    if (key == "level")     return "_level";
    if (key == "text")      return "_message";
    return key;
}

void JsonFormatter::Finalize() {
    auto& b = item_.payload;
    b += '{';
    bool first = true;

    auto emit = [&](std::string_view k, std::string_view v, bool is_json) {
        if (!first) b += ',';
        first = false;
        b += '"';
        std::string escaped_key;
        AppendJsonEscaped(escaped_key, TranslateKey(k));
        b += std::string_view(escaped_key);
        b += "\":";
        if (is_json) {
            b += v;
        } else {
            b += '"';
            std::string escaped_val;
            AppendJsonEscaped(escaped_val, v);
            b += std::string_view(escaped_val);
            b += '"';
        }
    };

    for (const auto& f : fields_) emit(f.key, f.value, f.is_json);
    emit("text", text_, false);

    b += "}\n";
}

LoggerItemRef JsonFormatter::ExtractLoggerItem() {
    if (!finalized_) {
        Finalize();
        finalized_ = true;
    }
    return item_;
}

}  // namespace ulog::impl::formatters
