#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace ulog::benchmark_support::allocation_tracking {

inline std::atomic<std::uint64_t> allocation_count{0};
inline std::atomic<std::uint64_t> allocation_bytes{0};
inline std::atomic<std::uint64_t> allocation_failure_count{0};

struct AlignedAllocationRequest final {
  std::size_t size{0};
  std::size_t alignment{0};
};

inline void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  allocation_bytes.fetch_add(size, std::memory_order_relaxed);
  if (void* memory = std::malloc(std::max(size, std::size_t{1}))) {
    return memory;
  }
  allocation_failure_count.fetch_add(1, std::memory_order_relaxed);
  throw std::bad_alloc{};
}

inline void* AllocateAligned(AlignedAllocationRequest request) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  allocation_bytes.fetch_add(request.size, std::memory_order_relaxed);
#if defined(_MSC_VER)
  if (void* memory = _aligned_malloc(std::max(request.size, std::size_t{1}), request.alignment)) {
    return memory;
  }
#else
  const std::size_t requested_size = std::max(request.size, std::size_t{1});
  if (requested_size > std::numeric_limits<std::size_t>::max() - request.alignment + 1U) {
    allocation_failure_count.fetch_add(1, std::memory_order_relaxed);
    throw std::bad_alloc{};
  }
  const std::size_t aligned_size =
      ((requested_size + request.alignment - 1U) / request.alignment) * request.alignment;
  if (void* memory = std::aligned_alloc(request.alignment, aligned_size)) {
    return memory;
  }
#endif
  allocation_failure_count.fetch_add(1, std::memory_order_relaxed);
  throw std::bad_alloc{};
}

inline void FreeAligned(void* memory) noexcept {
#if defined(_MSC_VER)
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

}  // namespace ulog::benchmark_support::allocation_tracking

// Include this private header in exactly one translation unit per test executable.
void* operator new(std::size_t size) {
  return ulog::benchmark_support::allocation_tracking::Allocate(size);
}
void* operator new[](std::size_t size) {
  return ulog::benchmark_support::allocation_tracking::Allocate(size);
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  return ulog::benchmark_support::allocation_tracking::AllocateAligned(
      {.size = size, .alignment = static_cast<std::size_t>(alignment)});
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ulog::benchmark_support::allocation_tracking::AllocateAligned(
      {.size = size, .alignment = static_cast<std::size_t>(alignment)});
}
void operator delete(void* memory, std::align_val_t) noexcept {
  ulog::benchmark_support::allocation_tracking::FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  ulog::benchmark_support::allocation_tracking::FreeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  ulog::benchmark_support::allocation_tracking::FreeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  ulog::benchmark_support::allocation_tracking::FreeAligned(memory);
}
