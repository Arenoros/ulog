#include "support/reference_kernel.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ulog::benchmark_support {

void ReferenceLedgerKernel::Prepare(const WorkloadCase& workload) {
  ValidateWorkloadCase(workload);
  capacity_bytes_ = workload.capacity_bytes;
  initial_retained_bytes_ = InitialOccupancyBytes(workload);
  retained_bytes_.store(initial_retained_bytes_, std::memory_order_relaxed);
  retained_high_water_bytes_.store(initial_retained_bytes_, std::memory_order_relaxed);
  attempted_records_.store(0, std::memory_order_relaxed);
  accepted_records_.store(0, std::memory_order_relaxed);
  rejected_records_.store(0, std::memory_order_relaxed);
}

void ReferenceLedgerKernel::BeginMeasurement() noexcept {
  attempted_records_.store(0, std::memory_order_relaxed);
  accepted_records_.store(0, std::memory_order_relaxed);
  rejected_records_.store(0, std::memory_order_relaxed);
  retained_high_water_bytes_.store(retained_bytes_.load(std::memory_order_relaxed),
                                   std::memory_order_relaxed);
}

ReferenceLedgerKernel::Attempt ReferenceLedgerKernel::TryProduce(
    [[maybe_unused]] std::size_t producer_index, std::span<const std::byte> payload) noexcept {
  attempted_records_.fetch_add(1, std::memory_order_relaxed);
  const std::size_t record_size_bytes = payload.size();
  std::size_t retained_bytes = retained_bytes_.load(std::memory_order_relaxed);
  for (;;) {
    if (retained_bytes > capacity_bytes_ || record_size_bytes > capacity_bytes_ - retained_bytes) {
      rejected_records_.fetch_add(1, std::memory_order_relaxed);
      return Attempt{AttemptStatus::kRejected, 0};
    }
    const std::size_t retained_after_publish = retained_bytes + record_size_bytes;
    if (retained_bytes_.compare_exchange_weak(retained_bytes, retained_after_publish,
                                              std::memory_order_relaxed)) {
      UpdateHighWater(retained_after_publish);
      accepted_records_.fetch_add(1, std::memory_order_relaxed);
      return Attempt{AttemptStatus::kAccepted, record_size_bytes};
    }
  }
}

void ReferenceLedgerKernel::Release(Attempt& attempt) noexcept {
  if (attempt.status() == AttemptStatus::kAccepted) {
    retained_bytes_.fetch_sub(attempt.retained_bytes(), std::memory_order_relaxed);
  }
}

KernelSnapshot ReferenceLedgerKernel::Snapshot() const noexcept {
  const auto initial_retained_bytes = static_cast<std::uint64_t>(initial_retained_bytes_);
  const auto high_water_bytes =
      static_cast<std::uint64_t>(retained_high_water_bytes_.load(std::memory_order_relaxed));
  const auto current_bytes =
      static_cast<std::uint64_t>(retained_bytes_.load(std::memory_order_relaxed));
  const auto capacity_bytes = static_cast<std::uint64_t>(capacity_bytes_);
  return KernelSnapshot{
      .attempted_records = attempted_records_.load(std::memory_order_relaxed),
      .accepted_records = accepted_records_.load(std::memory_order_relaxed),
      .rejected_records = rejected_records_.load(std::memory_order_relaxed),
      .allocation_count = 0,
      .allocation_failure_count = 0,
      .logical_retained_initial_bytes = initial_retained_bytes,
      .logical_retained_high_water_bytes = high_water_bytes,
      .logical_retained_current_bytes = current_bytes,
      .logical_retained_limit_bytes = capacity_bytes,
      .physical_retained_initial_bytes = initial_retained_bytes,
      .physical_retained_high_water_bytes = high_water_bytes,
      .physical_retained_current_bytes = current_bytes,
      .physical_retained_limit_bytes = capacity_bytes,
  };
}

void ReferenceLedgerKernel::UpdateHighWater(std::size_t retained_bytes) noexcept {
  std::size_t previous = retained_high_water_bytes_.load(std::memory_order_relaxed);
  while (previous < retained_bytes && !retained_high_water_bytes_.compare_exchange_weak(
                                          previous, retained_bytes, std::memory_order_relaxed)) {
  }
}

}  // namespace ulog::benchmark_support
