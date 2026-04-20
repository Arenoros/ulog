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
#include <ulog/record_enricher.hpp>
#include <ulog/sinks/structured_sink.hpp>
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

/// Accumulator that captures tags + trace context into a
/// `sinks::LogRecord` owned internally. Plugged into the fan-out when the
/// logger reports at least one structured sink — so the same `<< tag`
/// call that streams into a text formatter also populates the structured
/// record.
///
/// Typed `AddTag*` overrides are kept so native tag types (int, bool,
/// double, JSON) reach the sink without going through a string form.
class TagRecorder final : public impl::formatters::Base {
public:
    TagRecorder() : record_(std::make_unique<sinks::LogRecord>()) {}

    sinks::LogRecord* mutable_record() noexcept { return record_.get(); }
    std::unique_ptr<sinks::LogRecord> Release() noexcept { return std::move(record_); }

    void AddTag(std::string_view key, std::string_view value) override {
        record_->tags.push_back({std::string(key), std::string(value)});
    }
    void AddJsonTag(std::string_view key, const JsonString& value) override {
        record_->tags.push_back({std::string(key), value});
    }
    void AddTagInt64(std::string_view key, std::int64_t value) override {
        record_->tags.push_back({std::string(key), value});
    }
    void AddTagUInt64(std::string_view key, std::uint64_t value) override {
        record_->tags.push_back({std::string(key), value});
    }
    void AddTagDouble(std::string_view key, double value) override {
        record_->tags.push_back({std::string(key), value});
    }
    void AddTagBool(std::string_view key, bool value) override {
        record_->tags.push_back({std::string(key), value});
    }
    void SetTraceContext(std::string_view trace_id_hex,
                         std::string_view span_id_hex) override {
        if (!trace_id_hex.empty()) record_->trace_id = std::string(trace_id_hex);
        if (!span_id_hex.empty())  record_->span_id  = std::string(span_id_hex);
    }
    void SetText(std::string_view text) override {
        record_->text = std::string(text);
    }
    std::unique_ptr<impl::formatters::LoggerItemBase> ExtractLoggerItem() override {
        return nullptr;  // unused — LogHelper pulls the record via Release()
    }

private:
    std::unique_ptr<sinks::LogRecord> record_;
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
         const LogRecordLocation& location,
         bool active)
        : logger_ref(logger_ref),
          logger_owner(std::move(logger_owner)),
          level(level),
          location(location),
          active(active),
          writer(nullptr) {
        if (!active) return;

        const bool want_text = logger_ref.HasTextSinks();
        const bool want_structured = logger_ref.HasStructuredSinks();
        // Single vtable slot — cheaper than dynamic_cast; null for non-text
        // loggers. Cached once for both the extras branch and the
        // structured emit_location lookup below.
        impl::TextLoggerBase* text_base = logger_ref.AsTextLoggerBase();

        // Materialize the primary text formatter only when at least one
        // text sink is attached. Logger with only structured sinks skips
        // the formatter path entirely — zero bytes allocated for text
        // rendering on that path.
        if (want_text) {
            formatter = logger_ref.MakeFormatterInto(
                &formatter_scratch,
                sizeof(formatter_scratch),
                level, location);

            // Extras engage only when the logger has registered more than
            // the primary format. Atomic count check short-circuits the
            // snapshot pin for the single-format hot path. The snapshot,
            // once taken, is immutable (COW registry) — a concurrent
            // AddSink publishes a new vector; already-pinned vectors
            // stay valid for the LogHelper's lifetime. Sinks appended
            // after we pinned are invisible to this record; their
            // format_idx may overshoot items.size() and LogMulti skips
            // them via the out-of-range check.
            if (text_base && text_base->GetActiveFormatCount() > 1) {
                auto snap = text_base->GetActiveFormatsSnapshot();
                if (snap && snap->size() > 1) {
                    extras.reserve(snap->size() - 1);
                    for (std::size_t i = 1; i < snap->size(); ++i) {
                        extras.push_back(text_base->MakeFormatterForFormat(
                            (*snap)[i], level, location));
                    }
                }
            }
        }

        // Prime the structured recorder when any structured sink is present.
        // The record is seeded with level / timestamp / call-site metadata;
        // tags and text land later through the fanout / SetText path.
        //
        // Respect `TextLoggerBase::GetEmitLocation()` symmetrically with
        // the text formatter — a logger configured with
        // `emit_location = false` wants the location suppressed *everywhere*,
        // not only in the text `module` field.
        if (want_structured) {
            recorder.emplace();
            auto* rec = recorder->mutable_record();
            rec->level = level;
            rec->timestamp = std::chrono::system_clock::now();
            const bool emit_loc = text_base ? text_base->GetEmitLocation() : true;
            if (emit_loc) {
                const auto fn = location.function_name();
                const auto fl = location.file_name();
                if (!fn.empty()) rec->module_function.assign(fn);
                if (!fl.empty()) rec->module_file.assign(fl);
                rec->module_line = static_cast<int>(location.line());
            }
        }

        // Pick the writer target. Fanout engages whenever more than one
        // broadcast destination exists (primary + extras, or primary +
        // recorder, or any combination thereof). A single target
        // bypasses fanout entirely — that's the common LOG_* hot path.
        const std::size_t target_count =
            (formatter ? 1u : 0u) + extras.size() + (recorder ? 1u : 0u);
        if (target_count >= 2) {
            fanout.emplace();
            if (formatter) fanout->Add(formatter.get());
            for (auto& e : extras) fanout->Add(e.get());
            if (recorder)  fanout->Add(&*recorder);
            writer.Reset(&*fanout);
        } else if (formatter) {
            writer.Reset(formatter.get());
        } else if (recorder) {
            writer.Reset(&*recorder);
        }
        // target_count == 0: logger has no sinks at all. writer stays
        // null; tag / trace-hook calls are no-ops on a null formatter
        // pointer inside TagWriter.
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
    /// Engaged only when the logger has at least one structured sink.
    /// Accumulates tags / text / trace context into a sinks::LogRecord
    /// that is handed off to LogMulti alongside the text items.
    std::optional<TagRecorder> recorder;
    /// Engaged only when target_count >= 2; aggregates tag/trace calls
    /// and broadcasts them to every formatter (and recorder). Targets
    /// outlive the fanout (same frame) — the non-owning pointers are
    /// safe.
    std::optional<FanoutFormatter> fanout;
    detail::SmallString<1024> text;
    impl::TagWriter writer;
};

// ---------------- LogHelper ----------------

LogHelper::LogHelper(LoggerRef logger, Level level, const LogRecordLocation& location) noexcept
    : impl_(std::make_unique<Impl>(logger, LoggerPtr{}, level, location, /*active=*/true)) {}

LogHelper::LogHelper(LoggerRef logger, Level level, NoLog, const LogRecordLocation& location) noexcept
    : impl_(std::make_unique<Impl>(logger, LoggerPtr{}, level, location, /*active=*/false)) {}

LogHelper::LogHelper(LoggerPtr logger, Level level, const LogRecordLocation& location) noexcept
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
        // Dispatch the tracing hook + record enrichers through whichever
        // target TagWriter currently points to — the fanout when multiple
        // destinations are active, otherwise the sole target (formatter
        // OR recorder). Both satisfy the TagSink / Base contract.
        impl::formatters::Base* hook_target = nullptr;
        if (impl_->fanout)         hook_target = &*impl_->fanout;
        else if (impl_->formatter) hook_target = impl_->formatter.get();
        else if (impl_->recorder)  hook_target = &*impl_->recorder;
        if (hook_target) {
            FormatterTagSink sink(hook_target);
            impl::DispatchTracingHook(sink);
            impl::DispatchRecordEnrichers(sink);
        }

        // SetText mirrors to every destination so each produced item /
        // record carries the same text body.
        const auto text_view = impl_->text.view();
        if (impl_->formatter) impl_->formatter->SetText(text_view);
        for (auto& e : impl_->extras) if (e) e->SetText(text_view);
        if (impl_->recorder)  impl_->recorder->SetText(text_view);

        const bool want_flush = impl_->level >= impl_->logger_ref.GetFlushOn();

        // Single-format text-only hot path: no extras, no structured.
        // Route straight to Log(level, item) — skips the LogItemList
        // wrap and the LogMulti vector snapshot. This is the LOG_* path
        // for the overwhelming common case (one format, no structured
        // sink).
        if (impl_->formatter && impl_->extras.empty() && !impl_->recorder) {
            auto item = impl_->formatter->ExtractLoggerItem();
            impl_->logger_ref.Log(impl_->level, std::move(item));
            if (want_flush) impl_->logger_ref.Flush();
            return;
        }

        impl::LogItemList items;
        if (impl_->formatter) {
            items.reserve(1 + impl_->extras.size());
            items.push_back(impl_->formatter->ExtractLoggerItem());
            for (auto& e : impl_->extras) {
                if (e) items.push_back(e->ExtractLoggerItem());
            }
        }
        std::unique_ptr<sinks::LogRecord> structured =
            impl_->recorder ? impl_->recorder->Release() : nullptr;

        impl_->logger_ref.LogMulti(impl_->level, std::move(items), std::move(structured));
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
