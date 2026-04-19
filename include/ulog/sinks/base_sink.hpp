#pragma once

/// @file ulog/sinks/base_sink.hpp
/// @brief Sink interface — a destination for formatted log records.

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include <ulog/level.hpp>
#include <ulog/sinks/sink_stats.hpp>

namespace ulog::sinks {

/// Mode passed to Reopen(). Differentiates manual rotation from log-rotate tool requests.
enum class ReopenMode {
    kTruncate,  ///< Open existing or new file in truncate mode
    kAppend,    ///< Open existing or new file in append mode
};

/// Base class for every sink. Thread-safe per method (each sink guards its
/// own state internally). Level filtering is optional — sinks typically just
/// write; filtering happens in the logger.
class BaseSink {
public:
    virtual ~BaseSink() = default;

    /// Writes one finalized record. Implementations may buffer internally.
    virtual void Write(std::string_view record) = 0;

    /// Flushes any buffered state (default: no-op).
    virtual void Flush() {}

    /// Reopens the underlying resource (file rotation). Default: no-op.
    virtual void Reopen(ReopenMode /*mode*/) {}

    /// Returns a snapshot of per-sink counters. Default: all-zero — only
    /// sinks wrapped in an `InstrumentedSink` decorator record real
    /// numbers. The caller can diff two consecutive snapshots to get
    /// per-interval rates.
    virtual SinkStats GetStats() const noexcept { return {}; }

    /// Per-sink level gate (kNone by default — sink accepts everything).
    /// Applied BEFORE Write is called via ShouldLog().
    void SetLevel(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }
    Level GetLevel() const noexcept { return level_.load(std::memory_order_relaxed); }

    bool ShouldLog(Level msg_level) const noexcept {
        const auto cur = GetLevel();
        if (cur == Level::kNone) return true;
        return static_cast<int>(msg_level) >= static_cast<int>(cur);
    }

protected:
    BaseSink() = default;

private:
    std::atomic<Level> level_{Level::kNone};
};

using SinkPtr = std::shared_ptr<BaseSink>;

}  // namespace ulog::sinks
