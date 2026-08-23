#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <new>

#include "support/bounded_memory_resource.hpp"
#include "support/counting_memory_resource.hpp"

namespace {

void AllocateAndDeallocate(std::pmr::memory_resource& resource, std::size_t bytes) {
  void* const allocation = resource.allocate(bytes, alignof(std::max_align_t));
  resource.deallocate(allocation, bytes, alignof(std::max_align_t));
}

TEST(CountingMemoryResource, TracksCurrentAndPeakBytes) {
  ulog::test::CountingMemoryResource resource;

  void* const first = resource.allocate(32, alignof(std::max_align_t));
  void* const second = resource.allocate(64, alignof(std::max_align_t));
  EXPECT_EQ(resource.allocation_count(), 2U);
  EXPECT_EQ(resource.current_bytes(), 96U);
  EXPECT_EQ(resource.peak_bytes(), 96U);

  resource.deallocate(second, 64, alignof(std::max_align_t));
  resource.deallocate(first, 32, alignof(std::max_align_t));
  EXPECT_EQ(resource.current_bytes(), 0U);
  EXPECT_EQ(resource.peak_bytes(), 96U);
}

TEST(BoundedMemoryResource, RejectsAnAllocationBeyondTheBudget) {
  ulog::test::BoundedMemoryResource resource{64};

  void* const allocation = resource.allocate(48, alignof(std::max_align_t));
  EXPECT_THROW(AllocateAndDeallocate(resource, 17), std::bad_alloc);
  EXPECT_EQ(resource.current_bytes(), 48U);
  EXPECT_EQ(resource.peak_bytes(), 48U);

  resource.deallocate(allocation, 48, alignof(std::max_align_t));
  EXPECT_EQ(resource.current_bytes(), 0U);
}

TEST(BoundedMemoryResource, RollsBackReservationWhenUpstreamFails) {
  ulog::test::BoundedMemoryResource resource{64, std::pmr::null_memory_resource()};

  EXPECT_THROW(AllocateAndDeallocate(resource, 32), std::bad_alloc);
  EXPECT_EQ(resource.current_bytes(), 0U);
  EXPECT_EQ(resource.peak_bytes(), 32U);
}

}  // namespace
