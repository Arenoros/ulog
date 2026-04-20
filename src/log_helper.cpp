#include <ulog/log_helper.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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
    void SetTraceContext(std::string_view trace_id_hex,
                         std::string_view span_id_hex) override {
        if (f_) f_->SetTraceContext(trace_id_hex, span_id_hex);
    }
private:
    impl::formatters::Base* f_;
};

/// Forwards every formatter call to each target. ExtractLoggerItem is
/// unused — LogHelper extracts items from the underlying formatters
/// directly so one record produces N items in declared format order.
///
/// Non-owning: the wrapped formatters' lifetimes are managed by
/// `LogHelper::Impl`'s BasePtr members.
class FanoutFormatter final : public impl::formatters::Base {
public:
    void Add(impl::formatters::Base* f) {
        if (f) targets_.push_back(f);
    }
    bool empty() const noexcept { return targets_.empty(); }

    void AddTag(std::string_view key, std::string_view value) override {
        for (auto* t : targets_) t->AddTag(key, value);
    }
    void AddJsonTag(std::string_view key, const JsonString& value) override {
        for (auto* t : targets_) t->AddJsonTag(key, value);
    }
    void AddTagInt64(std::string_view key, std::int64_t value) override {
        for (auto* t : targets_) t->AddTagInt64(key, value);
    }
    void AddTagUInt64(std::string_view key, std::uint64_t value) override {
        for (auto* t : targets_) t->AddTagUInt64(key, value);
    }
    void AddTagDouble(std::string_view key, double value) override {
        for (auto* t : targets_) t->AddTagDouble(key, value);
    }
    void AddTagBool(std::string_view key, bool value) override {
        for (auto* t : targets_) t->AddTagBool(key, value);
    }
    void SetTraceContext(std::string_view trace_id_hex,
                         std::string_view span_id_hex) override {
        for (auto* t : targets_) t->SetTraceContext(trace_id_hex, span_id_hex);
    }
    void SetText(std::string_view text) override {
        for (auto* t : targets_) t->SetText(text);
    }
    std::unique_ptr<impl::formatters::LoggerItemBase> ExtractLoggerItem() override {
        return nullptr;  // unused — see class comment
    }

private:
    std::vector<impl::formatters::Base*> targets_;
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
        if (!active) return;

        const auto func_sv = location.function ? std::string_view(location.function) : std::string_view{};
        const auto file_sv = location.file ? std::string_view(location.file) : std::string_view{};

        // Primary formatter — inline scratch fast path, base format.
        formatter = logger_ref.MakeFormatterInto(
            &formatter_scratch,
            sizeof(formatter_scratch),
            level, func_sv, file_sv, location.line);

        // Detect multi-format: only TextLoggerBase participates.
        auto* text_base = dynamic_cast<impl::TextLoggerBase*>(&logger_ref);
        if (text_base && text_base->GetActiveFormatCount() > 1) {
            const auto formats = text_base->GetActiveFormats();
            extras.reserve(formats.size() - 1);
            for (std::size_t i = 1; i < formats.size(); ++i) {
                extras.push_back(text_base->MakeFormatterForFormat(
                    formats[i], level, func_sv, file_sv, location.line));
            }
            fanout.emplace();
            fanout->Add(formatter.get());
            for (auto& e : extras) fanout->Add(e.get());
            writer.Reset(&*fanout);
        } else {
            writer.Reset(formatter.get());
        }
    }

    impl::LoggerBase& logger_ref;
    LoggerPtr logger_owner;                     ///< Keeps the logger alive; may be null for ref path.
    Level level;
    LogRecordLocation location;
    bool active;
    /// Inline scratch for the primary formatter. Destructor runs via
    /// the `formatter` deleter — no heap alloc for the formatter itself
    /// on the hot path (formerly one `new` per record).
    alignas(impl::LoggerBase::kInlineFormatterAlign)
        std::byte formatter_scratch[impl::LoggerBase::kInlineFormatterSize];
    impl::formatters::BasePtr formatter;
    /// Extras — one per additional active format beyond the primary.
    /// Always heap-allocated; empty on the common single-format path.
    std::vector<impl::formatters::BasePtr> extras;
    /// Engaged only when extras is non-empty; aggregates tag/trace calls
    /// and broadcasts them to every formatter. Primary + extras outlive
    /// the fanout (same frame) — the non-owning pointers are safe.
    std::optional<FanoutFormatter> fanout;
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
        // Dispatch the tracing hook through whichever target TagWriter
        // currently points to — primary alone for single-format, fanout
        // for multi-format. Both satisfy the TagSink contract.
        impl::formatters::Base* hook_target = impl_->fanout
            ? static_cast<impl::formatters::Base*>(&*impl_->fanout)
            : impl_->formatter.get();
        FormatterTagSink sink(hook_target);
        impl::DispatchTracingHook(sink);

        // SetText mirrors to every formatter (primary + extras) so each
        // produced item carries the same text body.
        impl_->formatter->SetText(impl_->text.view());
        for (auto& e : impl_->extras) if (e) e->SetText(impl_->text.view());

        impl::LogItemList items;
        items.reserve(1 + impl_->extras.size());
        items.push_back(impl_->formatter->ExtractLoggerItem());
        for (auto& e : impl_->extras) {
            if (e) items.push_back(e->ExtractLoggerItem());
        }
        const bool want_flush = impl_->level >= impl_->logger_ref.GetFlushOn();
        impl_->logger_ref.LogMulti(impl_->level, std::move(items));
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
