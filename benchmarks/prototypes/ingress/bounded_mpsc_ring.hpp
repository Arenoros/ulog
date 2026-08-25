#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ingress_topology.hpp"

namespace ulog::benchmark_support::ingress {

template <std::size_t Capacity>
class BoundedMpscRing final {
  static_assert(Capacity > 0);
  static_assert((Capacity & (Capacity - 1U)) == 0U, "ring capacity must be a power of two");
  static_assert(Capacity <= std::numeric_limits<std::uint64_t>::max());

 public:
  [[nodiscard]] static constexpr std::string_view Name() noexcept { return "bounded-mpsc-ring"; }

  BoundedMpscRing() noexcept {
    for (std::size_t index = 0; index < Capacity; ++index) {
      cells_[index].turn.store(index, std::memory_order_relaxed);
    }
  }

  BoundedMpscRing(const BoundedMpscRing&) = delete;
  BoundedMpscRing& operator=(const BoundedMpscRing&) = delete;
  BoundedMpscRing(BoundedMpscRing&&) = delete;
  BoundedMpscRing& operator=(BoundedMpscRing&&) = delete;

  [[nodiscard]] static constexpr std::size_t MaximumPublicationActions() noexcept {
    return 2U * kMaximumConcurrentPublishers + 6U;
  }

  [[nodiscard]] PublishResult TryPublish(std::size_t producer_index, RecordHandle record) noexcept {
    std::size_t actions = 0;
    if (producer_index >= kMaximumConcurrentPublishers) {
      invalid_producer_counters_.attempted_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.rejected_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      return {.status = PublishStatus::kInvalid, .publication_actions = 3};
    }

    ProducerCounters& counters = producer_counters_[producer_index];
    counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
    ++actions;

    if (!IsValid(record)) {
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return {.status = PublishStatus::kInvalid, .publication_actions = actions};
    }

    std::uint64_t position = enqueue_position_.value.load(std::memory_order_relaxed);
    ++actions;
    Cell* reserved_cell = nullptr;
    for (std::size_t probe = 0; probe < kMaximumConcurrentPublishers; ++probe) {
      Cell& cell = cells_[Index(position)];
      const std::uint64_t turn = cell.turn.load(std::memory_order_acquire);
      ++actions;
      if (turn == position) {
        const std::uint64_t next_position = position + 1U;
        ++actions;
        if (enqueue_position_.value.compare_exchange_strong(position, next_position,
                                                            std::memory_order_relaxed)) {
          reserved_cell = &cell;
          break;
        }
        continue;
      }
      if (turn < position) {
        counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
        counters.full_rejections.fetch_add(1, std::memory_order_relaxed);
        actions += 2;
        return {.status = PublishStatus::kFull, .publication_actions = actions};
      }
      position = enqueue_position_.value.load(std::memory_order_relaxed);
      ++actions;
    }

    if (reserved_cell == nullptr) {
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.contention_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return {.status = PublishStatus::kContended, .publication_actions = actions};
    }

    reserved_cell->envelope = ConsumedRecord{.record = record, .admission_sequence = position};
    counters.enqueued_records.fetch_add(1, std::memory_order_relaxed);
    counters.enqueued_serialized_bytes.fetch_add(record.serialized_bytes,
                                                 std::memory_order_relaxed);
    counters.enqueued_charge_bytes.fetch_add(record.accounting_charge_bytes,
                                             std::memory_order_relaxed);
    reserved_cell->turn.store(position + 1U, std::memory_order_release);
    actions += 4;
    return {
        .status = PublishStatus::kAccepted,
        .admission_sequence = position,
        .publication_actions = actions,
    };
  }

  [[nodiscard]] ConsumeResult TryConsume() noexcept {
    const std::uint64_t position = dequeue_position_.value.load(std::memory_order_relaxed);
    Cell& cell = cells_[Index(position)];
    const std::uint64_t turn = cell.turn.load(std::memory_order_acquire);
    if (turn != position + 1U) {
      const bool publication_reserved =
          enqueue_position_.value.load(std::memory_order_acquire) > position;
      return {.status = publication_reserved ? ConsumeStatus::kPending : ConsumeStatus::kEmpty};
    }

    const ConsumedRecord consumed = cell.envelope;
    consumer_counters_.dequeued_records.fetch_add(1, std::memory_order_relaxed);
    consumer_counters_.dequeued_serialized_bytes.fetch_add(consumed.record.serialized_bytes,
                                                           std::memory_order_relaxed);
    consumer_counters_.dequeued_charge_bytes.fetch_add(consumed.record.accounting_charge_bytes,
                                                       std::memory_order_relaxed);
    cell.turn.store(position + Capacity, std::memory_order_release);
    dequeue_position_.value.store(position + 1U, std::memory_order_relaxed);
    return {.status = ConsumeStatus::kRecord, .record = consumed};
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
    for (const ProducerCounters& counters : producer_counters_) {
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
    std::atomic<std::uint64_t> turn{0};
    ConsumedRecord envelope{};
    std::array<std::byte, 64U - sizeof(std::atomic<std::uint64_t>) - sizeof(ConsumedRecord)>
        cache_line_padding{};
  };

  struct alignas(64) Position final {
    std::atomic<std::uint64_t> value{0};
    std::array<std::byte, 64U - sizeof(std::atomic<std::uint64_t>)> cache_line_padding{};
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

  struct alignas(64) InvalidProducerCounters final {
    std::atomic<std::uint64_t> attempted_records{0};
    std::atomic<std::uint64_t> rejected_records{0};
    std::atomic<std::uint64_t> invalid_rejections{0};
    std::array<std::byte, 64U - 3U * sizeof(std::atomic<std::uint64_t>)> cache_line_padding{};
  };

  struct alignas(64) ConsumerCounters final {
    std::atomic<std::uint64_t> dequeued_records{0};
    std::atomic<std::uint64_t> dequeued_serialized_bytes{0};
    std::atomic<std::uint64_t> dequeued_charge_bytes{0};
    std::array<std::byte, 64U - 3U * sizeof(std::atomic<std::uint64_t>)> cache_line_padding{};
  };

  static_assert(sizeof(Cell) == 64U);
  static_assert(sizeof(Position) == 64U);
  static_assert(sizeof(ProducerCounters) == 64U);
  static_assert(sizeof(InvalidProducerCounters) == 64U);
  static_assert(sizeof(ConsumerCounters) == 64U);

  [[nodiscard]] static constexpr std::size_t Index(std::uint64_t position) noexcept {
    return static_cast<std::size_t>(position & (Capacity - 1U));
  }

  [[nodiscard]] static constexpr bool IsValid(const RecordHandle& record) noexcept {
    constexpr std::uint64_t kMaximumPerRecord =
        std::numeric_limits<std::uint64_t>::max() / Capacity;
    return record.serialized_bytes > 0 &&
           record.accounting_charge_bytes >= record.serialized_bytes &&
           record.serialized_bytes <= kMaximumPerRecord &&
           record.accounting_charge_bytes <= kMaximumPerRecord;
  }

  [[nodiscard]] static constexpr std::uint64_t SaturatingDifference(
      std::uint64_t minuend, std::uint64_t subtrahend) noexcept {
    return minuend >= subtrahend ? minuend - subtrahend : 0;
  }

  std::array<Cell, Capacity> cells_{};
  Position enqueue_position_{};
  Position dequeue_position_{};
  std::array<ProducerCounters, kMaximumConcurrentPublishers> producer_counters_{};
  InvalidProducerCounters invalid_producer_counters_{};
  ConsumerCounters consumer_counters_{};
};

}  // namespace ulog::benchmark_support::ingress
