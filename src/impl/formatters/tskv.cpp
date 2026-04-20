#include <ulog/impl/formatters/tskv.hpp>

#include <utility>

#include <ulog/detail/small_string.hpp>
#include <ulog/detail/timestamp.hpp>
#include <ulog/detail/tskv_escape.hpp>

namespace ulog::impl::formatters {

TskvFormatter::TskvFormatter(Level level,
                             const LogRecordLocation& location,
                             std::chrono::system_clock::time_point tp,
                             TimestampFormat ts_fmt) {
    auto& b = item_->payload;

    detail::EncodeTskv(b, "timestamp", detail::TskvMode::kKey);
    b += detail::kTskvKeyValueSeparator;
    detail::AppendTimestamp(b, tp, ts_fmt);

    b += detail::kTskvPairsSeparator;
    detail::EncodeTskv(b, "level", detail::TskvMode::kKey);
    b += detail::kTskvKeyValueSeparator;
    detail::EncodeTskv(b, ToUpperCaseString(level), detail::TskvMode::kValue);

    if (location.has_value()) {
        b += detail::kTskvPairsSeparator;
        detail::EncodeTskv(b, "module", detail::TskvMode::kKey);
        b += detail::kTskvKeyValueSeparator;
        // Assemble `module` without fmt::format — precomputed line string
        // comes from LogRecordLocation so the only per-record cost is the
        // TSKV escaping of function+file.
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

LoggerItemPtr TskvFormatter::ExtractLoggerItem() {
    if (!item_) return LoggerItemPtr{nullptr};
    if (!finalized_) {
        item_->payload += '\n';
        finalized_ = true;
    }
    // Release ownership from the concrete-typed unique_ptr and re-wrap
    // with the polymorphic deleter — both deleters route to the same
    // pool, so steady-state still pays zero heap operations.
    return LoggerItemPtr{item_.release()};
}

}  // namespace ulog::impl::formatters
