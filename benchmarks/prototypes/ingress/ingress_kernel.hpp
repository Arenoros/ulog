#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "bounded_mpsc_ring.hpp"
#include "chunked_mpsc.hpp"
#include "per_producer_lanes.hpp"
#include "prototypes/record_storage/record_storage.hpp"
#include "support/workload_harness.hpp"

namespace ulog::benchmark_support::ingress {

template <typename Topology>
class IngressKernel final {
 public:
  class Attempt final {
   public:
    Attempt(Attempt&& other) noexcept
        : status_(other.status_), active_(std::exchange(other.active_, false)) {}

    Attempt& operator=(Attempt&&) = delete;
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;

    [[nodiscard]] AttemptStatus status() const noexcept { return status_; }

   private:
    friend class IngressKernel;

    Attempt(AttemptStatus status, bool active) noexcept : status_(status), active_(active) {}

    AttemptStatus status_{AttemptStatus::kRejected};
    bool active_{false};
  };

  [[nodiscard]] static constexpr std::string_view Name() noexcept { return Topology::Name(); }

  [[nodiscard]] static constexpr WorkloadAdmissionModel AdmissionModel() noexcept {
    return WorkloadAdmissionModel::kCapacityUpperBound;
  }

  void Prepare(const WorkloadCase& workload) {
    if (prepared_) {
      throw std::logic_error(
          "IngressKernel may be prepared only once because topology sequences are monotonic; "
          "construct a fresh benchmark kernel and retry.");
    }
    if (workload.producer_count == 0 || workload.producer_count > kMaximumConcurrentPublishers) {
      throw std::invalid_argument(
          "Ingress workload producer_count must be between 1 and 32; use the maintained matrix "
          "and retry.");
    }

    if constexpr (std::is_default_constructible_v<Topology>) {
      topology_.emplace();
    } else {
      static_assert(std::is_constructible_v<Topology, std::size_t>);
      topology_.emplace(workload.producer_count);
    }
    prepared_ = true;
    producer_count_ = workload.producer_count;
    retained_limit_bytes_ = static_cast<std::uint64_t>(workload.capacity_bytes);
    initial_retained_bytes_ = static_cast<std::uint64_t>(InitialOccupancyBytes(workload));
    physical_retained_bytes_.store(initial_retained_bytes_, std::memory_order_relaxed);
    logical_retained_high_water_bytes_ = initial_retained_bytes_;
    physical_retained_high_water_bytes_ = initial_retained_bytes_;
    release_arrivals_.store(0, std::memory_order_relaxed);
    expected_sequence_.store(0, std::memory_order_relaxed);
    ResetMeasurementState();
    for (auto& slot : record_slots_) {
      slot.Reset();
    }
  }

  void BeginMeasurement() noexcept {
    if (!topology_) {
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ResetProducerCounters();
    dequeued_serialized_bytes_.store(0, std::memory_order_relaxed);
    topology_measurement_baseline_ = topology_->GetSnapshot();
    fifo_error_count_.store(0, std::memory_order_relaxed);
    sequence_error_count_.store(0, std::memory_order_relaxed);
    record_validation_error_count_.store(0, std::memory_order_relaxed);
    logical_retained_high_water_bytes_ = initial_retained_bytes_;
    physical_retained_high_water_bytes_ = physical_retained_bytes_.load(std::memory_order_relaxed);
  }

  void ObserveRetainedHighWater() noexcept {
    logical_retained_high_water_bytes_ =
        std::max(logical_retained_high_water_bytes_, LogicalRetainedBytes());
    physical_retained_high_water_bytes_ =
        std::max(physical_retained_high_water_bytes_,
                 physical_retained_bytes_.load(std::memory_order_relaxed));
  }

  void EndMeasurement() noexcept {
    if (!topology_ || release_arrivals_.load(std::memory_order_relaxed) != 0 ||
        topology_->GetSnapshot().retained_records != 0) {
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  [[nodiscard]] RecordFootprint DescribeRecord(std::span<const std::byte> payload) const noexcept {
    return record_storage::DescribeRecord<record_storage::ContiguousPolicy>(payload);
  }

  [[nodiscard]] Attempt TryProduce(std::size_t producer_index,
                                   std::span<const std::byte> payload) noexcept {
    if (!prepared_ || !topology_ || producer_index >= producer_count_) {
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return Attempt{AttemptStatus::kRejected, false};
    }

    ProducerCounters& counters = producer_counters_[producer_index];
    ++counters.attempted_records;
    const RecordFootprint footprint = DescribeRecord(payload);
    if (!TryReservePhysical(footprint.accounting_charge_bytes)) {
      ++counters.rejected_records;
      return Attempt{AttemptStatus::kRejected, true};
    }

    auto& slot = record_slots_[producer_index];
    auto writer = slot.Begin(record_storage::BenchmarkRecordSeed(), footprint);
    const record_storage::RecordView view =
        record_storage::BuildBenchmarkRecord<record_storage::ContiguousPolicy>(std::move(writer),
                                                                               payload);
    if (!view || !SameFootprint(view.footprint(), footprint)) {
      slot.Reset();
      physical_retained_bytes_.fetch_sub(footprint.accounting_charge_bytes,
                                         std::memory_order_relaxed);
      ++counters.rejected_records;
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return Attempt{AttemptStatus::kRejected, true};
    }

    const std::uint64_t generation = ++slot_generations_[producer_index];
    record_views_[producer_index] = view;
    const PublishResult published = topology_->TryPublish(
        producer_index, RecordHandle{
                            .slot_index = static_cast<std::uint32_t>(producer_index),
                            .generation = generation,
                            .serialized_bytes = footprint.SerializedBytes(),
                            .accounting_charge_bytes = footprint.accounting_charge_bytes,
                        });
    counters.maximum_publication_actions =
        std::max(counters.maximum_publication_actions, published.publication_actions);
    if (published.status != PublishStatus::kAccepted || !published.admission_sequence) {
      slot.Reset();
      physical_retained_bytes_.fetch_sub(footprint.accounting_charge_bytes,
                                         std::memory_order_relaxed);
      ++counters.rejected_records;
      return Attempt{AttemptStatus::kRejected, true};
    }

    ++counters.accepted_records;
    counters.accepted_serialized_bytes += footprint.SerializedBytes();
    return Attempt{AttemptStatus::kAccepted, true};
  }

  void Release(Attempt& attempt) noexcept {
    if (!attempt.active_) {
      return;
    }
    attempt.active_ = false;
    const std::size_t arrivals = release_arrivals_.fetch_add(1, std::memory_order_acq_rel) + 1U;
    if (arrivals != producer_count_) {
      return;
    }

    DrainPublishedRecords();
    release_arrivals_.store(0, std::memory_order_release);
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

    const std::uint64_t logical_current = LogicalRetainedBytes();
    const std::uint64_t physical_current = physical_retained_bytes_.load(std::memory_order_relaxed);
    return {
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

  [[nodiscard]] TopologySnapshot MeasurementTopologySnapshot() const noexcept {
    if (!topology_) {
      return {};
    }
    const TopologySnapshot current = topology_->GetSnapshot();
    return {
        .attempted_records =
            current.attempted_records - topology_measurement_baseline_.attempted_records,
        .enqueued_records =
            current.enqueued_records - topology_measurement_baseline_.enqueued_records,
        .dequeued_records =
            current.dequeued_records - topology_measurement_baseline_.dequeued_records,
        .rejected_records =
            current.rejected_records - topology_measurement_baseline_.rejected_records,
        .full_rejections = current.full_rejections - topology_measurement_baseline_.full_rejections,
        .contention_rejections =
            current.contention_rejections - topology_measurement_baseline_.contention_rejections,
        .invalid_rejections =
            current.invalid_rejections - topology_measurement_baseline_.invalid_rejections,
        .retained_records = current.retained_records,
        .retained_serialized_bytes = current.retained_serialized_bytes,
        .retained_charge_bytes = current.retained_charge_bytes,
    };
  }

  [[nodiscard]] std::uint64_t fifo_error_count() const noexcept {
    return fifo_error_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t sequence_error_count() const noexcept {
    return sequence_error_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::uint64_t record_validation_error_count() const noexcept {
    return record_validation_error_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] std::size_t maximum_publication_actions_observed() const noexcept {
    std::size_t maximum = 0;
    for (std::size_t index = 0; index < producer_count_; ++index) {
      maximum = std::max(maximum, producer_counters_[index].maximum_publication_actions);
    }
    return maximum;
  }

  [[nodiscard]] static constexpr std::size_t publication_action_limit() noexcept {
    return Topology::MaximumPublicationActions();
  }

 private:
  static constexpr std::size_t kCacheLineBytes = 64;

  struct alignas(kCacheLineBytes) ProducerCounters final {
    std::uint64_t attempted_records{0};
    std::uint64_t accepted_records{0};
    std::uint64_t rejected_records{0};
    std::uint64_t accepted_serialized_bytes{0};
    std::size_t maximum_publication_actions{0};
    std::array<std::byte, kCacheLineBytes - 4U * sizeof(std::uint64_t) - sizeof(std::size_t)>
        cache_line_padding{};
  };

  static_assert(sizeof(ProducerCounters) == kCacheLineBytes);

  [[nodiscard]] static constexpr bool SameFootprint(const RecordFootprint& left,
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

  [[nodiscard]] std::uint64_t LogicalRetainedBytes() const noexcept {
    std::uint64_t accepted_serialized_bytes = 0;
    for (std::size_t index = 0; index < producer_count_; ++index) {
      accepted_serialized_bytes += producer_counters_[index].accepted_serialized_bytes;
    }
    const std::uint64_t dequeued_serialized_bytes =
        dequeued_serialized_bytes_.load(std::memory_order_relaxed);
    const std::uint64_t current_serialized_bytes =
        accepted_serialized_bytes >= dequeued_serialized_bytes
            ? accepted_serialized_bytes - dequeued_serialized_bytes
            : 0;
    return initial_retained_bytes_ + current_serialized_bytes;
  }

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

  void DrainPublishedRecords() noexcept {
    while (true) {
      const ConsumeResult consumed = topology_->TryConsume();
      if (consumed.status == ConsumeStatus::kEmpty) {
        return;
      }
      if (consumed.status == ConsumeStatus::kPending || !consumed.record) {
        record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
        return;
      }

      const ConsumedRecord envelope = *consumed.record;
      const std::uint64_t expected = expected_sequence_.load(std::memory_order_relaxed);
      if (envelope.admission_sequence != expected) {
        fifo_error_count_.fetch_add(1, std::memory_order_relaxed);
        sequence_error_count_.fetch_add(1, std::memory_order_relaxed);
      }
      expected_sequence_.store(expected + 1U, std::memory_order_relaxed);

      const std::size_t slot_index = envelope.record.slot_index;
      if (slot_index >= producer_count_ ||
          envelope.record.generation != slot_generations_[slot_index] ||
          !record_views_[slot_index] ||
          envelope.record.serialized_bytes !=
              record_views_[slot_index].footprint().SerializedBytes() ||
          envelope.record.accounting_charge_bytes !=
              record_views_[slot_index].footprint().accounting_charge_bytes) {
        record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      }

      if (slot_index < record_slots_.size()) {
        record_slots_[slot_index].Reset();
      }
      dequeued_serialized_bytes_.fetch_add(envelope.record.serialized_bytes,
                                           std::memory_order_relaxed);
      physical_retained_bytes_.fetch_sub(envelope.record.accounting_charge_bytes,
                                         std::memory_order_relaxed);
    }
  }

  void ResetProducerCounters() noexcept {
    for (auto& counters : producer_counters_) {
      counters.attempted_records = 0;
      counters.accepted_records = 0;
      counters.rejected_records = 0;
      counters.accepted_serialized_bytes = 0;
      counters.maximum_publication_actions = 0;
    }
  }

  void ResetMeasurementState() noexcept {
    ResetProducerCounters();
    dequeued_serialized_bytes_.store(0, std::memory_order_relaxed);
    topology_measurement_baseline_ = {};
    fifo_error_count_.store(0, std::memory_order_relaxed);
    sequence_error_count_.store(0, std::memory_order_relaxed);
    record_validation_error_count_.store(0, std::memory_order_relaxed);
  }

  bool prepared_{false};
  std::size_t producer_count_{0};
  std::uint64_t retained_limit_bytes_{0};
  std::uint64_t initial_retained_bytes_{0};
  std::uint64_t logical_retained_high_water_bytes_{0};
  std::uint64_t physical_retained_high_water_bytes_{0};
  std::optional<Topology> topology_{};
  TopologySnapshot topology_measurement_baseline_{};
  std::atomic<std::uint64_t> physical_retained_bytes_{0};
  std::atomic<std::uint64_t> dequeued_serialized_bytes_{0};
  std::atomic<std::size_t> release_arrivals_{0};
  std::atomic<std::uint64_t> expected_sequence_{0};
  std::atomic<std::uint64_t> fifo_error_count_{0};
  std::atomic<std::uint64_t> sequence_error_count_{0};
  std::atomic<std::uint64_t> record_validation_error_count_{0};
  std::array<record_storage::ContiguousRecordSlot, kMaximumConcurrentPublishers> record_slots_{};
  std::array<record_storage::RecordView, kMaximumConcurrentPublishers> record_views_{};
  std::array<std::uint64_t, kMaximumConcurrentPublishers> slot_generations_{};
  std::array<ProducerCounters, kMaximumConcurrentPublishers> producer_counters_{};
};

using BoundedRingIngressKernel = IngressKernel<BoundedMpscRing<64>>;
using ChunkedMpscIngressKernel = IngressKernel<ChunkedMpsc<64, 8>>;
using PerProducerLanesIngressKernel = IngressKernel<PerProducerLanes<64>>;

static_assert(WorkloadKernel<BoundedRingIngressKernel>);
static_assert(WorkloadKernel<ChunkedMpscIngressKernel>);
static_assert(WorkloadKernel<PerProducerLanesIngressKernel>);

}  // namespace ulog::benchmark_support::ingress
