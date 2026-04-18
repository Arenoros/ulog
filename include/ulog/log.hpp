#pragma once

/// @file ulog/log.hpp
/// @brief LOG_* macros and free functions — public logging entry points.
///
/// Macro names (LOG_INFO, LOG_ERROR, ...) are preserved from userver.
/// Define ULOG_NO_SHORT_MACROS to suppress short names and expose only
/// the prefixed ULOG_LOG_* variants for use with conflicting codebases
/// (e.g. glog).

#include <chrono>
#include <cstdint>
#include <string_view>

#include <ulog/fwd.hpp>
#include <ulog/level.hpp>
#include <ulog/log_helper.hpp>

namespace ulog {

// -------------------------------------------------------------------------
// Default logger management
// -------------------------------------------------------------------------

/// Returns the default logger previously set by SetDefaultLogger. If the logger
/// was not set — returns a null logger that does no logging.
LoggerRef GetDefaultLogger() noexcept;

/// Replaces the default logger. The provided LoggerPtr is kept alive.
void SetDefaultLogger(LoggerPtr new_default_logger) noexcept;

/// Sets the level of the default logger.
void SetDefaultLoggerLevel(Level level);

/// Returns the level of the default logger.
Level GetDefaultLoggerLevel() noexcept;

/// Returns true iff the default logger will emit records at `level`.
bool ShouldLog(Level level) noexcept;

/// Changes level of any logger.
void SetLoggerLevel(LoggerRef logger, Level level);

/// True iff the given logger will emit records at `level`.
bool LoggerShouldLog(LoggerRef logger, Level level) noexcept;
bool LoggerShouldLog(const LoggerPtr& logger, Level level) noexcept;

/// Returns the current level of the given logger.
Level GetLoggerLevel(LoggerRef logger) noexcept;

/// Forces a flush of the default logger.
void LogFlush();
void LogFlush(LoggerRef logger);

/// RAII guard: atomically replaces the default logger for a scope.
class DefaultLoggerGuard final {
public:
    explicit DefaultLoggerGuard(LoggerPtr new_default_logger) noexcept;
    ~DefaultLoggerGuard();
    DefaultLoggerGuard(const DefaultLoggerGuard&) = delete;
    DefaultLoggerGuard& operator=(const DefaultLoggerGuard&) = delete;

private:
    LoggerPtr prev_ptr_;
    LoggerPtr new_ptr_;
    Level prev_level_;
};

/// RAII guard: temporarily overrides the default logger's level.
class DefaultLoggerLevelScope final {
public:
    explicit DefaultLoggerLevelScope(Level level);
    ~DefaultLoggerLevelScope();
    DefaultLoggerLevelScope(const DefaultLoggerLevelScope&) = delete;
    DefaultLoggerLevelScope& operator=(const DefaultLoggerLevelScope&) = delete;

private:
    impl::LoggerBase& logger_;
    Level initial_;
};

namespace impl {

/// Per-source-line counter for LOG_LIMITED_*. Thread-local.
class RateLimitData {
public:
    std::uint64_t count_since_reset{0};
    std::uint64_t dropped_count{0};
    std::chrono::steady_clock::time_point last_reset_time{};
};

class RateLimiter {
public:
    explicit RateLimiter(RateLimitData& data) noexcept;
    bool ShouldLog() const noexcept { return should_log_; }
    std::uint64_t GetDroppedCount() const noexcept { return dropped_count_; }

private:
    bool should_log_{true};
    std::uint64_t dropped_count_{0};
};

/// Noop helper returned from compile-erased macros.
struct Noop {
    template <typename T>
    const Noop& operator<<(const T&) const noexcept {
        return *this;
    }
};

/// Static per-source-line registration for dynamic debug controls.
class StaticLogEntry final {
public:
    StaticLogEntry(const char* path, int line) noexcept;
    StaticLogEntry(const StaticLogEntry&) = delete;
    StaticLogEntry& operator=(const StaticLogEntry&) = delete;

    bool ShouldNotLog(LoggerRef logger, Level level) const noexcept;
    bool ShouldNotLog(const LoggerPtr& logger, Level level) const noexcept;
};

template <class NameHolder, int Line>
struct EntryStorage final {
    static inline StaticLogEntry entry{NameHolder::Get(), Line};
};

}  // namespace impl

}  // namespace ulog

// -------------------------------------------------------------------------
// Macro implementation
// -------------------------------------------------------------------------

// NOLINTBEGIN(cppcoreguidelines-macro-usage)

#define ULOG_IMPL_DYNAMIC_DEBUG_ENTRY                                                        \
    []() noexcept -> const ::ulog::impl::StaticLogEntry& {                                   \
        struct NameHolder {                                                                  \
            static constexpr const char* Get() noexcept { return __FILE__; }                 \
        };                                                                                   \
        const auto& entry = ::ulog::impl::EntryStorage<NameHolder, __LINE__>::entry;         \
        return entry;                                                                        \
    }

#define ULOG_IMPL_LOG_LOCATION() \
    ::ulog::LogRecordLocation { __FILE__, __LINE__, static_cast<const char*>(__func__) }

#define ULOG_IMPL_LOG_TO(logger, level, ...) \
    ::ulog::LogHelper((logger), (level), ULOG_IMPL_LOG_LOCATION())

// Dynamic-debug aware, ShouldLog-guarded record builder.
// Expands to a `for` statement that runs exactly once iff logging is enabled.
// Chaining `<< value` attaches to the LogHelper in the loop body.
#define ULOG_LOG_TO(logger, lvl, ...)                                                        \
    for (bool ulog_once__ = !ULOG_IMPL_DYNAMIC_DEBUG_ENTRY().ShouldNotLog((logger), (lvl));  \
         ulog_once__; ulog_once__ = false)                                                   \
        ULOG_IMPL_LOG_TO((logger), (lvl), __VA_ARGS__)

#define ULOG_LOG(lvl, ...) ULOG_LOG_TO(::ulog::GetDefaultLogger(), (lvl), __VA_ARGS__)

// ---- Compile-time erase by level (ULOG_ERASE_LOG_WITH_LEVEL=N) ----
#if defined(ULOG_ERASE_LOG_WITH_LEVEL)
#define ULOG_IMPL_ERASE(level_int, logger, ...)                                  \
    for (bool ulog_once__ = false; ulog_once__; ulog_once__ = false)             \
        ULOG_IMPL_LOG_TO((logger), ::ulog::Level::kTrace, __VA_ARGS__)
#define ULOG_IMPL_LOGS_TRACE_ERASER(MACRO, LOGGER, ...)                          \
    ULOG_IMPL_ERASE(0, LOGGER, __VA_ARGS__)
#if ULOG_ERASE_LOG_WITH_LEVEL > 0
#define ULOG_IMPL_LOGS_DEBUG_ERASER(MACRO, LOGGER, ...)                          \
    ULOG_IMPL_ERASE(1, LOGGER, __VA_ARGS__)
#endif
#if ULOG_ERASE_LOG_WITH_LEVEL > 1
#define ULOG_IMPL_LOGS_INFO_ERASER(MACRO, LOGGER, ...)                           \
    ULOG_IMPL_ERASE(2, LOGGER, __VA_ARGS__)
#endif
#if ULOG_ERASE_LOG_WITH_LEVEL > 2
#define ULOG_IMPL_LOGS_WARNING_ERASER(MACRO, LOGGER, ...)                        \
    ULOG_IMPL_ERASE(3, LOGGER, __VA_ARGS__)
#endif
#if ULOG_ERASE_LOG_WITH_LEVEL > 3
#define ULOG_IMPL_LOGS_ERROR_ERASER(MACRO, LOGGER, ...)                          \
    ULOG_IMPL_ERASE(4, LOGGER, __VA_ARGS__)
#endif
#endif  // ULOG_ERASE_LOG_WITH_LEVEL

#ifndef ULOG_IMPL_LOGS_TRACE_ERASER
#define ULOG_IMPL_LOGS_TRACE_ERASER(MACRO, LOGGER, ...) MACRO(LOGGER, ::ulog::Level::kTrace, __VA_ARGS__)
#endif
#ifndef ULOG_IMPL_LOGS_DEBUG_ERASER
#define ULOG_IMPL_LOGS_DEBUG_ERASER(MACRO, LOGGER, ...) MACRO(LOGGER, ::ulog::Level::kDebug, __VA_ARGS__)
#endif
#ifndef ULOG_IMPL_LOGS_INFO_ERASER
#define ULOG_IMPL_LOGS_INFO_ERASER(MACRO, LOGGER, ...) MACRO(LOGGER, ::ulog::Level::kInfo, __VA_ARGS__)
#endif
#ifndef ULOG_IMPL_LOGS_WARNING_ERASER
#define ULOG_IMPL_LOGS_WARNING_ERASER(MACRO, LOGGER, ...) MACRO(LOGGER, ::ulog::Level::kWarning, __VA_ARGS__)
#endif
#ifndef ULOG_IMPL_LOGS_ERROR_ERASER
#define ULOG_IMPL_LOGS_ERROR_ERASER(MACRO, LOGGER, ...) MACRO(LOGGER, ::ulog::Level::kError, __VA_ARGS__)
#endif

// -------- Long-named (always-available) macros --------
#define ULOG_LOG_TRACE(...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_DEBUG(...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_INFO(...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_WARNING(...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_ERROR(...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_CRITICAL(...) ULOG_LOG(::ulog::Level::kCritical, __VA_ARGS__)

#define ULOG_LOG_TRACE_TO(logger, ...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LOG_TO, logger, __VA_ARGS__)
#define ULOG_LOG_DEBUG_TO(logger, ...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LOG_TO, logger, __VA_ARGS__)
#define ULOG_LOG_INFO_TO(logger, ...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LOG_TO, logger, __VA_ARGS__)
#define ULOG_LOG_WARNING_TO(logger, ...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LOG_TO, logger, __VA_ARGS__)
#define ULOG_LOG_ERROR_TO(logger, ...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LOG_TO, logger, __VA_ARGS__)
#define ULOG_LOG_CRITICAL_TO(logger, ...) ULOG_LOG_TO(logger, ::ulog::Level::kCritical, __VA_ARGS__)

// -------- Rate-limited variants --------
#define ULOG_LOG_LIMITED_TO(logger, lvl, ...)                                                                \
    if (const ::ulog::impl::RateLimiter ulog_rl__ {                                                          \
        []() -> ::ulog::impl::RateLimitData& {                                                               \
            thread_local ::ulog::impl::RateLimitData d;                                                      \
            return d;                                                                                        \
        }()                                                                                                  \
    }; !ulog_rl__.ShouldLog()) {                                                                             \
    } else                                                                                                   \
        ULOG_LOG_TO((logger), (lvl), __VA_ARGS__) << ulog_rl__

#define ULOG_LOG_LIMITED(lvl, ...)          ULOG_LOG_LIMITED_TO(::ulog::GetDefaultLogger(), (lvl), __VA_ARGS__)

#define ULOG_LOG_LIMITED_TRACE(...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_DEBUG(...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_INFO(...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_WARNING(...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_ERROR(...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLogger(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_CRITICAL(...) ULOG_LOG_LIMITED(::ulog::Level::kCritical, __VA_ARGS__)

#define ULOG_LOG_LIMITED_TRACE_TO(logger, ...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_DEBUG_TO(logger, ...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_INFO_TO(logger, ...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_WARNING_TO(logger, ...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_ERROR_TO(logger, ...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_CRITICAL_TO(logger, ...) ULOG_LOG_LIMITED_TO(logger, ::ulog::Level::kCritical, __VA_ARGS__)

// -------- Short (default) macro aliases — preserved from userver --------
#ifndef ULOG_NO_SHORT_MACROS

#define LOG_TO(logger, lvl, ...)           ULOG_LOG_TO(logger, lvl, __VA_ARGS__)
#define LOG(lvl, ...)                      ULOG_LOG(lvl, __VA_ARGS__)

#define LOG_TRACE(...)                     ULOG_LOG_TRACE(__VA_ARGS__)
#define LOG_DEBUG(...)                     ULOG_LOG_DEBUG(__VA_ARGS__)
#define LOG_INFO(...)                      ULOG_LOG_INFO(__VA_ARGS__)
#define LOG_WARNING(...)                   ULOG_LOG_WARNING(__VA_ARGS__)
#define LOG_ERROR(...)                     ULOG_LOG_ERROR(__VA_ARGS__)
#define LOG_CRITICAL(...)                  ULOG_LOG_CRITICAL(__VA_ARGS__)

#define LOG_TRACE_TO(logger, ...)          ULOG_LOG_TRACE_TO(logger, __VA_ARGS__)
#define LOG_DEBUG_TO(logger, ...)          ULOG_LOG_DEBUG_TO(logger, __VA_ARGS__)
#define LOG_INFO_TO(logger, ...)           ULOG_LOG_INFO_TO(logger, __VA_ARGS__)
#define LOG_WARNING_TO(logger, ...)        ULOG_LOG_WARNING_TO(logger, __VA_ARGS__)
#define LOG_ERROR_TO(logger, ...)          ULOG_LOG_ERROR_TO(logger, __VA_ARGS__)
#define LOG_CRITICAL_TO(logger, ...)       ULOG_LOG_CRITICAL_TO(logger, __VA_ARGS__)

#define LOG_LIMITED_TO(logger, lvl, ...)   ULOG_LOG_LIMITED_TO(logger, lvl, __VA_ARGS__)
#define LOG_LIMITED(lvl, ...)              ULOG_LOG_LIMITED(lvl, __VA_ARGS__)

#define LOG_LIMITED_TRACE(...)             ULOG_LOG_LIMITED_TRACE(__VA_ARGS__)
#define LOG_LIMITED_DEBUG(...)             ULOG_LOG_LIMITED_DEBUG(__VA_ARGS__)
#define LOG_LIMITED_INFO(...)              ULOG_LOG_LIMITED_INFO(__VA_ARGS__)
#define LOG_LIMITED_WARNING(...)           ULOG_LOG_LIMITED_WARNING(__VA_ARGS__)
#define LOG_LIMITED_ERROR(...)             ULOG_LOG_LIMITED_ERROR(__VA_ARGS__)
#define LOG_LIMITED_CRITICAL(...)          ULOG_LOG_LIMITED_CRITICAL(__VA_ARGS__)

#define LOG_LIMITED_TRACE_TO(logger, ...)    ULOG_LOG_LIMITED_TRACE_TO(logger, __VA_ARGS__)
#define LOG_LIMITED_DEBUG_TO(logger, ...)    ULOG_LOG_LIMITED_DEBUG_TO(logger, __VA_ARGS__)
#define LOG_LIMITED_INFO_TO(logger, ...)     ULOG_LOG_LIMITED_INFO_TO(logger, __VA_ARGS__)
#define LOG_LIMITED_WARNING_TO(logger, ...)  ULOG_LOG_LIMITED_WARNING_TO(logger, __VA_ARGS__)
#define LOG_LIMITED_ERROR_TO(logger, ...)    ULOG_LOG_LIMITED_ERROR_TO(logger, __VA_ARGS__)
#define LOG_LIMITED_CRITICAL_TO(logger, ...) ULOG_LOG_LIMITED_CRITICAL_TO(logger, __VA_ARGS__)

#endif  // !ULOG_NO_SHORT_MACROS

// NOLINTEND(cppcoreguidelines-macro-usage)
