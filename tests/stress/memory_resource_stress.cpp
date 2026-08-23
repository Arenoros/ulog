#include <cstddef>
#include <memory_resource>
#include <thread>
#include <vector>

#include "support/counting_memory_resource.hpp"

int RunStressTest() {
  constexpr std::size_t kThreadCount = 4;
  constexpr std::size_t kIterationsPerThread = 2'000;
  constexpr std::size_t kAllocationSize = 64;

  ulog::test::CountingMemoryResource resource;
  std::vector<std::thread> threads;
  threads.reserve(kThreadCount);
  for (std::size_t thread_index = 0; thread_index < kThreadCount; ++thread_index) {
    threads.emplace_back([&resource] {
      for (std::size_t iteration = 0; iteration < kIterationsPerThread; ++iteration) {
        void* const allocation = resource.allocate(kAllocationSize, alignof(std::max_align_t));
        resource.deallocate(allocation, kAllocationSize, alignof(std::max_align_t));
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  const std::size_t expected_allocations = kThreadCount * kIterationsPerThread;
  return resource.allocation_count() == expected_allocations && resource.current_bytes() == 0 ? 0
                                                                                              : 1;
}

int main() noexcept {
  try {
    return RunStressTest();
  } catch (...) {
    return 1;
  }
}
