#include "prototypes/reservation/producer_credit_reservation_kernel.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace ulog::benchmark_support::reservation {

void ProducerCreditReservationKernel::Prepare(const WorkloadCase& workload) {
  ValidateWorkloadCase(workload);
  ledger_.Reset(workload.capacity_bytes, InitialOccupancyBytes(workload), workload.producer_count);
  logical_initial_bytes_ = 0;
  logical_high_water_bytes_ = 0;
  physical_initial_bytes_ = 0;
  physical_high_water_bytes_ = 0;
  ResetCounters();
}

void ProducerCreditReservationKernel::BeginMeasurement() noexcept {
  const auto initial_snapshot = ledger_.GetSnapshot();
  logical_initial_bytes_ = initial_snapshot.logical_retained_bytes;
  logical_high_water_bytes_ = logical_initial_bytes_;
  physical_initial_bytes_ = initial_snapshot.physical_retained_bytes;
  physical_high_water_bytes_ = physical_initial_bytes_;
  ResetCounters();
}

void ProducerCreditReservationKernel::ObserveRetainedHighWater() noexcept {
  const auto snapshot = ledger_.GetSnapshot();
  logical_high_water_bytes_ = std::max(logical_high_water_bytes_, snapshot.logical_retained_bytes);
  physical_high_water_bytes_ =
      std::max(physical_high_water_bytes_, snapshot.physical_retained_bytes);
}

void ProducerCreditReservationKernel::EndMeasurement() { ledger_.ReturnAllCredits(); }

ProducerCreditReservationKernel::Attempt ProducerCreditReservationKernel::TryProduce(
    std::size_t producer_index, std::span<const std::byte> payload) noexcept {
  auto& counters = producer_counters_[producer_index];
  ++counters.attempted_records;
  auto ownership =
      ledger_.TryBuild(producer_index, payload.size(),
                       [](std::size_t reserved_bytes) noexcept { return reserved_bytes; });
  if (!ownership) {
    ++counters.rejected_records;
    return Attempt{AttemptStatus::kRejected, ProducerCreditLedger::Ownership{}};
  }

  ++counters.accepted_records;
  return Attempt{AttemptStatus::kAccepted, std::move(ownership)};
}

void ProducerCreditReservationKernel::Release(Attempt& attempt) noexcept {
  attempt.ownership_.Release();
}

KernelSnapshot ProducerCreditReservationKernel::Snapshot() const noexcept {
  const auto ledger_snapshot = ledger_.GetSnapshot();
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
      .logical_retained_initial_bytes = static_cast<std::uint64_t>(logical_initial_bytes_),
      .logical_retained_high_water_bytes = static_cast<std::uint64_t>(logical_high_water_bytes_),
      .logical_retained_current_bytes =
          static_cast<std::uint64_t>(ledger_snapshot.logical_retained_bytes),
      .logical_retained_limit_bytes = static_cast<std::uint64_t>(ledger_snapshot.capacity_bytes),
      .physical_retained_initial_bytes = static_cast<std::uint64_t>(physical_initial_bytes_),
      .physical_retained_high_water_bytes = static_cast<std::uint64_t>(
          std::max(physical_high_water_bytes_, ledger_snapshot.physical_retained_bytes)),
      .physical_retained_current_bytes =
          static_cast<std::uint64_t>(ledger_snapshot.physical_retained_bytes),
      .physical_retained_limit_bytes = static_cast<std::uint64_t>(ledger_snapshot.capacity_bytes),
  };
}

void ProducerCreditReservationKernel::ResetCounters() noexcept {
  for (auto& counters : producer_counters_) {
    counters = ProducerCounters{};
  }
}

}  // namespace ulog::benchmark_support::reservation
