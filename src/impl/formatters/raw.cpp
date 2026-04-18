#include <ulog/impl/formatters/raw.hpp>

#include <ulog/detail/tskv_escape.hpp>

namespace ulog::impl::formatters {

void RawFormatter::AddTag(std::string_view key, std::string_view value) {
    auto& b = item_.payload;
    if (has_fields_) b += detail::kTskvPairsSeparator;
    detail::EncodeTskv(b, key, detail::TskvMode::kKeyReplacePeriod);
    b += detail::kTskvKeyValueSeparator;
    detail::EncodeTskv(b, value, detail::TskvMode::kValue);
    has_fields_ = true;
}

void RawFormatter::AddJsonTag(std::string_view key, const JsonString& value) {
    AddTag(key, value.View());
}

void RawFormatter::SetText(std::string_view text) {
    auto& b = item_.payload;
    if (has_fields_) b += detail::kTskvPairsSeparator;
    detail::EncodeTskv(b, "text", detail::TskvMode::kKey);
    b += detail::kTskvKeyValueSeparator;
    detail::EncodeTskv(b, text, detail::TskvMode::kValue);
    has_fields_ = true;
}

LoggerItemRef RawFormatter::ExtractLoggerItem() {
    if (!finalized_) {
        item_.payload += '\n';
        finalized_ = true;
    }
    return item_;
}

}  // namespace ulog::impl::formatters
