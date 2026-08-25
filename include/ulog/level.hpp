#pragma once

#include <cstdint>

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

}  // namespace ulog
