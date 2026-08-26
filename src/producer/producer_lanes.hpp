#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::producer::ingress {

struct RecordHandle final {
  std::uint32_t slot_index{0};
  std::uint32_t producer_index{0};
  std::uint64_t generation{0};
  std::uint64_t serialized_bytes{0};
  std::uint64_t accounting_charge_bytes{0};
};

struct Envelope final {
  RecordHandle record{};
  std::uint64_t admission_sequence{0};
};

enum class ClaimStatus : std::uint8_t { kClaimed, kFull, kContended, kInvalid };

enum class ConsumptionStatus : std::uint8_t { kRecord, kEmpty, kPending };

class ProducerLanes final {
 public:
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
    [[nodiscard]] ClaimStatus status() const noexcept { return status_; }
    [[nodiscard]] std::optional<std::size_t> cell_index() const noexcept {
      return owner_ != nullptr ? std::optional<std::size_t>{cell_index_} : std::nullopt;
    }

   private:
    friend class ProducerLanes;
    explicit PublicationClaim(ClaimStatus status) noexcept : status_(status) {}
    PublicationClaim(ProducerLanes& owner, std::size_t producer_index, std::uint64_t write_position,
                     std::size_t cell_index) noexcept
        : owner_(&owner),
          status_(ClaimStatus::kClaimed),
          producer_index_(producer_index),
          write_position_(write_position),
          cell_index_(cell_index) {}

    void Abandon() noexcept {
      if (owner_ != nullptr) {
        owner_->Abandon(*this);
      }
    }
    void MoveFrom(PublicationClaim& other) noexcept {
      owner_ = std::exchange(other.owner_, nullptr);
      status_ = other.status_;
      producer_index_ = other.producer_index_;
      write_position_ = other.write_position_;
      cell_index_ = other.cell_index_;
    }

    ProducerLanes* owner_{nullptr};
    ClaimStatus status_{ClaimStatus::kInvalid};
    std::size_t producer_index_{0};
    std::uint64_t write_position_{0};
    std::size_t cell_index_{0};
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
    [[nodiscard]] ConsumptionStatus status() const noexcept { return status_; }
    [[nodiscard]] const Envelope& envelope() const noexcept { return envelope_; }
    [[nodiscard]] std::size_t producer_index() const noexcept { return producer_index_; }

    void Acknowledge() noexcept {
      if (owner_ != nullptr) {
        owner_->Acknowledge(*this);
      }
    }

   private:
    friend class ProducerLanes;
    explicit ConsumptionClaim(ConsumptionStatus status) noexcept : status_(status) {}
    ConsumptionClaim(ProducerLanes& owner, std::size_t producer_index, std::uint64_t read_position,
                     Envelope envelope) noexcept
        : owner_(&owner),
          status_(ConsumptionStatus::kRecord),
          producer_index_(producer_index),
          read_position_(read_position),
          envelope_(envelope) {}

    void Cancel() noexcept {
      if (owner_ != nullptr) {
        owner_->Cancel(*this);
      }
    }
    void MoveFrom(ConsumptionClaim& other) noexcept {
      owner_ = std::exchange(other.owner_, nullptr);
      status_ = other.status_;
      producer_index_ = other.producer_index_;
      read_position_ = other.read_position_;
      envelope_ = other.envelope_;
    }

    ProducerLanes* owner_{nullptr};
    ConsumptionStatus status_{ConsumptionStatus::kEmpty};
    std::size_t producer_index_{0};
    std::uint64_t read_position_{0};
    Envelope envelope_{};
  };

  ProducerLanes(std::size_t producer_count, std::size_t ingress_cells) noexcept
      : producer_count_(producer_count) {
    const std::size_t minimum_lane_capacity = ingress_cells / producer_count;
    const std::size_t extra_capacity_lanes = ingress_cells % producer_count;
    std::size_t next_offset = 0;
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
      auto& lane = lanes_[producer];
      lane.offset = next_offset;
      lane.capacity = minimum_lane_capacity + (producer < extra_capacity_lanes ? 1U : 0U);
      next_offset += lane.capacity;
    }
  }

  ProducerLanes(const ProducerLanes&) = delete;
  ProducerLanes& operator=(const ProducerLanes&) = delete;

  [[nodiscard]] PublicationClaim TryClaimPublication(std::size_t producer_index) noexcept {
    if (producer_index >= producer_count_) {
      return PublicationClaim{ClaimStatus::kInvalid};
    }
    auto& lane = lanes_[producer_index];
    if (lane.publishing.test_and_set(std::memory_order_acquire)) {
      return PublicationClaim{ClaimStatus::kContended};
    }
    const std::uint64_t write_position = lane.write_position;
    const std::uint64_t read_position = lane.read_position.load(std::memory_order_acquire);
    if (write_position - read_position >= lane.capacity) {
      lane.publishing.clear(std::memory_order_release);
      return PublicationClaim{ClaimStatus::kFull};
    }
    const std::size_t cell_index =
        lane.offset + static_cast<std::size_t>(write_position % lane.capacity);
    return PublicationClaim{*this, producer_index, write_position, cell_index};
  }

  [[nodiscard]] std::optional<std::uint64_t> Publish(PublicationClaim&& claim,
                                                     RecordHandle record) noexcept {
    if (!claim || claim.owner_ != this || !IsValid(record) ||
        record.slot_index != claim.cell_index_ || record.producer_index != claim.producer_index_) {
      return std::nullopt;
    }

    // Sequence allocation is the ownership-transfer linearization point.
    const std::uint64_t sequence =
        next_admission_sequence_.value.fetch_add(1, std::memory_order_relaxed);
    auto& lane = lanes_[claim.producer_index_];
    cells_[claim.cell_index_].envelope = Envelope{.record = record, .admission_sequence = sequence};
    lane.write_position = claim.write_position_ + 1U;
    lane.published_position.store(claim.write_position_ + 1U, std::memory_order_release);
    lane.publishing.clear(std::memory_order_release);
    claim.owner_ = nullptr;
    return sequence;
  }

  [[nodiscard]] ConsumptionClaim TryClaimConsumption() noexcept {
    if (consumer_.claim_active) {
      return ConsumptionClaim{ConsumptionStatus::kPending};
    }

    bool later_record_ready = false;
    for (std::size_t producer = 0; producer < producer_count_; ++producer) {
      auto& lane = lanes_[producer];
      const std::uint64_t read_position = lane.read_position.load(std::memory_order_relaxed);
      if (lane.published_position.load(std::memory_order_acquire) == read_position) {
        continue;
      }
      const auto& envelope =
          cells_[lane.offset + static_cast<std::size_t>(read_position % lane.capacity)].envelope;
      if (envelope.admission_sequence != consumer_.next_sequence) {
        later_record_ready = true;
        continue;
      }
      consumer_.claim_active = true;
      return ConsumptionClaim{*this, producer, read_position, envelope};
    }

    const bool publication_in_progress =
        next_admission_sequence_.value.load(std::memory_order_acquire) != consumer_.next_sequence;
    return ConsumptionClaim{later_record_ready || publication_in_progress
                                ? ConsumptionStatus::kPending
                                : ConsumptionStatus::kEmpty};
  }

  [[nodiscard]] bool IsProducerDrained(std::size_t producer_index) const noexcept {
    if (producer_index >= producer_count_) {
      return false;
    }
    const auto& lane = lanes_[producer_index];
    return !lane.publishing.test(std::memory_order_acquire) &&
           lane.published_position.load(std::memory_order_acquire) ==
               lane.read_position.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::uint64_t AcceptedCount() const noexcept {
    return next_admission_sequence_.value.load(std::memory_order_relaxed);
  }

 private:
  friend struct ProducerLanesTestAccess;

  struct alignas(kAccountingQuantumBytes) Cell final {
    Envelope envelope{};
    std::array<std::byte, kAccountingQuantumBytes - sizeof(Envelope)> padding{};
  };

  struct alignas(kAccountingQuantumBytes) Lane final {
    std::atomic<std::uint64_t> read_position{0};
    std::atomic<std::uint64_t> published_position{0};
    std::size_t offset{0};
    std::size_t capacity{0};
    std::uint64_t write_position{0};
    std::atomic_flag publishing = ATOMIC_FLAG_INIT;
    std::array<std::byte, kAccountingQuantumBytes - 2U * sizeof(std::atomic<std::uint64_t>) -
                              2U * sizeof(std::size_t) - sizeof(std::uint64_t) -
                              sizeof(std::atomic_flag)>
        padding{};
  };

  struct alignas(kAccountingQuantumBytes) AdmissionSequence final {
    std::atomic<std::uint64_t> value{0};
    std::array<std::byte, kAccountingQuantumBytes - sizeof(std::atomic<std::uint64_t>)> padding{};
  };

  struct alignas(kAccountingQuantumBytes) ConsumerState final {
    std::uint64_t next_sequence{0};
    bool claim_active{false};
    std::array<std::byte, kAccountingQuantumBytes - sizeof(std::uint64_t) - sizeof(bool)> padding{};
  };

  static_assert(sizeof(Cell) == kAccountingQuantumBytes);
  static_assert(sizeof(Lane) == kAccountingQuantumBytes);
  static_assert(sizeof(AdmissionSequence) == kAccountingQuantumBytes);
  static_assert(sizeof(ConsumerState) == kAccountingQuantumBytes);

  [[nodiscard]] static bool IsValid(const RecordHandle& record) noexcept {
    return record.serialized_bytes != 0U &&
           record.accounting_charge_bytes >= record.serialized_bytes &&
           record.slot_index < kMaximumIngressCells &&
           record.producer_index < kMaximumProducerSlots;
  }

  void Abandon(PublicationClaim& claim) noexcept {
    lanes_[claim.producer_index_].publishing.clear(std::memory_order_release);
    claim.owner_ = nullptr;
  }

  void Acknowledge(ConsumptionClaim& claim) noexcept {
    lanes_[claim.producer_index_].read_position.store(claim.read_position_ + 1U,
                                                      std::memory_order_release);
    ++consumer_.next_sequence;
    consumer_.claim_active = false;
    claim.owner_ = nullptr;
  }

  void Cancel(ConsumptionClaim& claim) noexcept {
    consumer_.claim_active = false;
    claim.owner_ = nullptr;
  }

  std::array<Cell, kMaximumIngressCells> cells_{};
  std::array<Lane, kMaximumProducerSlots> lanes_{};
  AdmissionSequence next_admission_sequence_{};
  ConsumerState consumer_{};
  std::size_t producer_count_{0};
};

}  // namespace ulog::detail::producer::ingress
