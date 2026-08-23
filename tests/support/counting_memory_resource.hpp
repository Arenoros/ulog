#pragma once

#include <atomic>
#include <cstddef>
#include <memory_resource>

#include "atomic_max.hpp"

namespace ulog::test {

class CountingMemoryResource final : public std::pmr::memory_resource {
 public:
  explicit CountingMemoryResource(
      std::pmr::memory_resource* upstream = std::pmr::get_default_resource()) noexcept
      : upstream_(upstream) {}

  [[nodiscard]] std::size_t allocation_count() const noexcept {
    return allocation_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t current_bytes() const noexcept {
    return current_bytes_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t peak_bytes() const noexcept {
    return peak_bytes_.load(std::memory_order_relaxed);
  }

 private:
  void* do_allocate(std::size_t bytes, std::size_t alignment) override {
    void* const result = upstream_->allocate(bytes, alignment);
    allocation_count_.fetch_add(1, std::memory_order_relaxed);
    const std::size_t current = current_bytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    UpdateRelaxedMaximum(peak_bytes_, current);
    return result;
  }

  void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
    upstream_->deallocate(pointer, bytes, alignment);
    current_bytes_.fetch_sub(bytes, std::memory_order_relaxed);
  }

  [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
    return this == &other;
  }

  std::pmr::memory_resource* upstream_;
  std::atomic<std::size_t> allocation_count_{0};
  std::atomic<std::size_t> current_bytes_{0};
  std::atomic<std::size_t> peak_bytes_{0};
};

}  // namespace ulog::test
