#pragma once
#include <type_traits>

namespace ulog::utils {

    /// @brief Function that extracts integral value from enum or StrongTypedef
    template <class T, std::enable_if_t<std::is_enum_v<T>, int> /*Enable*/ = 0>
    constexpr auto UnderlyingValue(T v) noexcept {
        return static_cast<std::underlying_type_t<T>>(v);
    }

}  // namespace ulog::utils