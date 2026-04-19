#include <ulog/impl/formatters/json.hpp>

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <ulog/detail/timestamp.hpp>

namespace ulog::impl::formatters {

namespace {

/// Appends `value` escaped as a JSON string body (no surrounding quotes).
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

}  // namespace

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

void JsonFormatter::EmitField(std::string_view key, std::string_view value, bool value_is_json) {
    auto& b = item_->payload;
    // Opening `{` is written by the ctor before any field; subsequent
    // fields always get a leading comma.
    b += ',';
    b += '"';
    AppendJsonEscaped(b, TranslateKey(key));
    b += '"';
    b += ':';
    if (value_is_json) {
        b += value;
    } else {
        b += '"';
        AppendJsonEscaped(b, value);
        b += '"';
    }
}

JsonFormatter::JsonFormatter(Level level,
                             std::string_view module_function,
                             std::string_view module_file,
                             int module_line,
                             std::chrono::system_clock::time_point tp,
                             Variant variant)
    : variant_(variant) {
    auto& b = item_->payload;
    b += '{';
    // Emit the first field without a leading comma; everything else goes
    // through EmitField which always prepends ",".
    b += '"';
    AppendJsonEscaped(b, TranslateKey("timestamp"));
    b += "\":\"";
    const auto ts = detail::FormatTimestampUtc(tp);
    AppendJsonEscaped(b, ts);
    b += '"';

    EmitField("level", ToUpperCaseString(level), /*is_json=*/false);

    if (!module_function.empty() || !module_file.empty()) {
        const auto module_value = fmt::format("{} ( {}:{} )",
                                              module_function, module_file, module_line);
        EmitField("module", module_value, /*is_json=*/false);
    }
}

void JsonFormatter::AddTag(std::string_view key, std::string_view value) {
    if (!item_) return;
    EmitField(key, value, /*is_json=*/false);
}

void JsonFormatter::AddJsonTag(std::string_view key, const JsonString& value) {
    if (!item_) return;
    EmitField(key, value.View(), /*is_json=*/true);
}

void JsonFormatter::SetText(std::string_view text) { text_.assign(text.data(), text.size()); }

std::unique_ptr<LoggerItemBase> JsonFormatter::ExtractLoggerItem() {
    if (!item_) return nullptr;
    if (!finalized_) {
        EmitField("text", text_, /*is_json=*/false);
        item_->payload += '}';
        item_->payload += '\n';
        finalized_ = true;
    }
    return std::move(item_);
}

}  // namespace ulog::impl::formatters
