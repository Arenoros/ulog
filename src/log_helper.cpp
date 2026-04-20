#include <ulog/log_helper.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
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

/// Thread-local object cache. Amortizes heap allocation of `T` across
/// logs on the same thread: first `MaxCapacity` records in a thread
/// call `new Storage`; subsequent records reuse freed slabs. `Push` in
/// the record dtor returns the storage to the cache; when full, the
/// slab is freed normally. Thread-exit cleanup: `LocalState` dtor walks
/// remaining slabs and frees them (`T` objects have already been
/// destroyed by the corresponding `Push`).
///
/// Based on `userver::logging::ThreadLocalMemPool`
/// (universal/src/logging/log_helper.cpp) with `thread_local` static
/// instead of userver's `compiler::ThreadLocal` abstraction.
///
/// Thread-safety: per-thread state, no cross-thread synchronization. A
/// `Push` must happen on the *same thread* that called `Pop`, otherwise
/// the slab leaks into the wrong cache (still memory-safe — the `T` is
/// destroyed first and the raw storage is trivially freeable — but the
/// pool amortization is lost for that pair of logs).
template <typename T, std::size_t MaxCapacity = 16>
class ThreadLocalMemPool {
    // Push() calls std::destroy_at under noexcept; a throwing dtor would
    // violate the contract and terminate. Guard at compile time.
    static_assert(std::is_nothrow_destructible_v<T>,
                  "ThreadLocalMemPool<T>: T destructor must be noexcept");
public:
    template <typename... Args>
    static std::unique_ptr<T> Pop(Args&&... args) {
        auto& pool = LocalPool();
        Storage* raw;
        if (pool.count == 0) {
            raw = new Storage;  // uninitialized — placement-new below
        } else {
            // Take top of stack; release() transfers ownership out of the
            // unique_ptr without invoking its deleter (which would free the
            // raw memory we want to reuse).
            raw = pool.slabs[--pool.count].release();
        }
        try {
            T* obj = ::new (static_cast<void*>(raw)) T(std::forward<Args>(args)...);
            return std::unique_ptr<T>(obj);
        } catch (...) {
            delete raw;  // Storage is trivially destructible; frees memory.
            throw;
        }
    }

    /// Return an object to the per-thread cache. Destroys `*obj`
    /// unconditionally; if the cache is full, frees the storage. Always
    /// leaves `obj` null on exit.
    static void Push(std::unique_ptr<T> obj) noexcept {
        if (!obj) return;
        auto& pool = LocalPool();
        if (pool.count == MaxCapacity) {
            // Cache full — let the default deleter free everything at
            // scope exit. Explicit reset() to make the dtor chain run
            // before we return (cheaper than deferring it to the
            // caller's stack frame).
            obj.reset();
            return;
        }
        // Disarm the default deleter: we'll destroy T manually and keep
        // the raw storage alive in the cache.
        T* p = obj.release();
        std::destroy_at(p);
        // sizeof/alignof(Storage) == sizeof/alignof(T) by construction,
        // so reinterpret to Storage* is well-defined; `delete` on this
        // pointer later (via Storage unique_ptr) uses the correct size
        // and alignment.
        pool.slabs[pool.count++].reset(reinterpret_cast<Storage*>(p));
    }

private:
    struct alignas(T) Storage {
        std::byte raw[sizeof(T)];
    };

    struct LocalState {
        std::array<std::unique_ptr<Storage>, MaxCapacity> slabs{};
        std::size_t count{0};
        // Implicit dtor: the array's unique_ptrs free their slabs on
        // thread exit. Objects inside were destroyed by earlier Push().
    };

    static LocalState& LocalPool() noexcept {
        thread_local LocalState state;
        return state;
    }
};

/// Hard per-record text cap. Matches userver's `kSizeLimit` so services
/// ported across the stacks cannot produce silently diverging log sizes.
/// Exceeding this causes subsequent streaming writes to be dropped and
/// the emitted record to carry `truncated=true`.
constexpr std::size_t kSizeLimit = 10000;

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
        // Single vtable slot — cheaper than dynamic_cast; null for
        // non-text loggers. Shared by text / structured setup paths.
        impl::TextLoggerBase* text_base = logger_ref.AsTextLoggerBase();
        PrepareTextFormatters(text_base);
        PrepareStructuredRecorder(text_base);
        SelectWriterTarget();
    }

    /// Materialise the primary formatter (placement-new into inline
    /// scratch when it fits) plus any extra formatters registered
    /// through `TextLoggerBase::RegisterSinkFormat` for per-sink
    /// format overrides. No-op when the logger has no text sinks.
    void PrepareTextFormatters(impl::TextLoggerBase* text_base) {
        if (!logger_ref.HasTextSinks()) return;

        formatter = logger_ref.MakeFormatterInto(
            &formatter_scratch,
            sizeof(formatter_scratch),
            level, location);

        // Extras engage only when the logger has registered more than
        // the primary format. Atomic count check short-circuits the
        // snapshot pin for the single-format hot path. The snapshot,
        // once taken, is immutable (COW registry) — a concurrent
        // AddSink publishes a new vector; already-pinned vectors stay
        // valid for the LogHelper's lifetime. Sinks appended after we
        // pinned are invisible to this record; their format_idx may
        // overshoot items.size() and LogMulti skips them via the
        // out-of-range check.
        if (!text_base || text_base->GetActiveFormatCount() <= 1) return;
        auto snap = text_base->GetActiveFormatsSnapshot();
        if (!snap || snap->size() <= 1) return;
        extras.reserve(snap->size() - 1);
        for (std::size_t i = 1; i < snap->size(); ++i) {
            extras.push_back(text_base->MakeFormatterForFormat(
                (*snap)[i], level, location));
        }
    }

    /// Seed the structured record with level / timestamp / call-site
    /// metadata; tags and text land later through the fanout / SetText
    /// path. Respects `TextLoggerBase::GetEmitLocation()` symmetrically
    /// with the text formatter — a logger configured with
    /// `emit_location = false` wants the location suppressed
    /// everywhere, not only in the text `module` field.
    void PrepareStructuredRecorder(impl::TextLoggerBase* text_base) {
        if (!logger_ref.HasStructuredSinks()) return;
        recorder.emplace();
        auto* rec = recorder->mutable_record();
        rec->level = level;
        rec->timestamp = std::chrono::system_clock::now();
        const bool emit_loc = text_base ? text_base->GetEmitLocation() : true;
        if (!emit_loc) return;
        const auto fn = location.function_name();
        const auto fl = location.file_name();
        if (!fn.empty()) rec->module_function.assign(fn);
        if (!fl.empty()) rec->module_file.assign(fl);
        rec->module_line = static_cast<int>(location.line());
    }

    /// Pick the writer target. Fanout engages whenever more than one
    /// broadcast destination exists (primary + extras, or primary +
    /// recorder, or any combination thereof). A single target bypasses
    /// fanout entirely — the common LOG_* hot path. Zero targets
    /// (logger has no sinks at all) leaves the writer null; tag /
    /// trace-hook calls are no-ops on a null formatter pointer.
    void SelectWriterTarget() {
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
    }

    /// Finalize the record: dispatch hooks, mirror text to targets,
    /// extract items, route through the logger, and flush when the
    /// level warrants it. Never throws — any exception is swallowed so
    /// the dtor path stays `noexcept`-safe. No-op when `!active`.
    void DoLog() noexcept {
        // `broken` is set when any streaming call caught an exception; we
        // bail before SetText to avoid emitting a half-rendered record.
        if (!active || broken) return;
        try {
            // Dispatch the tracing hook + record enrichers through whichever
            // target TagWriter currently points to — the fanout when multiple
            // destinations are active, otherwise the sole target (formatter
            // OR recorder). Both satisfy the TagSink / Base contract.
            impl::formatters::Base* hook_target = nullptr;
            if (fanout)         hook_target = &*fanout;
            else if (formatter) hook_target = formatter.get();
            else if (recorder)  hook_target = &*recorder;
            if (hook_target) {
                FormatterTagSink sink(hook_target);
                impl::DispatchTracingHook(sink);
                impl::DispatchRecordEnrichers(sink);
                // Per-logger common tags — layered on top of global
                // enrichers so they appear identically on every record
                // emitted through this particular logger. Default impl
                // in LoggerBase walks the atomic snapshot; loggers may
                // override for runtime-computed tags.
                logger_ref.PrependCommonTags(sink);
            }

            // Surface the size-limit signal through the same writer the
            // hooks used. Guaranteed to go to every active target via the
            // fanout (or the single target when only one is engaged).
            // Null writer target (no sinks) silently drops — harmless.
            if (truncated) writer.PutTag("truncated", "true");

            // SetText mirrors to every destination so each produced item /
            // record carries the same text body.
            const auto text_view = text.view();
            if (formatter) formatter->SetText(text_view);
            for (auto& e : extras) if (e) e->SetText(text_view);
            if (recorder)  recorder->SetText(text_view);

            const bool want_flush = level >= logger_ref.GetFlushOn();

            // Single-format text-only hot path: no extras, no structured.
            // Route straight to Log(level, item) — skips the LogItemList
            // wrap and the LogMulti vector snapshot. This is the LOG_* path
            // for the overwhelming common case (one format, no structured
            // sink).
            if (formatter && extras.empty() && !recorder) {
                auto item = formatter->ExtractLoggerItem();
                logger_ref.Log(level, std::move(item));
                if (want_flush) logger_ref.Flush();
                return;
            }

            impl::LogItemList items;
            if (formatter) {
                items.reserve(1 + extras.size());
                items.push_back(formatter->ExtractLoggerItem());
                for (auto& e : extras) {
                    if (e) items.push_back(e->ExtractLoggerItem());
                }
            }
            std::unique_ptr<sinks::LogRecord> structured =
                recorder ? recorder->Release() : nullptr;

            logger_ref.LogMulti(level, std::move(items), std::move(structured));
            if (want_flush) logger_ref.Flush();
        } catch (...) {
            // Never throw from DoLog — invariant relied on by ~LogHelper.
        }
    }

    /// Called from `LogHelper::InternalLoggingError` when a streaming
    /// op throws: mark the record as broken so `DoLog` skips emit, and
    /// subsequent `<<` calls short-circuit via `broken` early-return
    /// in the Put helpers.
    void MarkAsBroken() noexcept { broken = true; }

    /// Current size of the text buffer. `Put*` consult this against
    /// `kSizeLimit` before each append.
    std::size_t GetTextSize() const noexcept { return text.size(); }

    /// True when the record has already hit (or passed) the size cap.
    /// Used by `Put*` to short-circuit and by callers via
    /// `LogHelper::IsLimitReached`.
    bool IsLimitReached() const noexcept {
        return truncated || text.size() >= kSizeLimit;
    }

    /// Appends `sv` to the text buffer. Gates on broken/active/limit.
    /// Overshoot-by-one-chunk allowance — `sv` is never split; if the
    /// post-append size crosses the cap we flag `truncated=true` so
    /// `DoLog` emits the `truncated=true` tag.
    void AddText(std::string_view sv) noexcept {
        if (!active || broken) return;
        if (text.size() >= kSizeLimit) {
            truncated = true;
            return;
        }
        try {
            text += sv;
        } catch (...) {
            broken = true;
            return;
        }
        if (text.size() >= kSizeLimit) {
            truncated = true;
        }
    }

    /// Appends a single character — same gating as `AddText`. Separate
    /// method avoids constructing a `string_view` for single-char writes
    /// (`operator<<(Quoted)` calls this for the bracketing `"`).
    void AddChar(char c) noexcept {
        if (!active || broken) return;
        if (text.size() >= kSizeLimit) {
            truncated = true;
            return;
        }
        try {
            text += c;
        } catch (...) {
            broken = true;
            return;
        }
        if (text.size() >= kSizeLimit) {
            truncated = true;
        }
    }

    /// Plain string-valued tag. No-op on broken/inactive record;
    /// swallows any exception from the writer via `broken` mark.
    void AddTag(std::string_view key, std::string_view value) noexcept {
        if (!active || broken) return;
        try {
            writer.PutTag(key, value);
        } catch (...) {
            broken = true;
        }
    }

    /// Tag whose value is pre-serialized JSON. Same gating as AddTag.
    void AddJsonTag(std::string_view key, const JsonString& value) noexcept {
        if (!active || broken) return;
        try {
            writer.PutJsonTag(key, value);
        } catch (...) {
            broken = true;
        }
    }

    /// Spills every key-value pair of `extra` through the tag writer.
    void AddLogExtra(const LogExtra& extra) noexcept {
        if (!active || broken) return;
        try {
            writer.PutLogExtra(extra);
        } catch (...) {
            broken = true;
        }
    }

    impl::LoggerBase& logger_ref;
    LoggerPtr logger_owner;                     ///< Keeps the logger alive; may be null for ref path.
    Level level;
    LogRecordLocation location;
    bool active;
    /// True after a streaming op caught an exception. Guards DoLog
    /// against emitting a half-rendered record.
    bool broken{false};
    /// True once a `Put*` call dropped bytes because the buffer reached
    /// `kSizeLimit`. DoLog emits a `truncated=true` tag when set so
    /// downstream consumers can distinguish a clean record from one
    /// that silently lost its tail.
    bool truncated{false};
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

// Pool allocation failure (bad_alloc or Impl ctor throw) leaves impl_
// null and the dtor fast-path silently drops the record. Phase 2 will
// surface the error on stderr via InternalLoggingError.

LogHelper::LogHelper(LoggerRef logger, Level level, const LogRecordLocation& location) noexcept
    : impl_(nullptr) {
    try {
        impl_ = ThreadLocalMemPool<Impl>::Pop(
            logger, LoggerPtr{}, level, location, /*active=*/true);
    } catch (...) {}
}

LogHelper::LogHelper(LoggerRef logger, Level level, NoLog, const LogRecordLocation& location) noexcept
    : impl_(nullptr) {
    try {
        impl_ = ThreadLocalMemPool<Impl>::Pop(
            logger, LoggerPtr{}, level, location, /*active=*/false);
    } catch (...) {}
}

LogHelper::LogHelper(LoggerPtr logger, Level level, const LogRecordLocation& location) noexcept
    : impl_(nullptr) {
    if (!logger) {
        // Pathological case — caller passed null. Drop.
        return;
    }
    auto& ref = *logger;
    try {
        impl_ = ThreadLocalMemPool<Impl>::Pop(
            ref, std::move(logger), level, location, /*active=*/true);
    } catch (...) {}
}

LogHelper::~LogHelper() {
    if (!impl_) return;
    impl_->DoLog();
    ThreadLocalMemPool<Impl>::Push(std::move(impl_));
}

bool LogHelper::IsActive() const noexcept {
    return impl_ && impl_->active && !impl_->broken;
}

impl::TagWriter& LogHelper::GetTagWriter() noexcept {
    if (impl_) return impl_->writer;
    // Pool Pop threw — caller gets a dummy writer with a null target;
    // `PutTag` / `PutLogExtra` are no-ops against a null formatter.
    // thread_local keeps the fallback per-thread so concurrent callers
    // don't race on the same dummy's future-proofed mutable state.
    thread_local impl::TagWriter dummy{nullptr};
    return dummy;
}

void LogHelper::InternalLoggingError(const char* msg) noexcept {
    // Write a short diagnostic line to stderr. Use fputs + \n — no
    // fmt::format (that was the thing that just threw), no iostream
    // (pulls locale, sync overhead). Failures from fputs are ignored:
    // if stderr is closed we can't do anything about it anyway.
    std::fputs("ulog::LogHelper: ", stderr);
    std::fputs(msg ? msg : "internal error", stderr);
    std::fputc('\n', stderr);
    if (impl_) impl_->MarkAsBroken();
}

// ---- text stream appenders ----
//
// LogHelper-level `Put*` are thin delegates to `Impl::AddText` /
// `Impl::AddChar`. All mutation / limit / broken-flag logic lives
// on Impl so the public surface stays minimal and internals don't
// leak into extension operators.

void LogHelper::Put(std::string_view sv) {
    if (impl_) impl_->AddText(sv);
}
void LogHelper::Put(const char* s) {
    if (impl_) impl_->AddText(std::string_view(s ? s : ""));
}
void LogHelper::Put(bool v) {
    if (impl_) impl_->AddText(std::string_view(v ? "true" : "false"));
}
void LogHelper::Put(char v) {
    if (impl_) impl_->AddChar(v);
}
void LogHelper::PutFormatted(std::string s) {
    if (impl_) impl_->AddText(std::string_view(s));
}

void LogHelper::PutTag(std::string_view key, std::string_view value) noexcept {
    if (impl_) impl_->AddTag(key, value);
}
void LogHelper::PutJsonTag(std::string_view key, const JsonString& value) noexcept {
    if (impl_) impl_->AddJsonTag(key, value);
}

bool LogHelper::IsLimitReached() const noexcept {
    return !impl_ || impl_->IsLimitReached();
}

LogHelper& LogHelper::operator<<(const LogExtra& extra) & noexcept {
    if (impl_) impl_->AddLogExtra(extra);
    return *this;
}
LogHelper&& LogHelper::operator<<(const LogExtra& extra) && noexcept {
    *this << extra;
    return std::move(*this);
}

LogHelper& LogHelper::operator<<(Hex v) & noexcept {
    if (IsActive()) {
        try {
            impl_->AddText(fmt::format("0x{:016X}", v.value));
        } catch (...) {
            InternalLoggingError("operator<<(Hex) threw");
        }
    }
    return *this;
}
LogHelper& LogHelper::operator<<(HexShort v) & noexcept {
    if (IsActive()) {
        try {
            impl_->AddText(fmt::format("{:X}", v.value));
        } catch (...) {
            InternalLoggingError("operator<<(HexShort) threw");
        }
    }
    return *this;
}
LogHelper& LogHelper::operator<<(Quoted v) & noexcept {
    if (IsActive()) {
        // Route through the gated appenders so the kSizeLimit cap
        // applies uniformly — otherwise a gigantic quoted payload
        // would bypass truncation.
        impl_->AddChar('"');
        impl_->AddText(v.value);
        impl_->AddChar('"');
    }
    return *this;
}

LogHelper& LogHelper::WithException(const std::exception& ex) & noexcept {
    if (IsActive()) {
        impl_->AddTag("exception_type", typeid(ex).name());
        impl_->AddTag("exception_msg", ex.what());
    }
    return *this;
}

LogHelper& LogHelper::operator<<(const impl::RateLimiter& rl) & noexcept {
    if (IsActive() && rl.GetDroppedCount() > 0) {
        try {
            impl_->AddText(fmt::format(" [dropped={}]", rl.GetDroppedCount()));
        } catch (...) {
            InternalLoggingError("operator<<(RateLimiter) threw");
        }
    }
    return *this;
}

}  // namespace ulog
