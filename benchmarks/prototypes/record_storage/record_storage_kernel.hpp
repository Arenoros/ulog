#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "prototypes/record_storage/record_storage.hpp"
#include "support/workload_harness.hpp"

namespace ulog::benchmark_support::record_storage {
namespace impl {

template <typename Policy>
[[nodiscard]] constexpr std::string_view CandidateName() noexcept {
  if constexpr (std::same_as<Policy, ContiguousPolicy>) {
    return "contiguous-record";
  } else if constexpr (std::same_as<Policy, ChunkedPolicy>) {
    return "chunked-record";
  } else {
    static_assert(std::same_as<Policy, HybridPolicy>);
    return "hybrid-record";
  }
}

[[nodiscard]] constexpr bool SameFootprint(const RecordFootprint& left,
                                           const RecordFootprint& right) noexcept {
  return left.requested_message_bytes == right.requested_message_bytes &&
         left.stored_message_bytes == right.stored_message_bytes &&
         left.owned_payload_bytes == right.owned_payload_bytes &&
         left.metadata_bytes == right.metadata_bytes &&
         left.fragmentation_bytes == right.fragmentation_bytes &&
         left.accounting_charge_bytes == right.accounting_charge_bytes &&
         left.minimum_accounting_charge_bytes == right.minimum_accounting_charge_bytes &&
         left.truncated == right.truncated;
}

}  // namespace impl

template <typename Policy>
class RecordStorageKernel final {
 public:
  class Attempt final {
   public:
    Attempt(Attempt&& other) noexcept
        : status_(other.status_),
          producer_index_(other.producer_index_),
          serialized_bytes_(other.serialized_bytes_),
          accounting_charge_bytes_(other.accounting_charge_bytes_),
          active_(std::exchange(other.active_, false)) {}

    Attempt& operator=(Attempt&&) = delete;
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;

    [[nodiscard]] AttemptStatus status() const noexcept { return status_; }

   private:
    friend class RecordStorageKernel;

    explicit Attempt(AttemptStatus status) noexcept : status_(status) {}

    Attempt(std::size_t producer_index, std::uint64_t serialized_bytes,
            std::uint64_t accounting_charge_bytes) noexcept
        : status_(AttemptStatus::kAccepted),
          producer_index_(producer_index),
          serialized_bytes_(serialized_bytes),
          accounting_charge_bytes_(accounting_charge_bytes),
          active_(true) {}

    AttemptStatus status_{AttemptStatus::kRejected};
    std::size_t producer_index_{0};
    std::uint64_t serialized_bytes_{0};
    std::uint64_t accounting_charge_bytes_{0};
    bool active_{false};
  };

  [[nodiscard]] static constexpr std::string_view Name() noexcept {
    return impl::CandidateName<Policy>();
  }

  void Prepare(const WorkloadCase& workload) {
    if (workload.producer_count > kMaxProducerCount) {
      throw std::invalid_argument(
          "Record-storage workload exceeds 32 producer slots; reduce producer_count and retry.");
    }
    producer_count_ = workload.producer_count;
    retained_limit_bytes_ = static_cast<std::uint64_t>(workload.capacity_bytes);
    initial_retained_bytes_ = static_cast<std::uint64_t>(InitialOccupancyBytes(workload));
    logical_retained_bytes_.store(initial_retained_bytes_, std::memory_order_relaxed);
    physical_retained_bytes_.store(initial_retained_bytes_, std::memory_order_relaxed);
    logical_retained_high_water_bytes_ = initial_retained_bytes_;
    physical_retained_high_water_bytes_ = initial_retained_bytes_;
    record_validation_error_count_.store(0, std::memory_order_relaxed);
    ResetCounters();
    for (auto& slot : record_slots_) {
      slot.Reset();
    }
  }

  void BeginMeasurement() noexcept {
    ResetCounters();
    record_validation_error_count_.store(0, std::memory_order_relaxed);
    logical_retained_high_water_bytes_ = logical_retained_bytes_.load(std::memory_order_relaxed);
    physical_retained_high_water_bytes_ = physical_retained_bytes_.load(std::memory_order_relaxed);
  }

  void ObserveRetainedHighWater() noexcept {
    logical_retained_high_water_bytes_ =
        std::max(logical_retained_high_water_bytes_,
                 logical_retained_bytes_.load(std::memory_order_relaxed));
    physical_retained_high_water_bytes_ =
        std::max(physical_retained_high_water_bytes_,
                 physical_retained_bytes_.load(std::memory_order_relaxed));
  }

  void EndMeasurement() noexcept {
    for (std::size_t index = 0; index < producer_count_; ++index) {
      record_slots_[index].Reset();
    }
  }

  [[nodiscard]] RecordFootprint DescribeRecord(std::span<const std::byte> payload) const noexcept {
    return record_storage::DescribeRecord<Policy>(payload);
  }

  [[nodiscard]] Attempt TryProduce(std::size_t producer_index,
                                   std::span<const std::byte> payload) noexcept {
    if (producer_index >= producer_count_ || producer_index >= kMaxProducerCount) {
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return Attempt{AttemptStatus::kRejected};
    }

    auto& counters = producer_counters_[producer_index];
    ++counters.attempted_records;
    const RecordFootprint footprint = DescribeRecord(payload);
    if (!TryReservePhysical(footprint.accounting_charge_bytes)) {
      ++counters.rejected_records;
      return Attempt{AttemptStatus::kRejected};
    }

    auto& slot = record_slots_[producer_index];
    auto writer = slot.Begin(BenchmarkRecordSeed(), footprint);
    const RecordView view = BuildBenchmarkRecord<Policy>(std::move(writer), payload);
    const RecordFootprint actual_footprint = view.footprint();
    if (!impl::SameFootprint(actual_footprint, footprint)) {
      slot.Reset();
      physical_retained_bytes_.fetch_sub(footprint.accounting_charge_bytes,
                                         std::memory_order_relaxed);
      ++counters.rejected_records;
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return Attempt{AttemptStatus::kRejected};
    }

    logical_retained_bytes_.fetch_add(footprint.SerializedBytes(), std::memory_order_relaxed);
    ++counters.accepted_records;
    return Attempt{producer_index, footprint.SerializedBytes(), footprint.accounting_charge_bytes};
  }

  void Release(Attempt& attempt) noexcept {
    if (!attempt.active_) {
      return;
    }
    record_slots_[attempt.producer_index_].Reset();
    logical_retained_bytes_.fetch_sub(attempt.serialized_bytes_, std::memory_order_relaxed);
    physical_retained_bytes_.fetch_sub(attempt.accounting_charge_bytes_, std::memory_order_relaxed);
    attempt.active_ = false;
  }

  [[nodiscard]] KernelSnapshot Snapshot() const noexcept {
    std::uint64_t attempted_records = 0;
    std::uint64_t accepted_records = 0;
    std::uint64_t rejected_records = 0;
    for (std::size_t index = 0; index < producer_count_; ++index) {
      attempted_records += producer_counters_[index].attempted_records;
      accepted_records += producer_counters_[index].accepted_records;
      rejected_records += producer_counters_[index].rejected_records;
    }

    const std::uint64_t logical_current = logical_retained_bytes_.load(std::memory_order_relaxed);
    const std::uint64_t physical_current = physical_retained_bytes_.load(std::memory_order_relaxed);
    return KernelSnapshot{
        .attempted_records = attempted_records,
        .accepted_records = accepted_records,
        .rejected_records = rejected_records,
        .allocation_count = 0,
        .allocation_failure_count = 0,
        .logical_retained_initial_bytes = initial_retained_bytes_,
        .logical_retained_high_water_bytes =
            std::max(logical_retained_high_water_bytes_, logical_current),
        .logical_retained_current_bytes = logical_current,
        .logical_retained_limit_bytes = retained_limit_bytes_,
        .physical_retained_initial_bytes = initial_retained_bytes_,
        .physical_retained_high_water_bytes =
            std::max(physical_retained_high_water_bytes_, physical_current),
        .physical_retained_current_bytes = physical_current,
        .physical_retained_limit_bytes = retained_limit_bytes_,
    };
  }

  [[nodiscard]] std::uint64_t record_validation_error_count() const noexcept {
    return record_validation_error_count_.load(std::memory_order_relaxed);
  }

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

  [[nodiscard]] bool TryReservePhysical(std::uint64_t charge) noexcept {
    std::uint64_t current = physical_retained_bytes_.load(std::memory_order_relaxed);
    while (current <= retained_limit_bytes_ && charge <= retained_limit_bytes_ - current) {
      if (physical_retained_bytes_.compare_exchange_weak(
              current, current + charge, std::memory_order_relaxed, std::memory_order_relaxed)) {
        return true;
      }
    }
    return false;
  }

  void ResetCounters() noexcept {
    for (auto& counters : producer_counters_) {
      counters.attempted_records = 0;
      counters.accepted_records = 0;
      counters.rejected_records = 0;
    }
  }

  std::size_t producer_count_{0};
  std::uint64_t retained_limit_bytes_{0};
  std::uint64_t initial_retained_bytes_{0};
  std::uint64_t logical_retained_high_water_bytes_{0};
  std::uint64_t physical_retained_high_water_bytes_{0};
  std::atomic<std::uint64_t> logical_retained_bytes_{0};
  std::atomic<std::uint64_t> physical_retained_bytes_{0};
  std::atomic<std::uint64_t> record_validation_error_count_{0};
  std::array<RecordSlot<Policy>, kMaxProducerCount> record_slots_{};
  std::array<ProducerCounters, kMaxProducerCount> producer_counters_{};
};

using ContiguousRecordStorageKernel = RecordStorageKernel<ContiguousPolicy>;
using ChunkedRecordStorageKernel = RecordStorageKernel<ChunkedPolicy>;
using HybridRecordStorageKernel = RecordStorageKernel<HybridPolicy>;

static_assert(WorkloadKernel<ContiguousRecordStorageKernel>);
static_assert(WorkloadKernel<ChunkedRecordStorageKernel>);
static_assert(WorkloadKernel<HybridRecordStorageKernel>);

}  // namespace ulog::benchmark_support::record_storage
