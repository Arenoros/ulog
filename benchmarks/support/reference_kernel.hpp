#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "support/workload_harness.hpp"

namespace ulog::benchmark_support {

class ReferenceLedgerKernel final {
 public:
  class Attempt final {
   public:
    Attempt(AttemptStatus status, std::size_t retained_bytes) noexcept
        : status_(status), retained_bytes_(retained_bytes) {}

    [[nodiscard]] AttemptStatus status() const noexcept { return status_; }
    [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }

   private:
    AttemptStatus status_;
    std::size_t retained_bytes_;
  };

  [[nodiscard]] static constexpr std::string_view Name() noexcept { return "reference-ledger"; }

  void Prepare(const WorkloadCase& workload);
  void BeginMeasurement() noexcept;
  void ObserveRetainedHighWater() noexcept;
  void EndMeasurement() noexcept {}
  [[nodiscard]] Attempt TryProduce(std::size_t producer_index,
                                   std::span<const std::byte> payload) noexcept;
  void Release(Attempt& attempt) noexcept;
  [[nodiscard]] KernelSnapshot Snapshot() const noexcept;

 private:
  void UpdateHighWater(std::size_t retained_bytes) noexcept;

  std::size_t capacity_bytes_{0};
  std::size_t initial_retained_bytes_{0};
  std::atomic<std::size_t> retained_bytes_{0};
  std::atomic<std::size_t> retained_high_water_bytes_{0};
  std::atomic<std::uint64_t> attempted_records_{0};
  std::atomic<std::uint64_t> accepted_records_{0};
  std::atomic<std::uint64_t> rejected_records_{0};
};

}  // namespace ulog::benchmark_support
