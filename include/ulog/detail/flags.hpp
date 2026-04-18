#pragma once

/// @file ulog/detail/flags.hpp
/// @brief Minimal type-safe bit flags helper (subset of userver utils::Flags).

#include <type_traits>

namespace ulog::detail {

template <typename Enum>
class Flags {
public:
    static_assert(std::is_enum_v<Enum>);
    using UnderlyingType = std::underlying_type_t<Enum>;

    constexpr Flags() noexcept = default;
    constexpr Flags(Enum value) noexcept : value_(static_cast<UnderlyingType>(value)) {}

    constexpr Flags& operator|=(Flags rhs) noexcept {
        value_ |= rhs.value_;
        return *this;
    }
    constexpr Flags& operator&=(Flags rhs) noexcept {
        value_ &= rhs.value_;
        return *this;
    }
    constexpr Flags& Clear(Flags rhs) noexcept {
        value_ &= ~rhs.value_;
        return *this;
    }

    constexpr friend Flags operator|(Flags a, Flags b) noexcept { return Flags(static_cast<UnderlyingType>(a.value_ | b.value_)); }
    constexpr friend Flags operator&(Flags a, Flags b) noexcept { return Flags(static_cast<UnderlyingType>(a.value_ & b.value_)); }

    constexpr explicit operator bool() const noexcept { return value_ != 0; }
    constexpr UnderlyingType GetValue() const noexcept { return value_; }

    constexpr bool operator==(Flags rhs) const noexcept { return value_ == rhs.value_; }
    constexpr bool operator!=(Flags rhs) const noexcept { return value_ != rhs.value_; }

private:
    constexpr explicit Flags(UnderlyingType raw) noexcept : value_(raw) {}
    UnderlyingType value_{};
};

}  // namespace ulog::detail
