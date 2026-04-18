#pragma once

/// @file ulog/level.hpp
/// @brief Log severity levels.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace ulog {

/// Log severity levels.
enum class Level : std::uint8_t {
    kTrace = 0,     ///< Very verbose debug messages
    kDebug = 1,     ///< Debug messages
    kInfo = 2,      ///< Informational messages
    kWarning = 3,   ///< Warning messages
    kError = 4,     ///< Error messages
    kCritical = 5,  ///< Fatal errors, non-disableable
    kNone = 6,      ///< Do not output messages
};

inline constexpr auto kLevelMax = static_cast<int>(Level::kNone);

/// Parses a lowercase level name; throws std::runtime_error if unknown.
Level LevelFromString(std::string_view name);

/// Lowercase string representation: "trace", "info", ...
std::string_view ToString(Level level);

/// Uppercase string representation: "TRACE", "INFO", ...
std::string_view ToUpperCaseString(Level level) noexcept;

/// Returns std::nullopt if input is std::nullopt, otherwise LevelFromString.
std::optional<Level> OptionalLevelFromString(const std::optional<std::string>& name);

}  // namespace ulog

namespace std {
template <>
struct hash<::ulog::Level> {
    std::size_t operator()(::ulog::Level level) const noexcept {
        return static_cast<std::size_t>(level);
    }
};
}  // namespace std
