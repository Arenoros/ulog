#include <ulog/impl/formatters/tskv.hpp>

#include <utility>

#include <fmt/format.h>

#include <ulog/detail/timestamp.hpp>
#include <ulog/detail/tskv_escape.hpp>

namespace ulog::impl::formatters {

TskvFormatter::TskvFormatter(Level level,
                             std::string_view module_function,
                             std::string_view module_file,
                             int module_line,
                             std::chrono::system_clock::time_point tp) {
    auto& b = item_->payload;

    detail::EncodeTskv(b, "timestamp", detail::TskvMode::kKey);
    b += detail::kTskvKeyValueSeparator;
    detail::AppendTimestampUtc(b, tp);

    b += detail::kTskvPairsSeparator;
    detail::EncodeTskv(b, "level", detail::TskvMode::kKey);
    b += detail::kTskvKeyValueSeparator;
    detail::EncodeTskv(b, ToUpperCaseString(level), detail::TskvMode::kValue);

    if (!module_function.empty() || !module_file.empty()) {
        b += detail::kTskvPairsSeparator;
        detail::EncodeTskv(b, "module", detail::TskvMode::kKey);
        b += detail::kTskvKeyValueSeparator;
        const auto module_value = fmt::format("{} ( {}:{} )",
                                              module_function, module_file, module_line);
        detail::EncodeTskv(b, module_value, detail::TskvMode::kValue);
    }
}

void TskvFormatter::AddTag(std::string_view key, std::string_view value) {
    if (!item_) return;
    auto& b = item_->payload;
    b += detail::kTskvPairsSeparator;
    detail::EncodeTskv(b, key, detail::TskvMode::kKeyReplacePeriod);
    b += detail::kTskvKeyValueSeparator;
    detail::EncodeTskv(b, value, detail::TskvMode::kValue);
}

void TskvFormatter::AddJsonTag(std::string_view key, const JsonString& value) {
    AddTag(key, value.View());
}

void TskvFormatter::SetText(std::string_view text) {
    if (!item_) return;
    auto& b = item_->payload;
    b += detail::kTskvPairsSeparator;
    detail::EncodeTskv(b, "text", detail::TskvMode::kKey);
    b += detail::kTskvKeyValueSeparator;
    detail::EncodeTskv(b, text, detail::TskvMode::kValue);
}

std::unique_ptr<LoggerItemBase> TskvFormatter::ExtractLoggerItem() {
    if (!item_) return nullptr;
    if (!finalized_) {
        item_->payload += '\n';
        finalized_ = true;
    }
    return std::move(item_);
}

}  // namespace ulog::impl::formatters
