#pragma once

/// @file ulog/impl/logger_base.hpp
/// @brief Base logger interface.

#include <atomic>
#include <memory>
#include <string_view>

#include <ulog/format.hpp>
#include <ulog/fwd.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/level.hpp>

namespace ulog::impl {

using formatters::LoggerItemBase;
using formatters::LoggerItemRef;

/// Base class for all loggers. Thread-safe level access via atomics.
class LoggerBase {
public:
    virtual ~LoggerBase();

    /// Writes a formatted log item.
    virtual void Log(Level level, LoggerItemRef item) = 0;

    /// Flushes pending output (if any).
    virtual void Flush() = 0;

    /// Creates a formatter suited to this logger's text format.
    /// `module_function`, `module_file`, `module_line` describe the call site
    /// (from the LOG_* macro expansion). `tp` is the record's timestamp.
    virtual formatters::BasePtr MakeFormatter(Level level,
                                              std::string_view module_function,
                                              std::string_view module_file,
                                              int module_line) = 0;

    void SetLevel(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }
    Level GetLevel() const noexcept { return level_.load(std::memory_order_relaxed); }

    void SetFlushOn(Level level) noexcept { flush_on_.store(level, std::memory_order_relaxed); }
    Level GetFlushOn() const noexcept { return flush_on_.load(std::memory_order_relaxed); }

    bool ShouldLog(Level level) const noexcept {
        return static_cast<int>(level) >= static_cast<int>(GetLevel());
    }

protected:
    LoggerBase() = default;

private:
    std::atomic<Level> level_{Level::kInfo};
    std::atomic<Level> flush_on_{Level::kWarning};
};

/// Logger that produces textual output via one of the built-in formatters
/// (TSKV/LTSV/RAW/JSON). Dispatches `MakeFormatter` by format.
class TextLoggerBase : public LoggerBase {
public:
    explicit TextLoggerBase(Format format) noexcept : format_(format) {}
    Format GetFormat() const noexcept { return format_; }
    formatters::BasePtr MakeFormatter(Level level,
                                      std::string_view module_function,
                                      std::string_view module_file,
                                      int module_line) override;

private:
    Format format_;
};

}  // namespace ulog::impl
