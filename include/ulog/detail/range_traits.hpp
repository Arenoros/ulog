#pragma once

/// @file ulog/detail/range_traits.hpp
/// @brief SFINAE traits shared by `LogHelper::operator<<` (in
///        `ulog/log_helper.hpp`) and the free range overloads (in
///        `ulog/log_helper_range.hpp`).
///
/// Split into its own header so the member template `operator<<` can
/// `enable_if`-exclude logged-range types without pulling in the full
/// `log_helper_range.hpp` (which in turn depends on `log_helper.hpp`).
/// Keeps the exclusion symmetric so GCC does not see two identical-
/// signature candidates — one member, one free — for rvalue LogHelper
/// streaming.

#include <iterator>
#include <string_view>
#include <type_traits>

namespace ulog::detail {

/// True iff `const T&` has `std::begin` / `std::end`.
template <typename T, typename = void>
struct HasBeginEndImpl : std::false_type {};

template <typename T>
struct HasBeginEndImpl<T, std::void_t<
    decltype(std::begin(std::declval<const T&>())),
    decltype(std::end(std::declval<const T&>()))>> : std::true_type {};

/// True iff `T` should be handled by the range `operator<<` overload.
/// Excludes string-like types (handled by `Put(const char*)` / string
/// view path) and arrays (which have a dedicated
/// `operator<<(LogHelper&, const T(&)[N])` overload).
template <typename T>
constexpr bool kIsLoggableRange =
    HasBeginEndImpl<T>::value &&
    !std::is_convertible_v<const T&, std::string_view> &&
    !std::is_array_v<T>;

}  // namespace ulog::detail
