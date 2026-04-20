#pragma once

/// @file ulog/log_helper_range.hpp
/// @brief `operator<<(LogHelper&, Range)` for containers — userver parity.
///
/// Renders sequences as `[a, b, c]`, maps as `{"k1": v1, "k2": v2}`.
/// String-like values are wrapped in `Quoted{}`. Respects
/// `LogHelper::IsLimitReached()`: once the 10 KB cap is crossed the
/// remaining elements are summarised as `, ...N more`.
///
/// Included automatically via `ulog/log.hpp`. Pulling
/// `log_helper.hpp` directly does NOT bring in these overloads.
///
/// Deliberately blocks `char`-element ranges (`std::vector<char>`,
/// `char[N]` array that is NOT a string literal, etc.) via
/// `static_assert` — the userver convention is to force callers to
/// convert into a `std::string_view` explicitly rather than accidentally
/// get `[h, e, l, l, o]`.

#include <cstddef>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <utility>

#include <ulog/log_helper.hpp>
#include <ulog/log_helper_extras.hpp>

namespace ulog {

namespace detail {

/// True iff `const T&` has `std::begin` / `std::end`.
template <typename T, typename = void>
struct HasBeginEndImpl : std::false_type {};

template <typename T>
struct HasBeginEndImpl<T, std::void_t<
    decltype(std::begin(std::declval<const T&>())),
    decltype(std::end(std::declval<const T&>()))>> : std::true_type {};

/// True iff `T` is a range but NOT string-like (i.e. not convertible to
/// `std::string_view`). The built-in `Put(const char*)` / `Put(string)`
/// paths already handle string-like values — we exclude them here so
/// the range overload doesn't hijack them. Arrays are routed through
/// the dedicated `operator<<(LogHelper&, const T(&)[N])` overload below
/// (reference-to-array partial-orders cleaner against the pointer
/// overload from `log_helper_extras.hpp`).
template <typename T>
constexpr bool kIsLoggableRange =
    HasBeginEndImpl<T>::value &&
    !std::is_convertible_v<const T&, std::string_view> &&
    !std::is_array_v<T>;

/// Decayed element type of a range.
template <typename T>
using RangeValueType =
    std::decay_t<decltype(*std::begin(std::declval<const T&>()))>;

/// True iff the element type is a narrow-character type. userver-style
/// trap: ranges of `char` are ambiguous between "list of 1-byte ints"
/// and "text" — force callers to pick explicitly.
template <typename T>
constexpr bool kIsCharLike =
    std::is_same_v<std::remove_cv_t<T>, char> ||
    std::is_same_v<std::remove_cv_t<T>, signed char> ||
    std::is_same_v<std::remove_cv_t<T>, unsigned char> ||
    std::is_same_v<std::remove_cv_t<T>, wchar_t> ||
    std::is_same_v<std::remove_cv_t<T>, char16_t> ||
    std::is_same_v<std::remove_cv_t<T>, char32_t>;

/// True iff `T` is a map-like type (has `key_type` AND `mapped_type`).
template <typename T, typename = void>
struct IsMapImpl : std::false_type {};

template <typename T>
struct IsMapImpl<T, std::void_t<typename T::key_type,
                                typename T::mapped_type>>
    : std::true_type {};

template <typename T>
constexpr bool kIsMap = IsMapImpl<T>::value;

/// Renders a single range element. Non-map elements recurse through
/// `lh << element`; string-like elements are wrapped in `Quoted{}` for
/// disambiguation in the output.
template <typename T>
inline void PutRangeElement(LogHelper& lh, const T& elem) {
    if constexpr (std::is_convertible_v<const T&, std::string_view>) {
        lh << Quoted{std::string_view(elem)};
    } else {
        lh << elem;
    }
}

/// Map-element specialisation for `std::pair<const K, V>`. Key goes
/// first (quoted when string-like), then `": "`, then the value.
template <typename K, typename V>
inline void PutRangeElement(LogHelper& lh,
                            const std::pair<const K, V>& kv) {
    if constexpr (std::is_convertible_v<const K&, std::string_view>) {
        lh << Quoted{std::string_view(kv.first)};
    } else {
        lh << kv.first;
    }
    lh << std::string_view(": ");
    PutRangeElement(lh, kv.second);
}

/// Walks `range` and emits `[a, b, c]` (sequence) or
/// `{"k1": v1, "k2": v2}` (map). Breaks out of the loop once the cap
/// has been reached and best-effort appends `, ...` + closing bracket
/// — both may no-op if the cap already saturated, in which case the
/// emitted record carries a `truncated=true` tag from DoLog so the
/// consumer can detect the cutoff.
template <typename T>
inline void PutRange(LogHelper& lh, const T& range) {
    using V = RangeValueType<T>;
    static_assert(!kIsCharLike<V>,
                  "streaming a char-element range via LogHelper is "
                  "ambiguous between \"list of ints\" and \"text\"; "
                  "wrap into std::string_view explicitly if you want "
                  "textual output");

    constexpr auto open  = kIsMap<T> ? std::string_view("{")
                                     : std::string_view("[");
    constexpr auto close = kIsMap<T> ? std::string_view("}")
                                     : std::string_view("]");

    lh << open;
    bool first = true;
    bool truncated_mid = false;
    for (const auto& elem : range) {
        if (lh.IsLimitReached()) {
            truncated_mid = true;
            break;
        }
        if (!first) lh << std::string_view(", ");
        first = false;
        PutRangeElement(lh, elem);
    }
    if (truncated_mid) {
        // Best-effort — may no-op if the cap already saturated. The
        // `truncated=true` tag emitted by DoLog stays authoritative.
        lh << std::string_view(", ...");
    }
    lh << close;
}

}  // namespace detail

/// Generic range streaming — sequences and maps. See file docstring.
template <typename T,
          std::enable_if_t<detail::kIsLoggableRange<T>, int> = 0>
inline LogHelper& operator<<(LogHelper& lh, const T& range) noexcept {
    try {
        detail::PutRange(lh, range);
    } catch (...) {
        // Streaming is noexcept; drop silently on alloc failure.
    }
    return lh;
}

template <typename T,
          std::enable_if_t<detail::kIsLoggableRange<T>, int> = 0>
inline LogHelper&& operator<<(LogHelper&& lh, const T& range) noexcept {
    lh << range;
    return std::move(lh);
}

/// Array overload. Needed because `int arr[3]` also matches the pointer
/// overload from `log_helper_extras.hpp` via array-to-pointer decay; a
/// reference-to-array signature wins over `const T*` on partial ordering
/// so the user gets a rendered list rather than a hex address. Char
/// arrays are excluded via SFINAE — string literals keep going through
/// the built-in `Put(const char*)` path.
template <typename T, std::size_t N,
          std::enable_if_t<!detail::kIsCharLike<T>, int> = 0>
inline LogHelper& operator<<(LogHelper& lh, const T (&arr)[N]) noexcept {
    try {
        detail::PutRange(lh, arr);
    } catch (...) {
    }
    return lh;
}

template <typename T, std::size_t N,
          std::enable_if_t<!detail::kIsCharLike<T>, int> = 0>
inline LogHelper&& operator<<(LogHelper&& lh, const T (&arr)[N]) noexcept {
    lh << arr;
    return std::move(lh);
}

}  // namespace ulog
