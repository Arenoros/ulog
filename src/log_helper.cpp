#include <ulog/log_helper.hpp>

#include <memory>
#include <string>
#include <utility>

#include <fmt/format.h>

#include <ulog/detail/small_string.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/impl/logger_base.hpp>
#include <ulog/impl/tag_writer.hpp>
#include <ulog/log.hpp>
#include <ulog/tracing_hook.hpp>

namespace ulog {

namespace {

class FormatterTagSink final : public TagSink {
public:
    explicit FormatterTagSink(impl::formatters::Base* f) noexcept : f_(f) {}
    void AddTag(std::string_view key, std::string_view value) override {
        if (f_) f_->AddTag(key, value);
    }
    void AddJsonTag(std::string_view key, const JsonString& value) override {
        if (f_) f_->AddJsonTag(key, value);
    }
private:
    impl::formatters::Base* f_;
};

}  // namespace

// ---------------- LogHelper::Impl ----------------

struct LogHelper::Impl {
    Impl(impl::LoggerBase& logger_ref,
         LoggerPtr logger_owner,
         Level level,
         LogRecordLocation location,
         bool active)
        : logger_ref(logger_ref),
          logger_owner(std::move(logger_owner)),
          level(level),
          location(location),
          active(active),
          writer(nullptr) {
        if (active) {
            formatter = logger_ref.MakeFormatter(
                level,
                location.function ? std::string_view(location.function) : std::string_view{},
                location.file ? std::string_view(location.file) : std::string_view{},
                location.line);
            writer.Reset(formatter.get());
        }
    }

    impl::LoggerBase& logger_ref;
    LoggerPtr logger_owner;                     ///< Keeps the logger alive; may be null for ref path.
    Level level;
    LogRecordLocation location;
    bool active;
    impl::formatters::BasePtr formatter;
    detail::SmallString<1024> text;
    impl::TagWriter writer;
};

// ---------------- LogHelper ----------------

LogHelper::LogHelper(LoggerRef logger, Level level, LogRecordLocation location) noexcept
    : impl_(std::make_unique<Impl>(logger, LoggerPtr{}, level, location, /*active=*/true)) {}

LogHelper::LogHelper(LoggerRef logger, Level level, LogRecordLocation location, NoLog) noexcept
    : impl_(std::make_unique<Impl>(logger, LoggerPtr{}, level, location, /*active=*/false)) {}

LogHelper::LogHelper(LoggerPtr logger, Level level, LogRecordLocation location) noexcept
    : impl_(nullptr) {
    if (!logger) {
        // Pathological case — caller passed null. Drop.
        impl_ = nullptr;
        return;
    }
    auto& ref = *logger;
    impl_ = std::make_unique<Impl>(ref, std::move(logger), level, location, /*active=*/true);
}

LogHelper::~LogHelper() {
    if (!impl_ || !impl_->active) return;
    try {
        FormatterTagSink sink(impl_->formatter.get());
        impl::DispatchTracingHook(sink);
        impl_->formatter->SetText(impl_->text.view());
        auto item = impl_->formatter->ExtractLoggerItem();
        const bool want_flush = impl_->level >= impl_->logger_ref.GetFlushOn();
        impl_->logger_ref.Log(impl_->level, std::move(item));
        if (want_flush) impl_->logger_ref.Flush();
    } catch (...) {
        // Never throw from a logger destructor.
    }
}

bool LogHelper::IsActive() const noexcept { return impl_ && impl_->active; }

impl::TagWriter& LogHelper::GetTagWriter() noexcept { return impl_->writer; }

// ---- text stream appenders ----

void LogHelper::Put(std::string_view sv) {
    if (!impl_ || !impl_->active) return;
    impl_->text += sv;
}
void LogHelper::Put(const char* s) { Put(std::string_view(s ? s : "")); }
void LogHelper::Put(bool v) { Put(std::string_view(v ? "true" : "false")); }
void LogHelper::Put(char v) {
    if (!impl_ || !impl_->active) return;
    impl_->text += v;
}
void LogHelper::PutFormatted(std::string s) { Put(std::string_view(s)); }

LogHelper& LogHelper::operator<<(const LogExtra& extra) & {
    if (impl_ && impl_->active) impl_->writer.PutLogExtra(extra);
    return *this;
}
LogHelper&& LogHelper::operator<<(const LogExtra& extra) && {
    *this << extra;
    return std::move(*this);
}

LogHelper& LogHelper::operator<<(Hex v) & {
    if (impl_ && impl_->active) PutFormatted(fmt::format("0x{:016X}", v.value));
    return *this;
}
LogHelper& LogHelper::operator<<(HexShort v) & {
    if (impl_ && impl_->active) PutFormatted(fmt::format("{:X}", v.value));
    return *this;
}
LogHelper& LogHelper::operator<<(Quoted v) & {
    if (impl_ && impl_->active) {
        impl_->text += '"';
        impl_->text += v.value;
        impl_->text += '"';
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
