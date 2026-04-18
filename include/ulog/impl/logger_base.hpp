#pragma once

/// @file ulog/impl/logger_base.hpp
/// @brief Base logger interface.

#include <atomic>
#include <memory>
#include <string_view>

#include <ulog/format.hpp>
#include <ulog/fwd.hpp>
#include <ulog/level.hpp>

namespace ulog::impl {

/// Opaque per-record payload handed between formatter and logger.
struct LoggerItemBase {
    virtual ~LoggerItemBase() = default;
};
using LoggerItemRef = LoggerItemBase&;

namespace formatters {
/// Base interface for formatter implementations; real one arrives with Phase 3.
class Base {
public:
    virtual ~Base() = default;
};
using BasePtr = std::unique_ptr<Base>;
}  // namespace formatters

/// Base class for all loggers. Thread-safe level access via atomics.
class LoggerBase {
public:
    virtual ~LoggerBase();

    /// Writes a formatted log item. Payload ownership belongs to caller.
    virtual void Log(Level level, LoggerItemRef item) = 0;

    /// Flushes pending output (if any).
    virtual void Flush() = 0;

    /// Creates a new formatter matching this logger's configuration.
    virtual formatters::BasePtr MakeFormatter(Level level, std::string_view text) = 0;

    void SetLevel(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }
    Level GetLevel() const noexcept { return level_.load(std::memory_order_relaxed); }

    /// Levels at which to auto-flush synchronously.
    void SetFlushOn(Level level) noexcept { flush_on_.store(level, std::memory_order_relaxed); }
    Level GetFlushOn() const noexcept { return flush_on_.load(std::memory_order_relaxed); }

    /// Whether a record at the given level should be emitted by this logger.
    bool ShouldLog(Level level) const noexcept {
        return static_cast<int>(level) >= static_cast<int>(GetLevel());
    }

protected:
    LoggerBase() = default;

private:
    std::atomic<Level> level_{Level::kInfo};
    std::atomic<Level> flush_on_{Level::kWarning};
};

/// Logger that produces textual output via pluggable formatters.
class TextLoggerBase : public LoggerBase {
public:
    explicit TextLoggerBase(Format format) noexcept : format_(format) {}
    Format GetFormat() const noexcept { return format_; }
    formatters::BasePtr MakeFormatter(Level level, std::string_view text) override;

private:
    Format format_;
};

}  // namespace ulog::impl
