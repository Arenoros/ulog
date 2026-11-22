#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ulog {

enum class Level : std::uint8_t {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarning = 3,
  kError = 4,
  kCritical = 5,
  kNone = 6,
};

[[nodiscard]] constexpr bool IsLevelEnabled(Level message_level, Level threshold) noexcept {
  const auto message = static_cast<std::uint8_t>(message_level);
  const auto minimum = static_cast<std::uint8_t>(threshold);
  const auto none = static_cast<std::uint8_t>(Level::kNone);
  return message < none && minimum <= none && message >= minimum;
}

namespace detail {

inline constexpr std::array<std::string_view, 7> kLevelNames{"trace", "debug",    "info", "warning",
                                                             "error", "critical", "none"};
inline constexpr std::array<std::string_view, 7> kUpperCaseLevelNames{
    "TRACE", "DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL", "NONE"};

[[nodiscard]] constexpr bool EqualsIgnoringAsciiCase(std::string_view text,
                                                     std::string_view lower) noexcept {
  if (text.size() != lower.size()) {
    return false;
  }
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char byte = text[index];
    if ((byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte) != lower[index]) {
      return false;
    }
  }
  return true;
}

}  // namespace detail

/// Lowercase canonical name, such as "warning"; empty for a value that is not a Level enumerator.
[[nodiscard]] constexpr std::string_view ToString(Level level) noexcept {
  const auto index = static_cast<std::size_t>(level);
  return index < detail::kLevelNames.size() ? detail::kLevelNames[index] : std::string_view{};
}

/// Uppercase canonical name, such as "WARNING"; empty for a value that is not a Level enumerator.
[[nodiscard]] constexpr std::string_view ToUpperCaseString(Level level) noexcept {
  const auto index = static_cast<std::size_t>(level);
  return index < detail::kUpperCaseLevelNames.size() ? detail::kUpperCaseLevelNames[index]
                                                     : std::string_view{};
}

struct LevelParseResult final {
  std::optional<Level> level{};

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return level.has_value(); }
  /// Correction for a rejected name; names every accepted value.
  [[nodiscard]] constexpr std::string_view HowToFix() const noexcept {
    return level ? std::string_view{}
                 : std::string_view{
                       "Use one of the level names trace, debug, info, warning, error, critical, "
                       "or none, in any letter case."};
  }
};

/// Parses one of the seven level names, ignoring ASCII case. Anything else is rejected.
[[nodiscard]] constexpr LevelParseResult ParseLevel(std::string_view name) noexcept {
  for (std::size_t index = 0; index < detail::kLevelNames.size(); ++index) {
    if (detail::EqualsIgnoringAsciiCase(name, detail::kLevelNames[index])) {
      return {.level = static_cast<Level>(index)};
    }
  }
  return {};
}

}  // namespace ulog
