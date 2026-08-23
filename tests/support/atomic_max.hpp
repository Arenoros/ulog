#pragma once

#include <atomic>
#include <cstddef>

namespace ulog::test {

inline void UpdateRelaxedMaximum(std::atomic<std::size_t>& maximum,
                                 std::size_t candidate) noexcept {
  std::size_t current = maximum.load(std::memory_order_relaxed);
  while (current < candidate &&
         !maximum.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
  }
}

}  // namespace ulog::test
