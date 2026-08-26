#pragma once

#include <cstdint>

namespace ulog::detail::control {

enum class UlogThreadRole : std::uint8_t { kExternal, kWorker, kIo, kCallback };

[[nodiscard]] UlogThreadRole GetUlogThreadRole() noexcept;

class ScopedUlogThreadRole final {
 public:
  explicit ScopedUlogThreadRole(UlogThreadRole role) noexcept;
  ~ScopedUlogThreadRole();

  ScopedUlogThreadRole(const ScopedUlogThreadRole&) = delete;
  ScopedUlogThreadRole& operator=(const ScopedUlogThreadRole&) = delete;

 private:
  UlogThreadRole previous_;
};

}  // namespace ulog::detail::control
