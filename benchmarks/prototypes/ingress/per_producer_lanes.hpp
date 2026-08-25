#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

#include "ingress_topology.hpp"

namespace ulog::benchmark_support::ingress {

template <std::size_t TotalCapacity>
class PerProducerLanes final {
  static_assert(TotalCapacity > 0);
  static_assert(TotalCapacity <= std::numeric_limits<std::uint64_t>::max());

 public:
  [[nodiscard]] static constexpr std::string_view Name() noexcept { return "per-producer-lanes"; }

  // Claims borrow this topology. Publication claims stay on their producer lane, and consumption
  // claims stay on the single consumer.
  class PublicationClaim final {
   public:
    PublicationClaim(PublicationClaim&& other) noexcept { MoveFrom(other); }
    PublicationClaim& operator=(PublicationClaim&& other) noexcept {
      if (this != &other) {
        Abandon();
        MoveFrom(other);
      }
      return *this;
    }
    PublicationClaim(const PublicationClaim&) = delete;
    PublicationClaim& operator=(const PublicationClaim&) = delete;
    ~PublicationClaim() { Abandon(); }

    [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] PublishStatus status() const noexcept { return status_; }
    [[nodiscard]] std::optional<std::size_t> cell_index() const noexcept {
      return owner_ != nullptr ? std::optional<std::size_t>{cell_index_} : std::nullopt;
    }
    [[nodiscard]] std::size_t publication_actions() const noexcept { return publication_actions_; }

   private:
    friend class PerProducerLanes;

    PublicationClaim(PublishStatus status, std::size_t publication_actions) noexcept
        : status_(status), publication_actions_(publication_actions) {}
    PublicationClaim(PerProducerLanes& owner, std::size_t producer_index,
                     std::uint64_t write_position, std::size_t cell_index,
                     std::size_t publication_actions) noexcept
        : owner_(&owner),
          status_(PublishStatus::kAccepted),
          producer_index_(producer_index),
          write_position_(write_position),
          cell_index_(cell_index),
          publication_actions_(publication_actions) {}

    void Abandon() noexcept {
      if (owner_ != nullptr) {
        owner_->AbandonPublicationClaim(*this);
      }
    }
    void MoveFrom(PublicationClaim& other) noexcept {
      owner_ = std::exchange(other.owner_, nullptr);
      status_ = other.status_;
      producer_index_ = other.producer_index_;
      write_position_ = other.write_position_;
      cell_index_ = other.cell_index_;
      publication_actions_ = other.publication_actions_;
    }

    PerProducerLanes* owner_{nullptr};
    PublishStatus status_{PublishStatus::kInvalid};
    std::size_t producer_index_{0};
    std::uint64_t write_position_{0};
    std::size_t cell_index_{0};
    std::size_t publication_actions_{0};
  };

  class ConsumptionClaim final {
   public:
    ConsumptionClaim(ConsumptionClaim&& other) noexcept { MoveFrom(other); }
    ConsumptionClaim& operator=(ConsumptionClaim&& other) noexcept {
      if (this != &other) {
        Cancel();
        MoveFrom(other);
      }
      return *this;
    }
    ConsumptionClaim(const ConsumptionClaim&) = delete;
    ConsumptionClaim& operator=(const ConsumptionClaim&) = delete;
    ~ConsumptionClaim() { Cancel(); }

    [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }
    [[nodiscard]] ConsumeStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::optional<ConsumedRecord>& record() const noexcept { return record_; }

    void Acknowledge() noexcept {
      if (owner_ != nullptr) {
        owner_->AcknowledgeConsumptionClaim(*this);
      }
    }

   private:
    friend class PerProducerLanes;

    explicit ConsumptionClaim(ConsumeStatus status) noexcept : status_(status) {}
    ConsumptionClaim(PerProducerLanes& owner, std::size_t producer_index,
                     std::uint64_t read_position, ConsumedRecord record) noexcept
        : owner_(&owner),
          status_(ConsumeStatus::kRecord),
          producer_index_(producer_index),
          read_position_(read_position),
          record_(record) {}

    void Cancel() noexcept {
      if (owner_ != nullptr) {
        owner_->CancelConsumptionClaim(*this);
      }
    }
    void MoveFrom(ConsumptionClaim& other) noexcept {
      owner_ = std::exchange(other.owner_, nullptr);
      status_ = other.status_;
      producer_index_ = other.producer_index_;
      read_position_ = other.read_position_;
      record_ = std::exchange(other.record_, std::nullopt);
    }

    PerProducerLanes* owner_{nullptr};
    ConsumeStatus status_{ConsumeStatus::kEmpty};
    std::size_t producer_index_{0};
    std::uint64_t read_position_{0};
    std::optional<ConsumedRecord> record_{};
  };

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

  [[nodiscard]] PublicationClaim TryClaimPublication(std::size_t producer_index) noexcept {
    std::size_t actions = 1;
    if (producer_index >= active_producers_) {
      invalid_producer_counters_.attempted_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.rejected_records.fetch_add(1, std::memory_order_relaxed);
      invalid_producer_counters_.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return PublicationClaim{PublishStatus::kInvalid, actions};
    }

    Lane& lane = lanes_[producer_index];
    ProducerCounters& counters = producer_counters_[producer_index];

    ++actions;
    if (lane.publishing.test_and_set(std::memory_order_acquire)) {
      counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.contention_rejections.fetch_add(1, std::memory_order_relaxed);
      actions += 2;
      return PublicationClaim{PublishStatus::kContended, actions};
    }

    const std::uint64_t write_position = lane.write_position;
    const std::uint64_t read_position = lane.read_position.load(std::memory_order_acquire);
    actions += 2;
    if (write_position - read_position >= lane.capacity) {
      counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.full_rejections.fetch_add(1, std::memory_order_relaxed);
      lane.publishing.clear(std::memory_order_release);
      actions += 3;
      return PublicationClaim{PublishStatus::kFull, actions};
    }

    const std::size_t cell_index =
        lane.offset + static_cast<std::size_t>(write_position % lane.capacity);
    return PublicationClaim{*this, producer_index, write_position, cell_index, actions};
  }

  [[nodiscard]] PublishResult Publish(PublicationClaim&& claim, RecordHandle record) noexcept {
    if (!claim) {
      return {.status = claim.status(), .publication_actions = claim.publication_actions()};
    }
    if (claim.owner_ != this) {
      return {.status = PublishStatus::kInvalid,
              .publication_actions = claim.publication_actions()};
    }

    Lane& lane = lanes_[claim.producer_index_];
    ProducerCounters& counters = producer_counters_[claim.producer_index_];
    std::size_t actions = claim.publication_actions_;
    if (!IsValid(record)) {
      counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      lane.publishing.clear(std::memory_order_release);
      claim.owner_ = nullptr;
      actions += 3;
      return {.status = PublishStatus::kInvalid, .publication_actions = actions};
    }

    // Admission is linearized here; every following operation is infallible publication work.
    const std::uint64_t admission_sequence =
        next_admission_sequence_.value.fetch_add(1, std::memory_order_relaxed);
    Cell& cell = cells_[claim.cell_index_];
    cell.envelope = ConsumedRecord{.record = record, .admission_sequence = admission_sequence};
    counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
    counters.enqueued_records.fetch_add(1, std::memory_order_relaxed);
    counters.enqueued_serialized_bytes.fetch_add(record.serialized_bytes,
                                                 std::memory_order_relaxed);
    counters.enqueued_charge_bytes.fetch_add(record.accounting_charge_bytes,
                                             std::memory_order_relaxed);
    lane.write_position = claim.write_position_ + 1U;
    lane.published_position.store(claim.write_position_ + 1U, std::memory_order_release);
    lane.publishing.clear(std::memory_order_release);
    claim.owner_ = nullptr;
    actions += 7;
    return {
        .status = PublishStatus::kAccepted,
        .admission_sequence = admission_sequence,
        .publication_actions = actions,
    };
  }

  [[nodiscard]] PublishResult TryPublish(std::size_t producer_index, RecordHandle record) noexcept {
    if (producer_index < active_producers_ && !IsValid(record)) {
      ProducerCounters& counters = producer_counters_[producer_index];
      counters.attempted_records.fetch_add(1, std::memory_order_relaxed);
      counters.rejected_records.fetch_add(1, std::memory_order_relaxed);
      counters.invalid_rejections.fetch_add(1, std::memory_order_relaxed);
      return {.status = PublishStatus::kInvalid, .publication_actions = 3};
    }

    auto claim = TryClaimPublication(producer_index);
    if (!claim) {
      return {.status = claim.status(), .publication_actions = claim.publication_actions()};
    }
    return Publish(std::move(claim), record);
  }

  [[nodiscard]] ConsumptionClaim TryClaimConsumption() noexcept {
    if (consumer_counters_.claim_active) {
      return ConsumptionClaim{ConsumeStatus::kPending};
    }

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
      consumer_counters_.claim_active = true;
      return ConsumptionClaim{*this, producer, read_position, consumed};
    }

    const bool publication_in_progress =
        next_admission_sequence_.value.load(std::memory_order_acquire) !=
        consumer_counters_.next_consumption_sequence;
    return ConsumptionClaim{later_record_is_ready || publication_in_progress
                                ? ConsumeStatus::kPending
                                : ConsumeStatus::kEmpty};
  }

  [[nodiscard]] ConsumeResult TryConsume() noexcept {
    auto claim = TryClaimConsumption();
    ConsumeResult result{.status = claim.status(), .record = claim.record()};
    claim.Acknowledge();
    return result;
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
    bool claim_active{false};
    std::array<std::byte, 64U - 4U * sizeof(std::uint64_t) - sizeof(bool)> cache_line_padding{};
  };

  static_assert(sizeof(Cell) == 64U);
  static_assert(sizeof(ProducerCounters) == 64U);
  static_assert(sizeof(Lane) == 64U);
  static_assert(sizeof(InvalidProducerCounters) == 64U);
  static_assert(sizeof(AdmissionSequence) == 64U);
  static_assert(sizeof(ConsumerCounters) == 64U);

  void AbandonPublicationClaim(PublicationClaim& claim) noexcept {
    lanes_[claim.producer_index_].publishing.clear(std::memory_order_release);
    claim.owner_ = nullptr;
  }

  void AcknowledgeConsumptionClaim(ConsumptionClaim& claim) noexcept {
    const ConsumedRecord consumed = *claim.record_;
    consumer_counters_.dequeued_records.fetch_add(1, std::memory_order_relaxed);
    consumer_counters_.dequeued_serialized_bytes.fetch_add(consumed.record.serialized_bytes,
                                                           std::memory_order_relaxed);
    consumer_counters_.dequeued_charge_bytes.fetch_add(consumed.record.accounting_charge_bytes,
                                                       std::memory_order_relaxed);
    lanes_[claim.producer_index_].read_position.store(claim.read_position_ + 1U,
                                                      std::memory_order_release);
    ++consumer_counters_.next_consumption_sequence;
    consumer_counters_.claim_active = false;
    claim.owner_ = nullptr;
    claim.record_.reset();
  }

  void CancelConsumptionClaim(ConsumptionClaim& claim) noexcept {
    consumer_counters_.claim_active = false;
    claim.owner_ = nullptr;
    claim.record_.reset();
  }

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
