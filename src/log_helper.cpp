#include <ulog/log_helper.hpp>

#include <chrono>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <ulog/detail/small_string.hpp>
#include <ulog/detail/timestamp.hpp>
#include <ulog/detail/tskv_escape.hpp>
#include <ulog/impl/logger_base.hpp>
#include <ulog/log.hpp>

namespace ulog {

// Forward-declare the minimal tag writer that LogHelper exposes.
namespace impl {

class TagWriter {
public:
    explicit TagWriter(detail::SmallString<4096>& buf) noexcept : buf_(buf) {}

    /// Writes a "key=value" TSKV pair (value is TSKV-escaped).
    void PutTag(std::string_view key, std::string_view value) {
        buf_ += detail::kTskvPairsSeparator;
        detail::EncodeTskv(buf_, key, detail::TskvMode::kKeyReplacePeriod);
        buf_ += detail::kTskvKeyValueSeparator;
        detail::EncodeTskv(buf_, value, detail::TskvMode::kValue);
    }

    /// Appends raw, pre-formatted bytes without escaping.
    void PutRaw(std::string_view raw) { buf_ += raw; }

    void PutLogExtra(const LogExtra& extra);

private:
    detail::SmallString<4096>& buf_;
};

}  // namespace impl

// ---------------- LogHelper::Impl ----------------

struct LogHelper::Impl {
    Impl(LoggerRef logger, Level level, LogRecordLocation location, bool active) noexcept
        : logger(logger),
          level(level),
          location(location),
          active(active),
          writer(buffer) {}

    LoggerRef logger;
    Level level;
    LogRecordLocation location;
    bool active;
    bool text_opened{false};
    detail::SmallString<4096> buffer;
    impl::TagWriter writer;

    void OpenHeader() {
        // timestamp=...\tlevel=...\tmodule=<function (file:line)>\ttext=
        detail::EncodeTskv(buffer, "timestamp", detail::TskvMode::kKey);
        buffer += detail::kTskvKeyValueSeparator;
        detail::AppendTimestampUtc(buffer, std::chrono::system_clock::now());

        buffer += detail::kTskvPairsSeparator;
        detail::EncodeTskv(buffer, "level", detail::TskvMode::kKey);
        buffer += detail::kTskvKeyValueSeparator;
        detail::EncodeTskv(buffer, ToUpperCaseString(level), detail::TskvMode::kValue);

        if (location.function || location.file) {
            buffer += detail::kTskvPairsSeparator;
            detail::EncodeTskv(buffer, "module", detail::TskvMode::kKey);
            buffer += detail::kTskvKeyValueSeparator;
            const auto module_field = fmt::format("{} ( {}:{} )",
                                                  location.function ? location.function : "",
                                                  location.file ? location.file : "",
                                                  location.line);
            detail::EncodeTskv(buffer, module_field, detail::TskvMode::kValue);
        }

        buffer += detail::kTskvPairsSeparator;
        detail::EncodeTskv(buffer, "text", detail::TskvMode::kKey);
        buffer += detail::kTskvKeyValueSeparator;
        text_opened = true;
    }
};

namespace impl {

void TagWriter::PutLogExtra(const LogExtra& extra) {
    for (const auto& [k, v] : extra.extra_) {
        buf_ += detail::kTskvPairsSeparator;
        detail::EncodeTskv(buf_, k, detail::TskvMode::kKeyReplacePeriod);
        buf_ += detail::kTskvKeyValueSeparator;
        std::visit(
            [&](const auto& x) {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, std::string>) {
                    detail::EncodeTskv(buf_, x, detail::TskvMode::kValue);
                } else if constexpr (std::is_same_v<T, JsonString>) {
                    detail::EncodeTskv(buf_, x.View(), detail::TskvMode::kValue);
                } else if constexpr (std::is_same_v<T, bool>) {
                    detail::EncodeTskv(buf_, x ? "true" : "false", detail::TskvMode::kValue);
                } else {
                    const auto s = fmt::format("{}", x);
                    detail::EncodeTskv(buf_, s, detail::TskvMode::kValue);
                }
            },
            v.GetValue());
    }
}

}  // namespace impl

// ---------------- LogHelper ----------------

LogHelper::LogHelper(LoggerRef logger, Level level, LogRecordLocation location) noexcept {
    const bool active = logger.ShouldLog(level);
    impl_ = std::make_unique<Impl>(logger, level, location, active);
    if (active) impl_->OpenHeader();
}

LogHelper::LogHelper(LoggerRef logger, Level level, LogRecordLocation location, NoLog) noexcept {
    impl_ = std::make_unique<Impl>(logger, level, location, /*active=*/false);
}

LogHelper::~LogHelper() {
    if (!impl_ || !impl_->active) return;
    // Terminate record with newline and hand off a text item to logger.
    impl_->buffer += '\n';

    struct TextItem final : impl::LoggerItemBase {
        std::string payload;
    };
    auto item = std::make_unique<TextItem>();
    item->payload.assign(impl_->buffer.data(), impl_->buffer.size());

    try {
        impl_->logger.Log(impl_->level, *item);
        if (impl_->level >= impl_->logger.GetFlushOn()) impl_->logger.Flush();
    } catch (...) {
        // Never throw from a logger destructor.
    }
}

bool LogHelper::IsActive() const noexcept { return impl_ && impl_->active; }

impl::TagWriter& LogHelper::GetTagWriter() noexcept { return impl_->writer; }

// ---- Stream appenders ----

void LogHelper::Put(std::string_view sv) {
    if (!impl_ || !impl_->active) return;
    detail::EncodeTskv(impl_->buffer, sv, detail::TskvMode::kValue);
}

void LogHelper::Put(const char* s) {
    Put(std::string_view(s ? s : ""));
}

void LogHelper::Put(bool v) {
    Put(std::string_view(v ? "true" : "false"));
}

void LogHelper::Put(char v) {
    if (!impl_ || !impl_->active) return;
    detail::EncodeTskvChar(impl_->buffer, v, detail::TskvMode::kValue);
}

void LogHelper::PutFormatted(std::string s) {
    Put(std::string_view(s));
}

LogHelper& LogHelper::operator<<(const LogExtra& extra) & {
    if (impl_ && impl_->active) impl_->writer.PutLogExtra(extra);
    return *this;
}
LogHelper&& LogHelper::operator<<(const LogExtra& extra) && {
    *this << extra;
    return std::move(*this);
}

LogHelper& LogHelper::operator<<(Hex v) & {
    if (impl_ && impl_->active) {
        PutFormatted(fmt::format("0x{:016X}", v.value));
    }
    return *this;
}
LogHelper& LogHelper::operator<<(HexShort v) & {
    if (impl_ && impl_->active) {
        PutFormatted(fmt::format("{:X}", v.value));
    }
    return *this;
}
LogHelper& LogHelper::operator<<(Quoted v) & {
    if (impl_ && impl_->active) {
        impl_->buffer += '"';
        detail::EncodeTskv(impl_->buffer, v.value, detail::TskvMode::kValue);
        impl_->buffer += '"';
    }
    return *this;
}

LogHelper& LogHelper::WithException(const std::exception& ex) & {
    if (impl_ && impl_->active) {
        impl_->writer.PutTag("exception_type", typeid(ex).name());
        impl_->writer.PutTag("exception_msg", ex.what());
    }
    return *this;
}

LogHelper& LogHelper::operator<<(const impl::RateLimiter& rl) & {
    if (impl_ && impl_->active && rl.GetDroppedCount() > 0) {
        PutFormatted(fmt::format(" [dropped={}]", rl.GetDroppedCount()));
    }
    return *this;
}

}  // namespace ulog
