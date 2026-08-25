#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ingress_topology.hpp"

namespace ulog::benchmark_support::ingress {

template <std::size_t TotalCapacity>
class PerProducerLanes final {
  static_assert(TotalCapacity > 0);
  static_assert(TotalCapacity <= std::numeric_limits<std::uint64_t>::max());

 public:
  [[nodiscard]] static constexpr std::string_view Name() noexcept { return "per-producer-lanes"; }

  explicit PerProducerLanes(std::size_t active_producers) noexcept
      : active_producers_(ValidActiveProducerCount(active_producers) ? active_producers : 0) {
    if (active_producers_ == 0) {
      return;
    }

    const std::size_t minimum_lane_capacity = TotalCapacity / active_producers_;
    const std::size_t extra_capacity_lanes = TotalCapacity % active_producers_;
    std::size_t next_offset = 0;
    for (std::size_t producer = 0; producer < active_producers_; ++producer) {
      Lane& lane = lanes_[producer];
      lane.offset = next_offset;
      lane.capacity = minimum_lane_capacity + (producer < extra_capacity_lanes ? 1U : 0U);
      next_offset += lane.capacity;
    }
  }

  PerProducerLanes(const PerProducerLanes&) = delete;
  PerProducerLanes& operator=(const PerProducerLanes&) = delete;
  PerProducerLanes(PerProducerLanes&&) = delete;
  PerProducerLanes& operator=(PerProducerLanes&&) = delete;

  [[nodiscard]] static constexpr std::size_t MaximumPublicationActions() noexcept { return 11; }

  [[nodiscard]] PublishResult TryPublish(std::size_t producer_index, RecordHandle record) noexcept {
    std::size_t actions = 1;
    if (producer_index >= active_producers_) {
      invalid_producer_counters_.attempted_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.rejected_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return {.status = PublishStatus::kInvalid, .publication_actions = actions};
    }

    Lane& lane = lanes_[producer_index];
    ProducerCounters& counters = producer_counters_[producer_index];
    counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
    if (!IsValid(record)) {
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return {.status = PublishStatus::kInvalid, .publication_actions = actions};
    }

    ++actions;
    if (lane.publishing.test_and_set(std::memory_order_acquire)) {
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.contention_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return {.status = PublishStatus::kContended, .publication_actions = actions};
    }

    const std::uint64_t write_position = lane.write_position;
    const std::uint64_t read_position = lane.read_position.load(std::memory_order_acquire);
    actions += 2;
    if (write_position - read_position >= lane.capacity) {
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.full_rejections.fetch_add(1, std::memory_order_relaxed);
      lane.publishing.clear(std::memory_order_release);
      actions += 3;
      return {.status = PublishStatus::kFull, .publication_actions = actions};
    }

    // Admission is linearized here; every following operation is infallible publication work.
    const std::uint64_t admission_sequence =
        next_admission_sequence_.value.fetch_add(1, std::memory_order_relaxed);
    Cell& cell = cells_[lane.offset + static_cast<std::size_t>(write_position % lane.capacity)];
    cell.envelope = ConsumedRecord{.record = record, .admission_sequence = admission_sequence};
    counters.enqueued_records.fetch_add(1, std::memory_order_relaxed);
    counters.enqueued_serialized_bytes.fetch_add(record.serialized_bytes,
                                                 std::memory_order_relaxed);
    counters.enqueued_charge_bytes.fetch_add(record.accounting_charge_bytes,
                                             std::memory_order_relaxed);
    lane.write_position = write_position + 1U;
    lane.published_position.store(write_position + 1U, std::memory_order_release);
    lane.publishing.clear(std::memory_order_release);
    actions += 7;
    return {
        .status = PublishStatus::kAccepted,
        .admission_sequence = admission_sequence,
        .publication_actions = actions,
    };
  }

  [[nodiscard]] ConsumeResult TryConsume() noexcept {
    bool later_record_is_ready = false;
    for (std::size_t producer = 0; producer < active_producers_; ++producer) {
      Lane& lane = lanes_[producer];
      const std::uint64_t read_position = lane.read_position.load(std::memory_order_relaxed);
      if (lane.published_position.load(std::memory_order_acquire) == read_position) {
        continue;
      }

      const Cell& cell =
          cells_[lane.offset + static_cast<std::size_t>(read_position % lane.capacity)];
      if (cell.envelope.admission_sequence != consumer_counters_.next_consumption_sequence) {
        later_record_is_ready = true;
        continue;
      }

      const ConsumedRecord consumed = cell.envelope;
      consumer_counters_.dequeued_records.fetch_add(1, std::memory_order_relaxed);
      consumer_counters_.dequeued_serialized_bytes.fetch_add(consumed.record.serialized_bytes,
                                                             std::memory_order_relaxed);
      consumer_counters_.dequeued_charge_bytes.fetch_add(consumed.record.accounting_charge_bytes,
                                                         std::memory_order_relaxed);
      lane.read_position.store(read_position + 1U, std::memory_order_release);
      ++consumer_counters_.next_consumption_sequence;
      return {.status = ConsumeStatus::kRecord, .record = consumed};
    }

    const bool publication_in_progress =
        next_admission_sequence_.value.load(std::memory_order_acquire) !=
        consumer_counters_.next_consumption_sequence;
    return {.status = later_record_is_ready || publication_in_progress ? ConsumeStatus::kPending
                                                                       : ConsumeStatus::kEmpty};
  }

  [[nodiscard]] TopologySnapshot GetSnapshot() const noexcept {
    TopologySnapshot snapshot{
        .attempted_records =
            invalid_producer_counters_.attempted_records.load(std::memory_order_relaxed),
        .rejected_records =
            invalid_producer_counters_.rejected_records.load(std::memory_order_relaxed),
        .invalid_rejections =
            invalid_producer_counters_.invalid_rejections.load(std::memory_order_relaxed),
    };
    std::uint64_t enqueued_serialized_bytes = 0;
    std::uint64_t enqueued_charge_bytes = 0;
    for (std::size_t producer = 0; producer < active_producers_; ++producer) {
      const ProducerCounters& counters = producer_counters_[producer];
      snapshot.attempted_records += counters.attempted_records.load(std::memory_order_relaxed);
      snapshot.enqueued_records += counters.enqueued_records.load(std::memory_order_relaxed);
      snapshot.rejected_records += counters.rejected_records.load(std::memory_order_relaxed);
      snapshot.full_rejections += counters.full_rejections.load(std::memory_order_relaxed);
      snapshot.contention_rejections +=
          counters.contention_rejections.load(std::memory_order_relaxed);
      snapshot.invalid_rejections += counters.invalid_rejections.load(std::memory_order_relaxed);
      enqueued_serialized_bytes +=
          counters.enqueued_serialized_bytes.load(std::memory_order_relaxed);
      enqueued_charge_bytes += counters.enqueued_charge_bytes.load(std::memory_order_relaxed);
    }

    snapshot.dequeued_records = consumer_counters_.dequeued_records.load(std::memory_order_relaxed);
    const std::uint64_t dequeued_serialized_bytes =
        consumer_counters_.dequeued_serialized_bytes.load(std::memory_order_relaxed);
    const std::uint64_t dequeued_charge_bytes =
        consumer_counters_.dequeued_charge_bytes.load(std::memory_order_relaxed);
    snapshot.retained_records =
        SaturatingDifference(snapshot.enqueued_records, snapshot.dequeued_records);
    snapshot.retained_serialized_bytes =
        SaturatingDifference(enqueued_serialized_bytes, dequeued_serialized_bytes);
    snapshot.retained_charge_bytes =
        SaturatingDifference(enqueued_charge_bytes, dequeued_charge_bytes);
    return snapshot;
  }

 private:
  struct alignas(64) Cell final {
    ConsumedRecord envelope{};
    std::array<std::byte, 64U - sizeof(ConsumedRecord)> cache_line_padding{};
  };

  struct alignas(64) ProducerCounters final {
    std::atomic<std::uint64_t> attempted_records{0};
    std::atomic<std::uint64_t> enqueued_records{0};
    std::atomic<std::uint64_t> rejected_records{0};
    std::atomic<std::uint64_t> full_rejections{0};
    std::atomic<std::uint64_t> contention_rejections{0};
    std::atomic<std::uint64_t> invalid_rejections{0};
    std::atomic<std::uint64_t> enqueued_serialized_bytes{0};
    std::atomic<std::uint64_t> enqueued_charge_bytes{0};
  };

  struct alignas(64) Lane final {
    std::atomic<std::uint64_t> read_position{0};
    std::atomic<std::uint64_t> published_position{0};
    std::size_t offset{0};
    std::size_t capacity{0};
    std::uint64_t write_position{0};
    std::atomic_flag publishing = ATOMIC_FLAG_INIT;
    std::array<std::byte, 64U - 2U * sizeof(std::atomic<std::uint64_t>) - 2U * sizeof(std::size_t) -
                              sizeof(std::uint64_t) - sizeof(std::atomic_flag)>
        cache_line_padding{};
  };

  struct alignas(64) InvalidProducerCounters final {
    std::atomic<std::uint64_t> attempted_records{0};
    std::atomic<std::uint64_t> rejected_records{0};
    std::atomic<std::uint64_t> invalid_rejections{0};
    std::array<std::byte, 64U - 3U * sizeof(std::atomic<std::uint64_t>)> cache_line_padding{};
  };

  struct alignas(64) AdmissionSequence final {
    std::atomic<std::uint64_t> value{0};
    std::array<std::byte, 64U - sizeof(std::atomic<std::uint64_t>)> cache_line_padding{};
  };

  struct alignas(64) ConsumerCounters final {
    std::atomic<std::uint64_t> dequeued_records{0};
    std::atomic<std::uint64_t> dequeued_serialized_bytes{0};
    std::atomic<std::uint64_t> dequeued_charge_bytes{0};
    std::uint64_t next_consumption_sequence{0};
    std::array<std::byte, 64U - 4U * sizeof(std::uint64_t)> cache_line_padding{};
  };

  static_assert(sizeof(Cell) == 64U);
  static_assert(sizeof(ProducerCounters) == 64U);
  static_assert(sizeof(Lane) == 64U);
  static_assert(sizeof(InvalidProducerCounters) == 64U);
  static_assert(sizeof(AdmissionSequence) == 64U);
  static_assert(sizeof(ConsumerCounters) == 64U);

  [[nodiscard]] static constexpr bool ValidActiveProducerCount(
      std::size_t active_producers) noexcept {
    return active_producers > 0 && active_producers <= kMaximumConcurrentPublishers &&
           active_producers <= TotalCapacity;
  }

  [[nodiscard]] static constexpr bool IsValid(const RecordHandle& record) noexcept {
    constexpr std::uint64_t kMaximumPerRecord =
        std::numeric_limits<std::uint64_t>::max() / TotalCapacity;
    return record.serialized_bytes > 0 &&
           record.accounting_charge_bytes >= record.serialized_bytes &&
           record.serialized_bytes <= kMaximumPerRecord &&
           record.accounting_charge_bytes <= kMaximumPerRecord;
  }

  [[nodiscard]] static constexpr std::uint64_t SaturatingDifference(
      std::uint64_t minuend, std::uint64_t subtrahend) noexcept {
    return minuend >= subtrahend ? minuend - subtrahend : 0;
  }

  std::array<Cell, TotalCapacity> cells_{};
  std::array<Lane, kMaximumConcurrentPublishers> lanes_{};
  std::array<ProducerCounters, kMaximumConcurrentPublishers> producer_counters_{};
  InvalidProducerCounters invalid_producer_counters_{};
  AdmissionSequence next_admission_sequence_{};
  ConsumerCounters consumer_counters_{};
  std::size_t active_producers_{0};
};

}  // namespace ulog::benchmark_support::ingress
