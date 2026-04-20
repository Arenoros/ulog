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

#include <ulog/detail/source_root.hpp>
#include <ulog/fwd.hpp>
#include <ulog/level.hpp>
#include <ulog/log_extra.hpp>

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

/// Source location captured by LOG_* macros.
///
/// Stores the call-site file / function / line from `__builtin_FILE` /
/// `__builtin_FUNCTION` / `__builtin_LINE`. The line number is pre-rendered
/// into an inline decimal buffer at construction so the formatter can
/// splice it into the `module` field without paying for a
/// `fmt::format("{}", line)` call per record — the saving scales linearly
/// with logging rate.
///
/// Capture point: use `LogRecordLocation::Current()` — a `static
/// constexpr` factory that relies on default-argument evaluation to
/// snapshot the caller's source position. LogHelper's ctor uses it as
/// a default, so `LogHelper(logger, level)` writes the caller's
/// location without the macro spelling out `__FILE__` / `__LINE__`.
///
/// The three-argument `LogRecordLocation(file, line, function)`
/// constructor is retained for backwards compatibility (tests,
/// synthetic records) and precomputes the line string the same way.
class LogRecordLocation {
    struct EmplaceEnabler {};
public:
    /// Backward-compatible direct construction. Preferable to use
    /// `Current()` in new code.
    constexpr LogRecordLocation(const char* file, int line, const char* function) noexcept
        : file_(file ? file : ""),
          function_(function ? function : ""),
          line_(static_cast<std::uint_least32_t>(line < 0 ? 0 : line)) {
        RenderLineDigits();
    }

    /// Default-constructed location — empty / zero. Used by helper
    /// paths that don't have a meaningful source position.
    constexpr LogRecordLocation() noexcept = default;

#if defined(ULOG_SOURCE_ROOT_LITERAL)
#define ULOG_IMPL_BUILTIN_FILE() \
    ::ulog::detail::TrimSourceRoot(__builtin_FILE(), ULOG_SOURCE_ROOT_LITERAL)
#else
#define ULOG_IMPL_BUILTIN_FILE() __builtin_FILE()
#endif

    /// Captures the caller's source position via compiler builtins.
    /// The `EmplaceEnabler` leading tag prevents callers from
    /// accidentally constructing a `LogRecordLocation` with three
    /// positional arguments through the `Current(...)` overload — they
    /// must use the direct constructor for that.
    static constexpr LogRecordLocation Current(
        EmplaceEnabler = {},
        const char* file = ULOG_IMPL_BUILTIN_FILE(),
        const char* function = __builtin_FUNCTION(),
        std::uint_least32_t line = __builtin_LINE()) noexcept {
        return LogRecordLocation(file, function, line);
    }

    constexpr const char* file_name() const noexcept { return file_; }
    constexpr const char* function_name() const noexcept { return function_; }
    constexpr std::uint_least32_t line() const noexcept { return line_; }

    /// Decimal string representation of `line()`. Precomputed once at
    /// construction; the view points into the internal buffer.
    constexpr std::string_view line_string() const noexcept {
        return std::string_view(line_string_, line_digits_);
    }

    /// Convenience — true iff a real source position was captured
    /// (non-empty file OR function). Used by formatters to decide
    /// whether to emit the `module` field.
    constexpr bool has_value() const noexcept {
        return (file_ && *file_) || (function_ && *function_);
    }

private:
    constexpr LogRecordLocation(const char* file, const char* function,
                                std::uint_least32_t line) noexcept
        : file_(file ? file : ""),
          function_(function ? function : ""),
          line_(line) {
        RenderLineDigits();
    }

    constexpr void RenderLineDigits() noexcept {
        auto value = line_;
        if (value == 0) {
            line_string_[0] = '0';
            line_digits_ = 1;
            return;
        }
        char temp[10] = {};
        std::uint_least32_t n = 0;
        while (value != 0 && n < sizeof(temp)) {
            temp[n++] = static_cast<char>('0' + (value % 10));
            value /= 10;
        }
        const std::uint_least32_t out = n <= sizeof(line_string_) ? n : sizeof(line_string_);
        for (std::uint_least32_t i = 0; i < out; ++i) {
            line_string_[i] = temp[out - 1 - i];
        }
        line_digits_ = out;
    }

    const char* file_{""};
    const char* function_{""};
    std::uint_least32_t line_{0};
    std::uint_least32_t line_digits_{0};
    char line_string_[8]{};
};

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
    template <typename T>
    LogHelper& operator<<(const T& value) & {
        Put(value);
        return *this;
    }
    template <typename T>
    LogHelper&& operator<<(const T& value) && {
        Put(value);
        return std::move(*this);
    }

    /// Stream LogExtra: merges its fields into the record.
    LogHelper& operator<<(const LogExtra& extra) &;
    LogHelper&& operator<<(const LogExtra& extra) &&;

    /// Stream helpers.
    LogHelper& operator<<(Hex v) &;
    LogHelper&& operator<<(Hex v) && { return std::move(*this << v); }
    LogHelper& operator<<(HexShort v) &;
    LogHelper&& operator<<(HexShort v) && { return std::move(*this << v); }
    LogHelper& operator<<(Quoted v) &;
    LogHelper&& operator<<(Quoted v) && { return std::move(*this << v); }

    /// Stream for the rate limiter used by LOG_LIMITED_*.
    LogHelper& operator<<(const impl::RateLimiter& rl) &;
    LogHelper&& operator<<(const impl::RateLimiter& rl) && { return std::move(*this << rl); }

    /// Captures an exception (message + type) into the record.
    LogHelper& WithException(const std::exception& ex) &;
    LogHelper&& WithException(const std::exception& ex) && { return std::move(WithException(ex)); }

    /// Access low-level tag writer.
    impl::TagWriter& GetTagWriter() noexcept;

    /// True if the helper will emit when destroyed.
    bool IsActive() const noexcept;

private:
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

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ulog
