#pragma once

/// @file ulog/impl/logger_base.hpp
/// @brief Base logger interface.

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/container/small_vector.hpp>
#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include <ulog/format.hpp>
#include <ulog/fwd.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/level.hpp>
#include <ulog/log_helper.hpp>
#include <ulog/sinks/structured_sink.hpp>
#include <ulog/tracing_hook.hpp>

namespace ulog::impl {

class TextLoggerBase;  // defined below

using formatters::LoggerItemBase;
using formatters::LoggerItemRef;

/// Inline-capacity 1 small_vector — multi-format records carry one item per
/// active format. The common case (single format) avoids heap allocation
/// for the carrier itself.
using LogItemList = boost::container::small_vector<std::unique_ptr<LoggerItemBase>, 1>;

/// Base class for all loggers. Thread-safe level access via atomics.
class LoggerBase {
public:
    virtual ~LoggerBase();

    /// Writes a formatted log item. Takes ownership — synchronous loggers
    /// consume immediately, asynchronous loggers move the pointer into
    /// their queue.
    virtual void Log(Level level, std::unique_ptr<LoggerItemBase> item) = 0;

    /// Multi-format + optional structured variant. `items[i]` was rendered
    /// with format `GetActiveFormats()[i]`; `structured` is the raw record
    /// accumulated for StructuredSink consumers (non-null iff the logger
    /// has at least one structured sink registered). LogHelper always
    /// routes through this single entry point so async loggers can pack
    /// both halves into one queue entry.
    ///
    /// Default: route the first item to `Log(level, item)` (covers
    /// single-format non-Text loggers), then dispatch `structured`
    /// through `LogStructured` for the (rare) subclass that mixes text
    /// and structured paths.
    virtual void LogMulti(Level level,
                          LogItemList items,
                          std::unique_ptr<sinks::LogRecord> structured = nullptr) {
        if (!items.empty() && items[0]) Log(level, std::move(items[0]));
        if (structured) LogStructured(level, std::move(structured));
    }

    /// Consumes a structured record on its own. Default implementation
    /// drops it — loggers without structured sinks never have their
    /// LogHelper build a record in the first place. SyncLogger and
    /// AsyncLogger override to dispatch to the sinks they hold.
    virtual void LogStructured(Level /*level*/, std::unique_ptr<sinks::LogRecord> /*record*/) {}

    /// Reports whether this logger owns any text sinks (sinks::BaseSink).
    /// LogHelper consults this to decide whether to materialize a
    /// formatter at all — a logger with only structured sinks skips the
    /// formatter path entirely. Default true preserves legacy behaviour
    /// (Mem/Null loggers leave the flag as-is).
    ///
    /// Non-virtual fast-path read — atomic acquire load, no lock. Sync
    /// and AsyncLogger update the flag under AddSink.
    bool HasTextSinks() const noexcept {
        return has_text_sinks_.load(std::memory_order_acquire);
    }

    /// Reports whether this logger owns any structured sinks. LogHelper
    /// consults this to decide whether to build a `sinks::LogRecord`.
    /// Default false — Sync/AsyncLogger flip it on `AddStructuredSink`.
    bool HasStructuredSinks() const noexcept {
        return has_structured_sinks_.load(std::memory_order_acquire);
    }

    /// Downcast accessor — returns `this` cast to `TextLoggerBase*` for
    /// loggers that derive from it, nullptr otherwise. LogHelper uses
    /// this to reach multi-format / emit_location state without paying
    /// for RTTI (`dynamic_cast`) on every record. Single vtable slot.
    virtual TextLoggerBase* AsTextLoggerBase() noexcept { return nullptr; }
    virtual const TextLoggerBase* AsTextLoggerBase() const noexcept { return nullptr; }

    /// Flushes pending output (if any).
    virtual void Flush() = 0;

    /// Scratch buffer size reserved by `LogHelper::Impl` for inline
    /// formatter construction. Large enough to fit every built-in
    /// formatter; a logger that wants a custom formatter exceeding this
    /// budget falls back to a heap allocation inside `MakeFormatterInto`.
    static constexpr std::size_t kInlineFormatterSize = 256;
    static constexpr std::size_t kInlineFormatterAlign = 16;

    /// Creates a formatter suited to this logger's text format.
    /// `location` carries the call-site file / function / line (from
    /// `LogRecordLocation::Current()`). Implementations try to
    /// placement-new the formatter into `scratch` (capacity/alignment
    /// guaranteed by `LogHelper` to meet the constants above) — when that
    /// works the returned `BasePtr` carries a destroy-only deleter so
    /// LogHelper pays zero heap allocations for the formatter. If the
    /// scratch is too small (custom big formatter, or the caller passed
    /// null/undersized), the implementation falls back to `new` and the
    /// deleter reclaims memory on destruction.
    virtual formatters::BasePtr MakeFormatterInto(
        void* scratch,
        std::size_t scratch_size,
        Level level,
        const LogRecordLocation& location) = 0;

    void SetLevel(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }
    Level GetLevel() const noexcept { return level_.load(std::memory_order_relaxed); }

    void SetFlushOn(Level level) noexcept { flush_on_.store(level, std::memory_order_relaxed); }
    Level GetFlushOn() const noexcept { return flush_on_.load(std::memory_order_relaxed); }

    bool ShouldLog(Level level) const noexcept {
        return static_cast<int>(level) >= static_cast<int>(GetLevel());
    }

    /// Per-logger common tags hook. LogHelper invokes this after global
    /// `RecordEnricher`s and before the user text is mirrored to
    /// formatters, so the tag appears alongside structured / text
    /// output on every emitted record.
    ///
    /// Default implementation walks the snapshot populated via
    /// `SetCommonTag` / `RemoveCommonTag` — concrete loggers that
    /// derive runtime-computed tags (ambient thread-local context,
    /// formatter-specific metadata) may override instead.
    ///
    /// Writes are lock-free via copy-on-write (`AddCommonTag` publishes
    /// a fresh snapshot under a mutex; readers load the atomic
    /// `shared_ptr` and walk it without locking). Snapshot pins kept by
    /// concurrent emissions stay valid until released — the underlying
    /// vector is immutable after publish.
    virtual void PrependCommonTags(TagSink& sink) const noexcept;

    /// Publishes `key=value` onto the logger's common-tags snapshot.
    /// Idempotent per key — a subsequent `SetCommonTag` with the same
    /// key overwrites the value. Thread-safe; readers never block.
    void SetCommonTag(std::string_view key, std::string_view value);

    /// Removes the tag published under `key`, if any. Thread-safe.
    void RemoveCommonTag(std::string_view key);

    /// Drops every common tag on this logger. Thread-safe.
    void ClearCommonTags() noexcept;

protected:
    LoggerBase() = default;

    /// Concrete loggers publish whether they currently own any text sinks.
    /// Bumped by AddSink (true) and surfaced to LogHelper via
    /// HasTextSinks(). Release on write, acquire on read.
    void SetHasTextSinks(bool v) noexcept {
        has_text_sinks_.store(v, std::memory_order_release);
    }

    /// Mirror of SetHasTextSinks for structured sinks. Default flag
    /// value is false — only loggers that actually support structured
    /// sinks ever flip it.
    void SetHasStructuredSinks(bool v) noexcept {
        has_structured_sinks_.store(v, std::memory_order_release);
    }

private:
    /// Storage for common tags — vector<pair<key, value>> behind an
    /// atomic shared_ptr for lock-free snapshot reads. `nullptr` when
    /// no tags were ever set (fast path — avoids the pin cost).
    using CommonTagsVec = std::vector<std::pair<std::string, std::string>>;

    std::atomic<Level> level_{Level::kInfo};
    std::atomic<Level> flush_on_{Level::kWarning};
    std::atomic<bool> has_text_sinks_{true};
    std::atomic<bool> has_structured_sinks_{false};
    /// Serialises publication of `common_tags_`. Readers do not lock.
    mutable std::mutex common_tags_mu_;
    /// Atomic snapshot of the common-tag list. Copy-on-write: every
    /// SetCommonTag / RemoveCommonTag rebuilds the vector and publishes
    /// a fresh `shared_ptr`. Readers load + walk without locking.
    boost::atomic_shared_ptr<const CommonTagsVec> common_tags_;
};

/// Logger that produces textual output via one of the built-in formatters
/// (TSKV/LTSV/RAW/JSON). Dispatches `MakeFormatter` by format.
///
/// Supports per-sink format overrides: each sink registers the format it
/// wants via `RegisterSinkFormat`, returning a stable index into the
/// `GetActiveFormats()` list. LogHelper consults the list to materialize
/// one formatter per distinct format and pass the resulting items to
/// `LogMulti`. The index is what concrete loggers (Sync/Async) store
/// alongside each sink to route.
class TextLoggerBase : public LoggerBase {
public:
    explicit TextLoggerBase(Format format,
                            bool emit_location = true,
                            TimestampFormat ts_fmt = TimestampFormat::kIso8601Micro);

    TextLoggerBase* AsTextLoggerBase() noexcept override { return this; }
    const TextLoggerBase* AsTextLoggerBase() const noexcept override { return this; }

    Format GetFormat() const noexcept { return format_; }
    bool GetEmitLocation() const noexcept { return emit_location_; }
    TimestampFormat GetTimestampFormat() const noexcept { return ts_fmt_; }

    /// Creates a formatter for this logger's base format (index 0).
    formatters::BasePtr MakeFormatterInto(void* scratch,
                                          std::size_t scratch_size,
                                          Level level,
                                          const LogRecordLocation& location) override;

    /// Creates a formatter for an explicit format (no scratch fast-path,
    /// always heap-allocates). Used by LogHelper for extra formats beyond
    /// the primary.
    formatters::BasePtr MakeFormatterForFormat(Format fmt,
                                               Level level,
                                               const LogRecordLocation& location);

    /// Registers the format a sink wants. `override` unset → sink uses the
    /// logger's base format (index 0). If the format is already active its
    /// existing index is returned; otherwise the list is extended.
    ///
    /// Thread-safe. Indexes are stable for the logger's lifetime (the list
    /// is append-only).
    std::size_t RegisterSinkFormat(std::optional<Format> override);

    /// Snapshot copy of the active formats. Index 0 is always the base
    /// format; subsequent entries are the distinct overrides in
    /// registration order. Heap-allocates a fresh vector; prefer
    /// `GetActiveFormatsSnapshot()` on hot paths.
    std::vector<Format> GetActiveFormats() const;

    /// Immutable pinned snapshot — lock-free atomic load, returns a
    /// shared_ptr to the currently active format list. The list itself
    /// never mutates (registry is copy-on-write); subsequent
    /// RegisterSinkFormat calls publish a fresh vector without
    /// invalidating already-held snapshots. Hot path on LogHelper.
    boost::shared_ptr<std::vector<Format> const> GetActiveFormatsSnapshot() const noexcept;

    /// Number of active formats. Atomic load; no allocation.
    std::size_t GetActiveFormatCount() const noexcept {
        return active_format_count_.load(std::memory_order_acquire);
    }

private:
    Format format_;
    bool emit_location_;
    TimestampFormat ts_fmt_;
    /// Serializes RegisterSinkFormat publishes. Readers do not lock —
    /// they load the atomic shared_ptr below.
    mutable std::mutex formats_mu_;
    /// Copy-on-write snapshot of the active format list. RegisterSinkFormat
    /// publishes a fresh vector under `formats_mu_`; readers take an
    /// atomic load (no lock, no copy).
    boost::atomic_shared_ptr<std::vector<Format> const> active_formats_;
    /// Fast-path count mirror — published together with active_formats_
    /// so callers that only need the size don't have to pin the shared_ptr.
    std::atomic<std::size_t> active_format_count_{1};
};

}  // namespace ulog::impl
