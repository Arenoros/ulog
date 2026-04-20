#pragma once

/// @file ulog/log_record_location.hpp
/// @brief Source-location capture for log records — file / function /
///        line from compiler builtins, with the line number pre-rendered
///        into an inline decimal buffer.
///
/// Extracted from `ulog/log_helper.hpp` so consumers (structured sinks,
/// async queue entries, etc.) can embed a `LogRecordLocation` by value
/// without pulling in the full streaming helper.

#include <cstdint>
#include <string_view>

#include <ulog/detail/source_root.hpp>

namespace ulog {

/// Source location captured by LOG_* macros.
///
/// Stores the call-site file / function / line from `__builtin_FILE` /
/// `__builtin_FUNCTION` / `__builtin_LINE`. The line number is
/// pre-rendered into an inline decimal buffer at construction so the
/// formatter can splice it into the `module` field without paying for a
/// `fmt::format("{}", line)` call per record — the saving scales
/// linearly with logging rate.
///
/// Capture point: use `LogRecordLocation::Current()` — a `static
/// constexpr` factory that relies on default-argument evaluation to
/// snapshot the caller's source position. `LogHelper`'s ctor uses it as
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
// ulog/log_record_location.hpp cannot clobber user definitions of the
// same name.
#undef ULOG_IMPL_BUILTIN_FILE

}  // namespace ulog
