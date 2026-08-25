#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

#include "prototypes/reservation/producer_credit_ledger.hpp"
#include "support/workload_harness.hpp"

namespace ulog::benchmark_support::reservation {

class ProducerCreditReservationKernel final {
 public:
  class Attempt final {
   public:
    Attempt(Attempt&&) noexcept = default;
    Attempt& operator=(Attempt&&) noexcept = default;
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;

    [[nodiscard]] AttemptStatus status() const noexcept { return status_; }

   private:
    friend class ProducerCreditReservationKernel;

    Attempt(AttemptStatus status, ProducerCreditLedger::Ownership ownership) noexcept
        : status_(status), ownership_(std::move(ownership)) {}

    AttemptStatus status_;
    ProducerCreditLedger::Ownership ownership_;
  };

  [[nodiscard]] static constexpr std::string_view Name() noexcept {
    return "producer-credit-reservation";
  }

  void Prepare(const WorkloadCase& workload);
  void BeginMeasurement() noexcept;
  void ObserveRetainedHighWater() noexcept;
  void EndMeasurement();
  [[nodiscard]] Attempt TryProduce(std::size_t producer_index,
                                   std::span<const std::byte> payload) noexcept;
  void Release(Attempt& attempt) noexcept;
  [[nodiscard]] KernelSnapshot Snapshot() const noexcept;

 private:
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

  ProducerCreditLedger ledger_;
  std::size_t logical_initial_bytes_{0};
  std::size_t logical_high_water_bytes_{0};
  std::size_t physical_initial_bytes_{0};
  std::size_t physical_high_water_bytes_{0};
  std::array<ProducerCounters, ProducerCreditLedger::kMaxProducerCount> producer_counters_{};
};

static_assert(WorkloadKernel<ProducerCreditReservationKernel>);

}  // namespace ulog::benchmark_support::reservation
