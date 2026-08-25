#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "prototypes/reservation/central_reservation_ledger.hpp"
#include "support/workload_harness.hpp"

namespace ulog::benchmark_support::reservation {

class CentralReservationKernel final {
 public:
  class Attempt final {
   public:
    Attempt(Attempt&&) noexcept = default;
    Attempt& operator=(Attempt&&) noexcept = default;
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;

    [[nodiscard]] AttemptStatus status() const noexcept { return status_; }

   private:
    friend class CentralReservationKernel;

    Attempt(AttemptStatus status, CentralReservationLedger::Ownership ownership) noexcept
        : status_(status), ownership_(std::move(ownership)) {}

    AttemptStatus status_;
    CentralReservationLedger::Ownership ownership_;
  };

  [[nodiscard]] static constexpr std::string_view Name() noexcept { return "central-reservation"; }

  [[nodiscard]] static constexpr WorkloadAdmissionModel AdmissionModel() noexcept {
    return WorkloadAdmissionModel::kExactCapacity;
  }

  void Prepare(const WorkloadCase& workload);
  void BeginMeasurement() noexcept;
  void ObserveRetainedHighWater() noexcept;
  void EndMeasurement() noexcept;
  [[nodiscard]] RecordFootprint DescribeRecord(std::span<const std::byte> payload) const noexcept {
    return MakePayloadOnlyRecordFootprint(payload.size());
  }
  [[nodiscard]] Attempt TryProduce(std::size_t producer_index,
                                   std::span<const std::byte> payload) noexcept;
  void Release(Attempt& attempt) noexcept;
  [[nodiscard]] KernelSnapshot Snapshot() const noexcept;

 private:
  static constexpr std::size_t kMaxProducerCount = 32;
  static constexpr std::size_t kProducerCounterAlignment = 64;

  struct alignas(kProducerCounterAlignment) ProducerCounters final {
    std::uint64_t attempted_records{0};
    std::uint64_t accepted_records{0};
    std::uint64_t rejected_records{0};
    std::array<std::byte, kProducerCounterAlignment - 3U * sizeof(std::uint64_t)>
        cache_line_padding{};
  };

  static_assert(sizeof(ProducerCounters) == kProducerCounterAlignment);

  void ResetCounters() noexcept;

  CentralReservationLedger ledger_;
  std::size_t initial_retained_bytes_{0};
  std::size_t retained_high_water_bytes_{0};
  std::array<ProducerCounters, kMaxProducerCount> producer_counters_{};
};

static_assert(WorkloadKernel<CentralReservationKernel>);

}  // namespace ulog::benchmark_support::reservation
