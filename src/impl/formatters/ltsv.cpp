#include <ulog/impl/formatters/ltsv.hpp>

#include <utility>

#include <ulog/detail/small_string.hpp>
#include <ulog/detail/timestamp.hpp>
#include <ulog/detail/tskv_escape.hpp>

namespace ulog::impl::formatters {

namespace {
constexpr char kLtsvSeparator = ':';
constexpr char kLtsvPairsSeparator = '\t';
}  // namespace

LtsvFormatter::LtsvFormatter(Level level,
                             const LogRecordLocation& location,
                             std::chrono::system_clock::time_point tp,
                             TimestampFormat ts_fmt) {
    auto& b = item_->payload;

    detail::EncodeTskv(b, "timestamp", detail::TskvMode::kKey);
    b += kLtsvSeparator;
    detail::AppendTimestamp(b, tp, ts_fmt);

    b += kLtsvPairsSeparator;
    detail::EncodeTskv(b, "level", detail::TskvMode::kKey);
    b += kLtsvSeparator;
    detail::EncodeTskv(b, ToUpperCaseString(level), detail::TskvMode::kValue);

    if (location.has_value()) {
        b += kLtsvPairsSeparator;
        detail::EncodeTskv(b, "module", detail::TskvMode::kKey);
        b += kLtsvSeparator;
        detail::SmallString<128> module_buf;
        module_buf += location.function_name();
        module_buf += " ( ";
        module_buf += location.file_name();
        module_buf += ':';
        module_buf += location.line_string();
        module_buf += " )";
        detail::EncodeTskv(b, module_buf.view(), detail::TskvMode::kValue);
    }
}

void LtsvFormatter::AddTag(std::string_view key, std::string_view value) {
    if (!item_) return;
    auto& b = item_->payload;
    b += kLtsvPairsSeparator;
    detail::EncodeTskv(b, key, detail::TskvMode::kKeyReplacePeriod);
    b += kLtsvSeparator;
    detail::EncodeTskv(b, value, detail::TskvMode::kValue);
}

void LtsvFormatter::AddJsonTag(std::string_view key, const JsonString& value) {
    AddTag(key, value.View());
}

void LtsvFormatter::SetText(std::string_view text) {
    if (!item_) return;
    auto& b = item_->payload;
    b += kLtsvPairsSeparator;
    detail::EncodeTskv(b, "text", detail::TskvMode::kKey);
    b += kLtsvSeparator;
    detail::EncodeTskv(b, text, detail::TskvMode::kValue);
}

std::unique_ptr<LoggerItemBase> LtsvFormatter::ExtractLoggerItem() {
    if (!item_) return nullptr;
    if (!finalized_) {
        item_->payload += '\n';
        finalized_ = true;
    }
    return std::move(item_);
}

}  // namespace ulog::impl::formatters
