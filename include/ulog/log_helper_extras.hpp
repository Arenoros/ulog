#pragma once

/// @file ulog/log_helper_extras.hpp
/// @brief Free-function `operator<<` overloads for common std types —
///        userver parity: chrono, exception, optional, error_code, pointers.
///
/// Included automatically via `ulog/log.hpp`. Users who pull
/// `log_helper.hpp` directly must include this header themselves if they
/// rely on any of the overloads below.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <optional>
#include <ratio>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <typeinfo>
#include <utility>

#include <ulog/detail/timestamp.hpp>
#include <ulog/impl/tag_writer.hpp>
#include <ulog/log_helper.hpp>

namespace ulog {

namespace detail {

/// Returns the short suffix for a `std::chrono::duration<Rep, Period>`.
/// Empty view → no well-known mapping for this Period; caller falls back
/// to an explicit `N/D s` representation.
template <typename Period>
constexpr std::string_view DurationSuffix() noexcept {
    if constexpr (std::is_same_v<Period, std::nano>)          return "ns";
    else if constexpr (std::is_same_v<Period, std::micro>)    return "us";
    else if constexpr (std::is_same_v<Period, std::milli>)    return "ms";
    else if constexpr (std::is_same_v<Period, std::ratio<1>>) return "s";
    else if constexpr (std::is_same_v<Period, std::ratio<60>>)    return "min";
    else if constexpr (std::is_same_v<Period, std::ratio<3600>>)  return "h";
    else return std::string_view{};
}

}  // namespace detail

/// `std::chrono::duration<Rep, Period>` — number + suffix.
/// Known periods: ns, us, ms, s, min, h. Unknown periods spell out the
/// ratio (`10 * 7/10s` for `duration<int, ratio<7,10>>{10}`).
template <typename Rep, typename Period>
inline LogHelper& operator<<(LogHelper& lh,
                             const std::chrono::duration<Rep, Period>& d) noexcept {
    lh << d.count();
    constexpr auto suffix = detail::DurationSuffix<Period>();
    if constexpr (!suffix.empty()) {
        lh << suffix;
    } else {
        lh << std::string_view(" * ")
           << static_cast<std::int64_t>(Period::num)
           << std::string_view("/")
           << static_cast<std::int64_t>(Period::den)
           << std::string_view("s");
    }
    return lh;
}

template <typename Rep, typename Period>
inline LogHelper&& operator<<(LogHelper&& lh,
                              const std::chrono::duration<Rep, Period>& d) noexcept {
    lh << d;
    return std::move(lh);
}

/// `std::chrono::system_clock::time_point` — ISO8601 microseconds UTC
/// (same shape as the built-in `timestamp=` field).
inline LogHelper& operator<<(LogHelper& lh,
                             std::chrono::system_clock::time_point tp) noexcept {
    try {
        lh << detail::FormatTimestamp(tp, TimestampFormat::kIso8601Micro);
    } catch (...) {
        // Streaming is noexcept — swallow any allocation error.
    }
    return lh;
}
inline LogHelper&& operator<<(LogHelper&& lh,
                              std::chrono::system_clock::time_point tp) noexcept {
    lh << tp;
    return std::move(lh);
}

/// `std::exception` (and derived). Emits `what()` as text and attaches an
/// `exception_type` tag with the runtime type name (`typeid(ex).name()`).
/// Prefer this over the named `LogHelper::WithException` when streaming
/// fits the call-site idiom: `LOG_ERROR() << "parse failed: " << ex`.
inline LogHelper& operator<<(LogHelper& lh, const std::exception& ex) noexcept {
    try {
        lh << std::string_view(ex.what() ? ex.what() : "");
        lh.GetTagWriter().PutTag("exception_type", typeid(ex).name());
    } catch (...) {
        // noexcept-safe — drop silently on alloc failure.
    }
    return lh;
}
inline LogHelper&& operator<<(LogHelper&& lh, const std::exception& ex) noexcept {
    lh << ex;
    return std::move(lh);
}

/// `std::optional<T>` — recurses into the value when populated, emits the
/// literal `(none)` when empty. Requires `LogHelper::operator<<(const T&)`
/// to be available.
template <typename T>
inline LogHelper& operator<<(LogHelper& lh, const std::optional<T>& opt) noexcept {
    if (opt.has_value()) {
        lh << *opt;
    } else {
        lh << std::string_view("(none)");
    }
    return lh;
}
template <typename T>
inline LogHelper&& operator<<(LogHelper&& lh, const std::optional<T>& opt) noexcept {
    lh << opt;
    return std::move(lh);
}

/// `std::error_code` — `category:value (message)`.
inline LogHelper& operator<<(LogHelper& lh, const std::error_code& ec) noexcept {
    try {
        lh << std::string_view(ec.category().name())
           << ':'
           << ec.value()
           << std::string_view(" (")
           << ec.message()
           << ')';
    } catch (...) {
        // Swallow — ec.message() can allocate and throw on bad categories.
    }
    return lh;
}
inline LogHelper&& operator<<(LogHelper&& lh, const std::error_code& ec) noexcept {
    lh << ec;
    return std::move(lh);
}

namespace detail {

/// Shared pointer-streaming implementation. `void*` is treated as a
/// plain address; non-void pointers pre-reinterpret to `void*` so the
/// hex rendering is uniform. `nullptr` prints `(null)`.
inline void StreamPointer(LogHelper& lh, const void* p) noexcept {
    if (p == nullptr) {
        lh << std::string_view("(null)");
    } else {
        lh << Hex{reinterpret_cast<std::uintptr_t>(p)};
    }
}

}  // namespace detail

/// Generic pointer streaming — nullptr-guard + hex address via
/// `ulog::Hex`. `const char*` stays on the text path through the
/// concrete `LogHelper::operator<<(const char*)`; `void*` / `T*`
/// types convert implicitly to `const void*` here.
///
/// Signature is deliberately non-template so partial ordering with
/// the array overload in `log_helper_range.hpp` stays unambiguous —
/// `const T(&)[N]` (array template) wins cleanly over `const void*`
/// (non-template with implicit decay+qualification) so arrays render
/// as `[a, b, c]` instead of a hex address.
///
/// Function pointers do not implicitly convert to `const void*` under
/// the standard — they need an explicit `reinterpret_cast<void*>(fn)`
/// to be streamed. That is intentional: streaming code addresses is
/// almost always a bug or a `std::endl`-style manipulator mistake.
inline LogHelper& operator<<(LogHelper& lh, const void* p) noexcept {
    detail::StreamPointer(lh, p);
    return lh;
}
inline LogHelper&& operator<<(LogHelper&& lh, const void* p) noexcept {
    lh << p;
    return std::move(lh);
}

/// `std::atomic<T>` — streams the `.load()` value via the existing
/// overload chain for `T`. Acquire-order, single load per emission.
template <typename T>
inline LogHelper& operator<<(LogHelper& lh, const std::atomic<T>& a) noexcept {
    lh << a.load(std::memory_order_acquire);
    return lh;
}
template <typename T>
inline LogHelper&& operator<<(LogHelper&& lh, const std::atomic<T>& a) noexcept {
    lh << a;
    return std::move(lh);
}

/// Callable streaming — invokes `fun(lh)` for deferred / conditional
/// formatting. The member template `operator<<(const T&)` excludes
/// invocable types via SFINAE (`!is_invocable_r_v<void, T&, LogHelper&>`)
/// so this overload wins uniquely for lambdas / functors.
///
/// Typical use:
/// ```cpp
/// LOG_INFO() << "result=" << [&](ulog::LogHelper& h) {
///     if (h.IsLimitReached()) return;
///     // expensive dump only when budget is available
///     DumpDetailed(h);
/// };
/// ```
template <typename Fun,
          std::enable_if_t<std::is_invocable_r_v<void, Fun&, LogHelper&>,
                           int> = 0>
inline LogHelper& operator<<(LogHelper& lh, Fun&& fun) noexcept {
    try {
        std::forward<Fun>(fun)(lh);
    } catch (...) {
        // Swallow — streaming is noexcept.
    }
    return lh;
}
template <typename Fun,
          std::enable_if_t<std::is_invocable_r_v<void, Fun&, LogHelper&>,
                           int> = 0>
inline LogHelper&& operator<<(LogHelper&& lh, Fun&& fun) noexcept {
    std::forward<Fun>(fun)(lh);
    return std::move(lh);
}

}  // namespace ulog
