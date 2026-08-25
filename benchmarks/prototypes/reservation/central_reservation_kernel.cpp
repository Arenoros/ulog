#include "prototypes/reservation/central_reservation_kernel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace ulog::benchmark_support::reservation {

void CentralReservationKernel::Prepare(const WorkloadCase& workload) {
  ValidateWorkloadCase(workload);
  initial_retained_bytes_ = InitialOccupancyBytes(workload);
  retained_high_water_bytes_ = initial_retained_bytes_;
  ledger_.Reset(workload.capacity_bytes, initial_retained_bytes_);
  ResetCounters();
}

void CentralReservationKernel::BeginMeasurement() noexcept {
  ResetCounters();
  retained_high_water_bytes_ = ledger_.GetSnapshot().current_bytes;
}

void CentralReservationKernel::ObserveRetainedHighWater() noexcept {
  retained_high_water_bytes_ =
      std::max(retained_high_water_bytes_, ledger_.GetSnapshot().current_bytes);
}

void CentralReservationKernel::EndMeasurement() noexcept {}

CentralReservationKernel::Attempt CentralReservationKernel::TryProduce(
    std::size_t producer_index, std::span<const std::byte> payload) noexcept {
  auto& counters = producer_counters_[producer_index];
  ++counters.attempted_records;
  auto ownership = ledger_.TryBuild(
      payload.size(), [](std::size_t reserved_bytes) noexcept { return reserved_bytes; });
  if (!ownership) {
    ++counters.rejected_records;
    return Attempt{AttemptStatus::kRejected, CentralReservationLedger::Ownership{}};
  }

  ++counters.accepted_records;
  return Attempt{AttemptStatus::kAccepted, std::move(ownership)};
}

void CentralReservationKernel::Release(Attempt& attempt) noexcept { attempt.ownership_.Release(); }

KernelSnapshot CentralReservationKernel::Snapshot() const noexcept {
  const auto ledger_snapshot = ledger_.GetSnapshot();
  const auto initial_retained_bytes = static_cast<std::uint64_t>(initial_retained_bytes_);
  const auto current_retained_bytes = static_cast<std::uint64_t>(ledger_snapshot.current_bytes);
  const auto retained_high_water_bytes = static_cast<std::uint64_t>(retained_high_water_bytes_);
  const auto retained_limit_bytes = static_cast<std::uint64_t>(ledger_snapshot.limit_bytes);
  std::uint64_t attempted_records = 0;
  std::uint64_t accepted_records = 0;
  std::uint64_t rejected_records = 0;
  for (const auto& counters : producer_counters_) {
    attempted_records += counters.attempted_records;
    accepted_records += counters.accepted_records;
    rejected_records += counters.rejected_records;
  }

  return KernelSnapshot{
      .attempted_records = attempted_records,
      .accepted_records = accepted_records,
      .rejected_records = rejected_records,
      .allocation_count = 0,
      .allocation_failure_count = 0,
      .logical_retained_initial_bytes = initial_retained_bytes,
      .logical_retained_high_water_bytes = retained_high_water_bytes,
      .logical_retained_current_bytes = current_retained_bytes,
      .logical_retained_limit_bytes = retained_limit_bytes,
      .physical_retained_initial_bytes = initial_retained_bytes,
      .physical_retained_high_water_bytes = retained_high_water_bytes,
      .physical_retained_current_bytes = current_retained_bytes,
      .physical_retained_limit_bytes = retained_limit_bytes,
  };
}

void CentralReservationKernel::ResetCounters() noexcept {
  for (auto& counters : producer_counters_) {
    counters = ProducerCounters{};
  }
}

}  // namespace ulog::benchmark_support::reservation
