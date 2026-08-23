#pragma once

#include <atomic>
#include <cstddef>
#include <memory_resource>
#include <new>

#include "atomic_max.hpp"

namespace ulog::test {

class BoundedMemoryResource final : public std::pmr::memory_resource {
 public:
  explicit BoundedMemoryResource(
      std::size_t limit_bytes,
      std::pmr::memory_resource* upstream = std::pmr::get_default_resource()) noexcept
      : limit_bytes_(limit_bytes), upstream_(upstream) {}

  [[nodiscard]] std::size_t limit_bytes() const noexcept { return limit_bytes_; }

  [[nodiscard]] std::size_t current_bytes() const noexcept {
    return current_bytes_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t peak_bytes() const noexcept {
    return peak_bytes_.load(std::memory_order_relaxed);
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    Reserve(bytes);
    try {
      return upstream_->allocate(bytes, alignment);
    } catch (...) {
      current_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
      throw;
    }
  }

  void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
    upstream_->deallocate(pointer, bytes, alignment);
    current_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
  }

  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  void Reserve(std::size_t bytes) {
    std::size_t current = current_bytes_.load(std::memory_order_relaxed);
    for (;;) {
      if (current > limit_bytes_ || bytes > limit_bytes_ - current) {
        throw std::bad_alloc{};
      }
      const std::size_t candidate = current + bytes;
      if (current_bytes_.compare_exchange_weak(current, candidate, std::memory_order_relaxed)) {
        UpdateRelaxedMaximum(peak_bytes_, candidate);
        return;
      }
    }
  }

  const std::size_t limit_bytes_;
  std::pmr::memory_resource* upstream_;
  std::atomic<std::size_t> current_bytes_{0};
  std::atomic<std::size_t> peak_bytes_{0};
};

}  // namespace ulog::test
