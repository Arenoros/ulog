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

#include <fmt/format.h>
#include <fmt/printf.h>

#include <ulog/detail/source_root.hpp>
#include <ulog/fwd.hpp>
#include <ulog/level.hpp>
#include <ulog/log_helper.hpp>

namespace ulog {

// -------------------------------------------------------------------------
// Default logger management
// -------------------------------------------------------------------------

/// Returns the default logger previously set by SetDefaultLogger. If the logger
/// was not set — returns a null logger that does no logging.
///
/// @warning The reference is only safe for the duration of a single logging
/// operation that will not race with SetDefaultLogger. For long-lived access
/// across threads use @ref GetDefaultLoggerPtr, which pins the logger with
/// a shared_ptr snapshot.
///
/// Marked `[[deprecated]]` because the short-lived-reference contract is
/// subtle and easy to miss. Call sites that just want to emit one record
/// should use the LOG_* macros (which internally snapshot a `LoggerPtr`);
/// call sites that need explicit access to the logger should use
/// `GetDefaultLoggerPtr()` so the refcount keeps the logger alive across
/// a concurrent `SetDefaultLogger` swap.
[[deprecated("Prefer GetDefaultLoggerPtr() — returned reference is only valid "
             "until the next SetDefaultLogger() on any thread")]]
LoggerRef GetDefaultLogger() noexcept;

/// Returns a reference-counted snapshot of the current default logger. The
/// returned pointer keeps the logger alive for the caller's lifetime even
/// when @ref SetDefaultLogger is invoked concurrently.
LoggerPtr GetDefaultLoggerPtr() noexcept;

/// Replaces the default logger. The provided LoggerPtr is kept alive.
void SetDefaultLogger(LoggerPtr new_default_logger) noexcept;

/// Releases the calling thread's cached pointer to the default logger.
/// The next LOG_* on this thread reloads from the global slot.
///
/// Call this on long-lived threads after `SetDefaultLogger(nullptr)` or
/// a hot swap if you want the old logger's ref count to drop *now*
/// instead of "at the next LOG_* on this thread" — otherwise a worker
/// that is idle for a long time keeps the superseded logger (and its
/// sinks) alive in its TLS slot.
void PurgeTlsDefaultLoggerCache() noexcept;

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
    RateLimiter(RateLimitData& data, const char* file, int line) noexcept;
    bool ShouldLog() const noexcept { return should_log_; }
    std::uint64_t GetDroppedCount() const noexcept { return dropped_count_; }

private:
    bool should_log_{true};
    std::uint64_t dropped_count_{0};
};

}  // namespace impl

/// Payload for the `RateLimitDropHandler` — one event per `LOG_LIMITED_*`
/// invocation that gets suppressed. Handler runs on the producing thread;
/// keep it non-blocking (push to an in-process queue / counter at most).
struct RateLimitDropEvent {
    /// Source file of the `LOG_LIMITED_*` call site (trimmed via
    /// `ULOG_SOURCE_ROOT_LITERAL` if configured, same shape as `module`).
    const char* file{nullptr};
    /// Source line of the call site.
    int line{0};
    /// Drops accumulated at this site during the current 1-second window.
    std::uint64_t site_dropped{0};
    /// Process-wide running total across every `LOG_LIMITED_*` site, in
    /// sync with `GetRateLimitDroppedTotal()` post-increment.
    std::uint64_t total_dropped{0};
};

/// Per-drop callback signature. `nullptr` deregisters.
using RateLimitDropHandler = void (*)(const RateLimitDropEvent&) noexcept;

/// Installs a callback fired on every `LOG_LIMITED_*` suppression. Thread
/// safe. One handler per process; installing replaces the prior one.
void SetRateLimitDropHandler(RateLimitDropHandler handler) noexcept;

/// Total number of log records suppressed by LOG_LIMITED_* across the whole
/// process since start (or since ResetRateLimitStats). Read-only.
std::uint64_t GetRateLimitDroppedTotal() noexcept;

/// Resets the global drop counter. Intended for tests / diagnostics only.
void ResetRateLimitStats() noexcept;

namespace impl {

/// Noop helper returned from compile-erased macros.
struct Noop {
    template <typename T>
    const Noop& operator<<(const T&) const noexcept {
        return *this;
    }
};

/// Static per-source-line registration for dynamic debug controls.
///
/// Each instance self-registers into a process-wide intrusive singly-linked
/// list on construction (lock-free CAS into a global atomic head). The list
/// is walked by `ulog::ForEachLogEntry` for runtime enumeration / dynamic
/// management UIs. Because instances have static storage duration, they are
/// never unlinked — the list grows monotonically during program lifetime.
class StaticLogEntry final {
public:
    StaticLogEntry(const char* path, int line) noexcept;
    StaticLogEntry(const StaticLogEntry&) = delete;
    StaticLogEntry& operator=(const StaticLogEntry&) = delete;

    bool ShouldNotLog(LoggerRef logger, Level level) const noexcept;
    bool ShouldNotLog(const LoggerPtr& logger, Level level) const noexcept;

    const char* path() const noexcept { return path_; }
    int line() const noexcept { return line_; }
    const StaticLogEntry* next() const noexcept { return next_; }

private:
    const char* path_;
    int line_;
    StaticLogEntry* next_{nullptr};
};

/// Head of the global `StaticLogEntry` list. Returned pointer is the
/// most-recently-registered entry; walk via `next()` until `nullptr`.
/// Safe to call at any time; new registrations are published with release
/// so readers observing a non-null head see a fully-constructed entry.
const StaticLogEntry* GetStaticLogEntriesHead() noexcept;

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
    ::ulog::LogRecordLocation { ULOG_IMPL_TRIM_FILE(__FILE__), __LINE__, static_cast<const char*>(__func__) }

#define ULOG_IMPL_LOG_TO(logger, level, ...) \
    ::ulog::LogHelper((logger), (level), ULOG_IMPL_LOG_LOCATION())

// Dynamic-debug aware, ShouldLog-guarded record builder.
//
// Snapshots `logger` into a universal reference so the expression is
// evaluated exactly once, regardless of whether the caller passed a
// LoggerRef or a temporary LoggerPtr (e.g. from GetDefaultLoggerPtr()).
// This keeps a shared_ptr alive for the full duration of the helper.
// Expands to a for-statement that runs exactly once iff logging is enabled;
// chaining `<< value` attaches to the LogHelper in the loop body.
#define ULOG_LOG_TO(logger, lvl, ...)                                                         \
    if (auto&& ulog_logger__ = (logger);                                                      \
        ULOG_IMPL_DYNAMIC_DEBUG_ENTRY().ShouldNotLog(ulog_logger__, (lvl))) {}                \
    else                                                                                      \
        for (bool ulog_once__ = true; ulog_once__; ulog_once__ = false)                       \
            ::ulog::LogHelper(                                                                \
                std::forward<decltype(ulog_logger__)>(ulog_logger__),                         \
                (lvl),                                                                        \
                ULOG_IMPL_LOG_LOCATION())

// Default-logger macro snapshots the shared_ptr through the atomic slot, so
// the record outlives any concurrent SetDefaultLogger() race.
#define ULOG_LOG(lvl, ...) ULOG_LOG_TO(::ulog::GetDefaultLoggerPtr(), (lvl), __VA_ARGS__)

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
#define ULOG_LOG_TRACE(...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_DEBUG(...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_INFO(...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_WARNING(...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_ERROR(...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LOG_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
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
        }(),                                                                                                 \
        ULOG_IMPL_TRIM_FILE(__FILE__),                                                                       \
        __LINE__                                                                                             \
    }; !ulog_rl__.ShouldLog()) {                                                                             \
    } else                                                                                                   \
        ULOG_LOG_TO((logger), (lvl), __VA_ARGS__) << ulog_rl__

#define ULOG_LOG_LIMITED(lvl, ...)          ULOG_LOG_LIMITED_TO(::ulog::GetDefaultLoggerPtr(), (lvl), __VA_ARGS__)

#define ULOG_LOG_LIMITED_TRACE(...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_DEBUG(...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_INFO(...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_WARNING(...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_ERROR(...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LOG_LIMITED_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LOG_LIMITED_CRITICAL(...) ULOG_LOG_LIMITED(::ulog::Level::kCritical, __VA_ARGS__)

#define ULOG_LOG_LIMITED_TRACE_TO(logger, ...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_DEBUG_TO(logger, ...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_INFO_TO(logger, ...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_WARNING_TO(logger, ...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_ERROR_TO(logger, ...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LOG_LIMITED_TO, logger, __VA_ARGS__)
#define ULOG_LOG_LIMITED_CRITICAL_TO(logger, ...) ULOG_LOG_LIMITED_TO(logger, ::ulog::Level::kCritical, __VA_ARGS__)

// -------- fmt-style variants (LFMT_*) --------
//
// Forwarding wrapper: format the payload via `fmt::format` first and
// stream the resulting std::string into a regular LogHelper. Level
// filtering + erasure + dynamic-debug all compose the same as the
// streaming LOG_* family, because LogHelper's operator<< is only
// touched after the macro's `if (ShouldNotLog) else ...` guard chose
// the else branch — so `fmt::format(...)` is never evaluated for a
// filtered record.
#define ULOG_LFMT_TO(logger, lvl, ...) \
    ULOG_LOG_TO(logger, lvl) << ::fmt::format(__VA_ARGS__)
#define ULOG_LFMT(lvl, ...)            ULOG_LFMT_TO(::ulog::GetDefaultLoggerPtr(), lvl, __VA_ARGS__)

#define ULOG_LFMT_TRACE(...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LFMT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LFMT_DEBUG(...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LFMT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LFMT_INFO(...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LFMT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LFMT_WARNING(...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LFMT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LFMT_ERROR(...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LFMT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LFMT_CRITICAL(...) ULOG_LFMT(::ulog::Level::kCritical, __VA_ARGS__)

#define ULOG_LFMT_TRACE_TO(logger, ...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LFMT_TO, logger, __VA_ARGS__)
#define ULOG_LFMT_DEBUG_TO(logger, ...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LFMT_TO, logger, __VA_ARGS__)
#define ULOG_LFMT_INFO_TO(logger, ...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LFMT_TO, logger, __VA_ARGS__)
#define ULOG_LFMT_WARNING_TO(logger, ...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LFMT_TO, logger, __VA_ARGS__)
#define ULOG_LFMT_ERROR_TO(logger, ...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LFMT_TO, logger, __VA_ARGS__)
#define ULOG_LFMT_CRITICAL_TO(logger, ...) ULOG_LFMT_TO(logger, ::ulog::Level::kCritical, __VA_ARGS__)

// -------- printf-style variants (LPRINT_*) --------
//
// `fmt::sprintf` accepts classic printf conversion specifiers
// (`%d`, `%s`, `%5.2f`, …), not the braces-based fmt syntax. Pays a
// modest extra runtime vs. `fmt::format` — intended as a compat path
// for codebases migrating from `printf`/`std::printf`-based logs.
#define ULOG_LPRINT_TO(logger, lvl, ...) \
    ULOG_LOG_TO(logger, lvl) << ::fmt::sprintf(__VA_ARGS__)
#define ULOG_LPRINT(lvl, ...)            ULOG_LPRINT_TO(::ulog::GetDefaultLoggerPtr(), lvl, __VA_ARGS__)

#define ULOG_LPRINT_TRACE(...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LPRINT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LPRINT_DEBUG(...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LPRINT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LPRINT_INFO(...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LPRINT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LPRINT_WARNING(...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LPRINT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LPRINT_ERROR(...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LPRINT_TO, ::ulog::GetDefaultLoggerPtr(), __VA_ARGS__)
#define ULOG_LPRINT_CRITICAL(...) ULOG_LPRINT(::ulog::Level::kCritical, __VA_ARGS__)

#define ULOG_LPRINT_TRACE_TO(logger, ...)    ULOG_IMPL_LOGS_TRACE_ERASER(ULOG_LPRINT_TO, logger, __VA_ARGS__)
#define ULOG_LPRINT_DEBUG_TO(logger, ...)    ULOG_IMPL_LOGS_DEBUG_ERASER(ULOG_LPRINT_TO, logger, __VA_ARGS__)
#define ULOG_LPRINT_INFO_TO(logger, ...)     ULOG_IMPL_LOGS_INFO_ERASER(ULOG_LPRINT_TO, logger, __VA_ARGS__)
#define ULOG_LPRINT_WARNING_TO(logger, ...)  ULOG_IMPL_LOGS_WARNING_ERASER(ULOG_LPRINT_TO, logger, __VA_ARGS__)
#define ULOG_LPRINT_ERROR_TO(logger, ...)    ULOG_IMPL_LOGS_ERROR_ERASER(ULOG_LPRINT_TO, logger, __VA_ARGS__)
#define ULOG_LPRINT_CRITICAL_TO(logger, ...) ULOG_LPRINT_TO(logger, ::ulog::Level::kCritical, __VA_ARGS__)

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

// fmt-style short aliases. `LFMT_INFO("user={} id={}", name, id)`.
#define LFMT(lvl, ...)                     ULOG_LFMT(lvl, __VA_ARGS__)
#define LFMT_TO(logger, lvl, ...)          ULOG_LFMT_TO(logger, lvl, __VA_ARGS__)
#define LFMT_TRACE(...)                    ULOG_LFMT_TRACE(__VA_ARGS__)
#define LFMT_DEBUG(...)                    ULOG_LFMT_DEBUG(__VA_ARGS__)
#define LFMT_INFO(...)                     ULOG_LFMT_INFO(__VA_ARGS__)
#define LFMT_WARNING(...)                  ULOG_LFMT_WARNING(__VA_ARGS__)
#define LFMT_ERROR(...)                    ULOG_LFMT_ERROR(__VA_ARGS__)
#define LFMT_CRITICAL(...)                 ULOG_LFMT_CRITICAL(__VA_ARGS__)
#define LFMT_TRACE_TO(logger, ...)         ULOG_LFMT_TRACE_TO(logger, __VA_ARGS__)
#define LFMT_DEBUG_TO(logger, ...)         ULOG_LFMT_DEBUG_TO(logger, __VA_ARGS__)
#define LFMT_INFO_TO(logger, ...)          ULOG_LFMT_INFO_TO(logger, __VA_ARGS__)
#define LFMT_WARNING_TO(logger, ...)       ULOG_LFMT_WARNING_TO(logger, __VA_ARGS__)
#define LFMT_ERROR_TO(logger, ...)         ULOG_LFMT_ERROR_TO(logger, __VA_ARGS__)
#define LFMT_CRITICAL_TO(logger, ...)      ULOG_LFMT_CRITICAL_TO(logger, __VA_ARGS__)

// printf-style short aliases. `LPRINT_INFO("user=%s id=%d", name, id)`.
#define LPRINT(lvl, ...)                   ULOG_LPRINT(lvl, __VA_ARGS__)
#define LPRINT_TO(logger, lvl, ...)        ULOG_LPRINT_TO(logger, lvl, __VA_ARGS__)
#define LPRINT_TRACE(...)                  ULOG_LPRINT_TRACE(__VA_ARGS__)
#define LPRINT_DEBUG(...)                  ULOG_LPRINT_DEBUG(__VA_ARGS__)
#define LPRINT_INFO(...)                   ULOG_LPRINT_INFO(__VA_ARGS__)
#define LPRINT_WARNING(...)                ULOG_LPRINT_WARNING(__VA_ARGS__)
#define LPRINT_ERROR(...)                  ULOG_LPRINT_ERROR(__VA_ARGS__)
#define LPRINT_CRITICAL(...)               ULOG_LPRINT_CRITICAL(__VA_ARGS__)
#define LPRINT_TRACE_TO(logger, ...)       ULOG_LPRINT_TRACE_TO(logger, __VA_ARGS__)
#define LPRINT_DEBUG_TO(logger, ...)       ULOG_LPRINT_DEBUG_TO(logger, __VA_ARGS__)
#define LPRINT_INFO_TO(logger, ...)        ULOG_LPRINT_INFO_TO(logger, __VA_ARGS__)
#define LPRINT_WARNING_TO(logger, ...)     ULOG_LPRINT_WARNING_TO(logger, __VA_ARGS__)
#define LPRINT_ERROR_TO(logger, ...)       ULOG_LPRINT_ERROR_TO(logger, __VA_ARGS__)
#define LPRINT_CRITICAL_TO(logger, ...)    ULOG_LPRINT_CRITICAL_TO(logger, __VA_ARGS__)

#endif  // !ULOG_NO_SHORT_MACROS

// NOLINTEND(cppcoreguidelines-macro-usage)
