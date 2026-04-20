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
    ///
    /// `file` / `function` are converted to `std::string_view` up front
    /// so the `char_traits::length` (i.e. `strlen`) call happens exactly
    /// once — on string literals the compiler folds it at compile time.
    /// Downstream formatters consume views directly, avoiding a runtime
    /// strlen on every record.
    constexpr LogRecordLocation(const char* file, int line, const char* function) noexcept
        : file_(file ? std::string_view(file) : std::string_view{}),
          function_(function ? std::string_view(function) : std::string_view{}),
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

    constexpr std::string_view file_name() const noexcept { return file_; }
    constexpr std::string_view function_name() const noexcept { return function_; }
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
        return !file_.empty() || !function_.empty();
    }

private:
    constexpr LogRecordLocation(const char* file, const char* function,
                                std::uint_least32_t line) noexcept
        : file_(file ? std::string_view(file) : std::string_view{}),
          function_(function ? std::string_view(function) : std::string_view{}),
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

    std::string_view file_;
    std::string_view function_;
    std::uint_least32_t line_{0};
    std::uint_least32_t line_digits_{0};
    char line_string_[8]{};
};

// Scope-less macros leak across the TU — undo here so headers including
// ulog/log_helper.hpp cannot clobber user definitions of the same name.
#undef ULOG_IMPL_BUILTIN_FILE

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
    template <typename T, std::enable_if_t<!std::is_pointer_v<T> &&
                                            !std::is_array_v<T>, int> = 0>
    LogHelper& operator<<(const T& value) & noexcept {
        try { Put(value); } catch (...) { InternalLoggingError("operator<< threw"); }
        return *this;
    }
    template <typename T, std::enable_if_t<!std::is_pointer_v<T> &&
                                            !std::is_array_v<T>, int> = 0>
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

    /// Captures an exception (message + type) into the record.
    LogHelper& WithException(const std::exception& ex) & noexcept;
    LogHelper&& WithException(const std::exception& ex) && noexcept { return std::move(WithException(ex)); }

    /// Access low-level tag writer. Returns a per-thread dummy writer
    /// when the helper failed to construct (e.g. pool Pop threw); the
    /// caller's tag writes become no-ops rather than null-dereferences.
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
