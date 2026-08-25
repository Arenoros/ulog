#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>

#include "ingress_topology.hpp"

namespace ulog::benchmark_support::ingress {

template <std::size_t CapacityValue = 64, std::size_t ChunkSizeValue = 8>
class ChunkedMpsc final {
  static_assert(CapacityValue > 0);
  static_assert((CapacityValue & (CapacityValue - 1U)) == 0U,
                "chunked capacity must be a power of two");
  static_assert(ChunkSizeValue >= 2,
                "chunk size must be at least two for reusable sequence-tagged cells");
  static_assert((ChunkSizeValue & (ChunkSizeValue - 1U)) == 0U,
                "chunk size must be a power of two");
  static_assert(ChunkSizeValue <= CapacityValue);
  static_assert(CapacityValue % ChunkSizeValue == 0);
  static_assert(CapacityValue / ChunkSizeValue <= kMaximumConcurrentPublishers,
                "every storage chunk must have at least one valid producer mapping");
  static_assert(CapacityValue <= std::numeric_limits<std::uint64_t>::max());

 public:
  [[nodiscard]] static constexpr std::string_view Name() noexcept { return "chunked-mpsc"; }

  ChunkedMpsc() noexcept {
    for (auto& chunk : chunks_) {
      for (std::size_t cell_index = 0; cell_index < ChunkSizeValue; ++cell_index) {
        chunk.cells[cell_index].turn.store(cell_index, std::memory_order_relaxed);
      }
    }
  }

  ChunkedMpsc(const ChunkedMpsc&) = delete;
  ChunkedMpsc& operator=(const ChunkedMpsc&) = delete;
  ChunkedMpsc(ChunkedMpsc&&) = delete;
  ChunkedMpsc& operator=(ChunkedMpsc&&) = delete;

  [[nodiscard]] static constexpr std::size_t Capacity() noexcept { return CapacityValue; }

  [[nodiscard]] static constexpr std::size_t StorageChunkSize() noexcept { return ChunkSizeValue; }

  [[nodiscard]] static constexpr std::size_t StorageChunkCount() noexcept {
    return CapacityValue / ChunkSizeValue;
  }

  [[nodiscard]] static constexpr std::size_t ChunkIndexForProducer(
      std::size_t producer_index) noexcept {
    return producer_index % StorageChunkCount();
  }

  [[nodiscard]] static constexpr std::size_t MaximumPublicationActions() noexcept {
    // producer shard + guard + tail + turn + tail advance + sequence + 3 shard totals + ready +
    // unlock
    return 11U;
  }

  [[nodiscard]] PublishResult TryPublish(std::size_t producer_index, RecordHandle record) noexcept {
    std::size_t actions = 0;
    if (producer_index >= kMaximumConcurrentPublishers) {
      invalid_producer_counters_.attempted_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.rejected_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 3;
      return {.status = PublishStatus::kInvalid, .publication_actions = actions};
    }

    ProducerCounters& producer_counters = producer_counters_[producer_index];
    producer_counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
    ++actions;
    if (!IsValid(record)) {
      producer_counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      producer_counters.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return {.status = PublishStatus::kInvalid, .publication_actions = actions};
    }

    Chunk& chunk = chunks_[ChunkIndexForProducer(producer_index)];
    // The guard stays held through global admission and readiness so physical chunk-head order
    // always matches global sequence order. Contenders reject instead of waiting.
    bool expected_unlocked = false;
    ++actions;
    if (!chunk.publication_guard.value.compare_exchange_strong(
            expected_unlocked, true, std::memory_order_acquire, std::memory_order_relaxed)) {
      producer_counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      producer_counters.contention_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return {.status = PublishStatus::kContended, .publication_actions = actions};
    }

    const std::uint64_t local_position = chunk.enqueue_position.value;
    ++actions;
    Cell& reserved_cell = CellAt(chunk, local_position);
    const std::uint64_t turn = reserved_cell.turn.load(std::memory_order_acquire);
    ++actions;
    if (turn != local_position) {
      producer_counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      producer_counters.full_rejections.fetch_add(1, std::memory_order_relaxed);
      chunk.publication_guard.value.store(false, std::memory_order_release);
      actions += 3;
      return {.status = PublishStatus::kFull, .publication_actions = actions};
    }
    chunk.enqueue_position.value = local_position + 1U;
    ++actions;

    // This is the sole global admission and sequence linearization point. Local reservation has
    // already succeeded, so all publication steps after this fetch-add are infallible.
    const std::uint64_t admission_sequence =
        next_admission_sequence_.value.fetch_add(1, std::memory_order_relaxed);
    ++actions;
    reserved_cell.envelope =
        ConsumedRecord{.record = record, .admission_sequence = admission_sequence};
    producer_counters.enqueued_records.fetch_add(1, std::memory_order_relaxed);
    producer_counters.enqueued_serialized_bytes.fetch_add(record.serialized_bytes,
                                                          std::memory_order_relaxed);
    producer_counters.enqueued_charge_bytes.fetch_add(record.accounting_charge_bytes,
                                                      std::memory_order_relaxed);
    reserved_cell.turn.store(local_position + 1U, std::memory_order_release);
    chunk.publication_guard.value.store(false, std::memory_order_release);
    actions += 5;
    return {
        .status = PublishStatus::kAccepted,
        .admission_sequence = admission_sequence,
        .publication_actions = actions,
    };
  }

  [[nodiscard]] ConsumeResult TryConsume() noexcept {
    const std::uint64_t expected_sequence = dequeue_sequence_.value;
    Chunk* selected_chunk = nullptr;
    Cell* selected_cell = nullptr;
    std::uint64_t selected_local_position = 0;

    for (auto& chunk : chunks_) {
      const std::uint64_t local_position = chunk.dequeue_position.value;
      Cell& cell = CellAt(chunk, local_position);
      const std::uint64_t turn = cell.turn.load(std::memory_order_acquire);
      if (turn == local_position + 1U && cell.envelope.admission_sequence == expected_sequence) {
        selected_chunk = &chunk;
        selected_cell = &cell;
        selected_local_position = local_position;
        break;
      }
    }

    if (selected_cell == nullptr) {
      const bool earlier_sequence_is_pending =
          next_admission_sequence_.value.load(std::memory_order_relaxed) > expected_sequence;
      return {.status =
                  earlier_sequence_is_pending ? ConsumeStatus::kPending : ConsumeStatus::kEmpty};
    }

    const ConsumedRecord consumed = selected_cell->envelope;
    consumer_counters_.dequeued_records.fetch_add(1, std::memory_order_relaxed);
    consumer_counters_.dequeued_serialized_bytes.fetch_add(consumed.record.serialized_bytes,
                                                           std::memory_order_relaxed);
    consumer_counters_.dequeued_charge_bytes.fetch_add(consumed.record.accounting_charge_bytes,
                                                       std::memory_order_relaxed);
    selected_cell->turn.store(selected_local_position + ChunkSizeValue, std::memory_order_release);
    selected_chunk->dequeue_position.value = selected_local_position + 1U;
    dequeue_sequence_.value = expected_sequence + 1U;
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
    for (const auto& counters : producer_counters_) {
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
  static constexpr std::size_t kCacheLineSize = 64;

  struct alignas(kCacheLineSize) Cell final {
    std::atomic<std::uint64_t> turn{0};
    ConsumedRecord envelope{};
    std::array<std::byte,
               kCacheLineSize - sizeof(std::atomic<std::uint64_t>) - sizeof(ConsumedRecord)>
        cache_line_padding{};
  };

  struct alignas(kCacheLineSize) AtomicPosition final {
    std::atomic<std::uint64_t> value{0};
    std::array<std::byte, kCacheLineSize - sizeof(std::atomic<std::uint64_t>)> cache_line_padding{};
  };

  struct alignas(kCacheLineSize) PublicationGuard final {
    std::atomic<bool> value{false};
    std::array<std::byte, kCacheLineSize - sizeof(std::atomic<bool>)> cache_line_padding{};
  };

  struct alignas(kCacheLineSize) PaddedPosition final {
    std::uint64_t value{0};
    std::array<std::byte, kCacheLineSize - sizeof(std::uint64_t)> cache_line_padding{};
  };

  struct alignas(kCacheLineSize) Chunk final {
    std::array<Cell, ChunkSizeValue> cells{};
    PublicationGuard publication_guard{};
    PaddedPosition enqueue_position{};
    PaddedPosition dequeue_position{};
  };

  struct alignas(kCacheLineSize) ProducerCounters final {
    std::atomic<std::uint64_t> attempted_records{0};
    std::atomic<std::uint64_t> enqueued_records{0};
    std::atomic<std::uint64_t> rejected_records{0};
    std::atomic<std::uint64_t> full_rejections{0};
    std::atomic<std::uint64_t> contention_rejections{0};
    std::atomic<std::uint64_t> invalid_rejections{0};
    std::atomic<std::uint64_t> enqueued_serialized_bytes{0};
    std::atomic<std::uint64_t> enqueued_charge_bytes{0};
  };

  struct alignas(kCacheLineSize) ConsumerCounters final {
    std::atomic<std::uint64_t> dequeued_records{0};
    std::atomic<std::uint64_t> dequeued_serialized_bytes{0};
    std::atomic<std::uint64_t> dequeued_charge_bytes{0};
    std::array<std::byte, kCacheLineSize - 3U * sizeof(std::atomic<std::uint64_t>)>
        cache_line_padding{};
  };

  struct alignas(kCacheLineSize) InvalidProducerCounters final {
    std::atomic<std::uint64_t> attempted_records{0};
    std::atomic<std::uint64_t> rejected_records{0};
    std::atomic<std::uint64_t> invalid_rejections{0};
    std::array<std::byte, kCacheLineSize - 3U * sizeof(std::atomic<std::uint64_t>)>
        cache_line_padding{};
  };

  static_assert(sizeof(Cell) == kCacheLineSize);
  static_assert(sizeof(AtomicPosition) == kCacheLineSize);
  static_assert(sizeof(PublicationGuard) == kCacheLineSize);
  static_assert(sizeof(PaddedPosition) == kCacheLineSize);
  static_assert(sizeof(Chunk) == (ChunkSizeValue + 3U) * kCacheLineSize);
  static_assert(sizeof(ProducerCounters) == kCacheLineSize);
  static_assert(sizeof(ConsumerCounters) == kCacheLineSize);
  static_assert(sizeof(InvalidProducerCounters) == kCacheLineSize);

  [[nodiscard]] static Cell& CellAt(Chunk& chunk, std::uint64_t local_position) noexcept {
    return chunk.cells[static_cast<std::size_t>(local_position & (ChunkSizeValue - 1U))];
  }

  [[nodiscard]] static constexpr bool IsValid(const RecordHandle& record) noexcept {
    constexpr std::uint64_t kMaximumPerRecord =
        std::numeric_limits<std::uint64_t>::max() / CapacityValue;
    return record.serialized_bytes > 0 &&
           record.accounting_charge_bytes >= record.serialized_bytes &&
           record.serialized_bytes <= kMaximumPerRecord &&
           record.accounting_charge_bytes <= kMaximumPerRecord;
  }

  [[nodiscard]] static constexpr std::uint64_t SaturatingDifference(
      std::uint64_t minuend, std::uint64_t subtrahend) noexcept {
    return minuend >= subtrahend ? minuend - subtrahend : 0;
  }

  std::array<Chunk, StorageChunkCount()> chunks_{};
  AtomicPosition next_admission_sequence_{};
  PaddedPosition dequeue_sequence_{};
  std::array<ProducerCounters, kMaximumConcurrentPublishers> producer_counters_{};
  ConsumerCounters consumer_counters_{};
  InvalidProducerCounters invalid_producer_counters_{};
};

}  // namespace ulog::benchmark_support::ingress
