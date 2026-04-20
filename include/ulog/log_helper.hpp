#pragma once

/// @file ulog/log_helper.hpp
/// @brief Stream-like builder for log records. Produced by LOG_* macros.

#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include <ulog/detail/range_traits.hpp>
#include <ulog/fwd.hpp>
#include <ulog/json_string.hpp>
#include <ulog/level.hpp>
#include <ulog/log_extra.hpp>
#include <ulog/log_record_location.hpp>

namespace ulog {

namespace impl {
class RateLimiter;
class TagWriter;
namespace formatters {
class Base;
struct BaseDeleter;
using BasePtr = std::unique_ptr<Base, BaseDeleter>;
}  // namespace formatters
}  // namespace impl

/// Hex-formatted value helper.
struct Hex {
    std::uintptr_t value;
};

/// Short-hex-formatted value helper (no leading zeros).
struct HexShort {
    std::uintptr_t value;
};

/// Quoted-string helper. Value will be output wrapped in double quotes.
struct Quoted {
    std::string_view value;
};

/// Stream-like builder. Assembled by LOG_* macros, flushed in destructor.
class LogHelper final {
public:
    /// RAII tag: when set, destructor does not emit (useful for `if(ShouldLog)`).
    struct NoLog {};

    /// `location` defaults to `LogRecordLocation::Current()` so the
    /// macro expansion does not have to spell out `__FILE__` /
    /// `__LINE__` / `__func__` — the compiler evaluates the default
    /// argument at the expansion point and the caller's position is
    /// captured through `__builtin_FILE` / `__builtin_LINE` /
    /// `__builtin_FUNCTION`.
    LogHelper(LoggerRef logger, Level level,
              const LogRecordLocation& location = LogRecordLocation::Current()) noexcept;
    LogHelper(LoggerRef logger, Level level, NoLog,
              const LogRecordLocation& location = LogRecordLocation::Current()) noexcept;

    /// Overload that keeps the logger alive for the helper's lifetime.
    /// Used by the default-logger macro expansion so the record survives a
    /// concurrent SetDefaultLogger() swap.
    LogHelper(LoggerPtr logger, Level level,
              const LogRecordLocation& location = LogRecordLocation::Current()) noexcept;

    ~LogHelper();

    LogHelper(const LogHelper&) = delete;
    LogHelper(LogHelper&&) = delete;
    LogHelper& operator=(const LogHelper&) = delete;
    LogHelper& operator=(LogHelper&&) = delete;

    /// Stream operator for arbitrary types. Primary entry point for LOG_* macros.
    ///
    /// All `operator<<` overloads are `noexcept` — any exception thrown
    /// by `fmt::format` (OOM on the format buffer, bad format spec, …)
    /// or by user `<<` conversion is swallowed, logged to `stderr` via
    /// `InternalLoggingError`, and the record is marked broken so the
    /// destructor skips emission. This lets callers use `LOG_*` from
    /// `noexcept` contexts (dtors, signal-safe paths, async
    /// work-guards) without risking `std::terminate`.
    ///
    /// Pointer types are deliberately excluded via SFINAE so the free
    /// function `operator<<(LogHelper&, const T*)` in
    /// `ulog/log_helper_extras.hpp` can kick in with the null-guard /
    /// hex-address rendering. `const char*` is routed back to the
    /// built-in `Put(const char*)` path through the concrete overload
    /// below so string literals and C-strings keep rendering as text.
    template <typename T, std::enable_if_t<
        !std::is_pointer_v<T> &&
        !std::is_array_v<T> &&
        !std::is_invocable_r_v<void, T&, LogHelper&> &&
        !detail::kIsLoggableRange<T>, int> = 0>
    LogHelper& operator<<(const T& value) & noexcept {
        try { Put(value); } catch (...) { InternalLoggingError("operator<< threw"); }
        return *this;
    }
    template <typename T, std::enable_if_t<
        !std::is_pointer_v<T> &&
        !std::is_array_v<T> &&
        !std::is_invocable_r_v<void, T&, LogHelper&> &&
        !detail::kIsLoggableRange<T>, int> = 0>
    LogHelper&& operator<<(const T& value) && noexcept {
        try { Put(value); } catch (...) { InternalLoggingError("operator<< threw"); }
        return std::move(*this);
    }

    /// Concrete `const char*` overload — keeps NUL-terminated strings
    /// (string literals, C-strings) on the text path. Lives on the
    /// class so the non-template match beats the free-function pointer
    /// template in `log_helper_extras.hpp` without relying on partial
    /// ordering.
    LogHelper& operator<<(const char* s) & noexcept {
        try { Put(s); } catch (...) { InternalLoggingError("operator<< threw"); }
        return *this;
    }
    LogHelper&& operator<<(const char* s) && noexcept {
        *this << s;
        return std::move(*this);
    }

    /// Stream LogExtra: merges its fields into the record.
    LogHelper& operator<<(const LogExtra& extra) & noexcept;
    LogHelper&& operator<<(const LogExtra& extra) && noexcept;

    /// Stream helpers.
    LogHelper& operator<<(Hex v) & noexcept;
    LogHelper&& operator<<(Hex v) && noexcept { return std::move(*this << v); }
    LogHelper& operator<<(HexShort v) & noexcept;
    LogHelper&& operator<<(HexShort v) && noexcept { return std::move(*this << v); }
    LogHelper& operator<<(Quoted v) & noexcept;
    LogHelper&& operator<<(Quoted v) && noexcept { return std::move(*this << v); }

    /// Stream for the rate limiter used by LOG_LIMITED_*.
    LogHelper& operator<<(const impl::RateLimiter& rl) & noexcept;
    LogHelper&& operator<<(const impl::RateLimiter& rl) && noexcept { return std::move(*this << rl); }

    /// fmt-style helper — forwards to `fmt::format` and appends the
    /// rendered text directly. Equivalent to
    /// `lh << fmt::format(fmt_str, args...)` but saves the temporary
    /// `std::string` copy and keeps `noexcept` guarantee.
    template <typename... Args>
    LogHelper& Format(fmt::format_string<Args...> fmt_str, Args&&... args) & noexcept {
        try {
            PutFormatted(fmt::format(fmt_str, std::forward<Args>(args)...));
        } catch (...) {
            InternalLoggingError("Format() threw");
        }
        return *this;
    }
    template <typename... Args>
    LogHelper&& Format(fmt::format_string<Args...> fmt_str, Args&&... args) && noexcept {
        this->Format(fmt_str, std::forward<Args>(args)...);
        return std::move(*this);
    }

    /// Captures an exception (message + type) into the record.
    LogHelper& WithException(const std::exception& ex) & noexcept;
    LogHelper&& WithException(const std::exception& ex) && noexcept { return std::move(WithException(ex)); }

    /// Attach a plain string-valued tag to the record. No-op on a
    /// broken or inactive helper. Extension `operator<<` overloads
    /// that need to add structured fields should call this (or
    /// `PutJsonTag`) directly rather than reaching for
    /// `GetTagWriter()` — the tag writer is an implementation
    /// detail that may change shape.
    void PutTag(std::string_view key, std::string_view value) noexcept;

    /// Attach a tag whose value is pre-serialised JSON. Identical
    /// gating to `PutTag`.
    void PutJsonTag(std::string_view key, const JsonString& value) noexcept;

    /// Access low-level tag writer. Deprecated — prefer `PutTag` /
    /// `PutJsonTag`. Retained for backwards compatibility with
    /// extension code compiled against earlier ulog releases.
    /// Returns a per-thread dummy writer when the helper failed to
    /// construct (e.g. pool `Pop` threw); the caller's tag writes
    /// become no-ops rather than null-dereferences.
    [[deprecated("Prefer LogHelper::PutTag / PutJsonTag — GetTagWriter "
                 "exposes the internal writer and will be made private "
                 "in a future release")]]
    impl::TagWriter& GetTagWriter() noexcept;

    /// True if the helper will emit when destroyed.
    bool IsActive() const noexcept;

    /// True once the accumulated text exceeds the 10 KB per-record
    /// budget. Further streaming writes are no-ops, and the emitted
    /// record gains a `truncated=true` tag. Callers doing expensive
    /// formatting (stack traces, serialised blobs) can short-circuit
    /// via this predicate rather than feeding bytes into the void.
    bool IsLimitReached() const noexcept;

private:
    /// Streaming methods below are private implementation detail; they
    /// may throw `std::bad_alloc` / format exceptions. Public `<<`
    /// overloads wrap them in try/catch so the user-facing contract
    /// stays `noexcept`.
    void Put(std::string_view sv);
    void Put(const char* s);
    void Put(const std::string& s) { Put(std::string_view(s)); }

    template <typename T>
    std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>> Put(T v) {
        PutFormatted(fmt::format("{}", v));
    }
    template <typename T>
    std::enable_if_t<std::is_enum_v<T>> Put(T v) {
        PutFormatted(fmt::format("{}", static_cast<std::underlying_type_t<T>>(v)));
    }
    void Put(bool v);
    void Put(char v);
    void PutFormatted(std::string s);

    /// Handle an internal failure (exception in a streaming method).
    /// Writes a diagnostic line to `stderr` and marks the record
    /// broken so the destructor skips `DoLog`. Never throws. `msg`
    /// should be a static string literal — not formatted at call site
    /// — so this path itself cannot fail.
    void InternalLoggingError(const char* msg) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ulog
