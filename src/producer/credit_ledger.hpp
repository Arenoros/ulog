#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::producer::credit {

class CreditLedger final {
 public:
  struct Config final {
    std::size_t capacity_bytes{0};
    std::size_t producer_count{0};
    /// Separate pool that only TryReservePriority draws from.
    std::size_t priority_capacity_bytes{0};
  };

  struct Snapshot final {
    std::size_t capacity_bytes{0};
    std::size_t central_available_bytes{0};
    std::size_t priority_capacity_bytes{0};
    std::size_t priority_available_bytes{0};
    /// Retained bytes of both pools.
    std::size_t logical_retained_bytes{0};
    std::size_t physical_retained_bytes{0};
    bool sample_consistent{true};
  };

  class Ownership;

  class Reservation final {
   public:
    Reservation() noexcept = default;
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    ~Reservation();

    [[nodiscard]] explicit operator bool() const noexcept { return ledger_ != nullptr; }
    [[nodiscard]] std::size_t requested_bytes() const noexcept { return requested_bytes_; }
    [[nodiscard]] std::size_t charge_bytes() const noexcept { return charge_bytes_; }
    [[nodiscard]] bool priority() const noexcept { return priority_; }

    [[nodiscard]] Ownership Commit(std::size_t retained_bytes) && noexcept;
    void Abandon() noexcept;

   private:
    friend class CreditLedger;
    Reservation(CreditLedger& ledger, std::size_t producer_index, std::size_t requested_bytes,
                std::size_t charge_bytes, bool priority) noexcept
        : ledger_(&ledger),
          producer_index_(producer_index),
          requested_bytes_(requested_bytes),
          charge_bytes_(charge_bytes),
          priority_(priority) {}

    CreditLedger* ledger_{nullptr};
    std::size_t producer_index_{0};
    std::size_t requested_bytes_{0};
    std::size_t charge_bytes_{0};
    /// Charged to the priority pool, which receives every returned byte directly.
    bool priority_{false};
  };

  class Ownership final {
   public:
    Ownership() noexcept = default;
    Ownership(Ownership&& other) noexcept;
    Ownership& operator=(Ownership&& other) noexcept;
    Ownership(const Ownership&) = delete;
    Ownership& operator=(const Ownership&) = delete;
    ~Ownership();

    [[nodiscard]] explicit operator bool() const noexcept { return ledger_ != nullptr; }
    [[nodiscard]] std::size_t producer_index() const noexcept { return producer_index_; }
    [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }
    [[nodiscard]] std::size_t charge_bytes() const noexcept { return charge_bytes_; }
    [[nodiscard]] bool priority() const noexcept { return priority_; }

    void Release() noexcept;

   private:
    friend class CreditLedger;
    Ownership(CreditLedger& ledger, std::size_t producer_index, std::size_t retained_bytes,
              std::size_t charge_bytes, bool priority) noexcept
        : ledger_(&ledger),
          producer_index_(producer_index),
          retained_bytes_(retained_bytes),
          charge_bytes_(charge_bytes),
          priority_(priority) {}

    CreditLedger* ledger_{nullptr};
    std::size_t producer_index_{0};
    std::size_t retained_bytes_{0};
    std::size_t charge_bytes_{0};
    bool priority_{false};
  };

  CreditLedger() noexcept = default;
  CreditLedger(const CreditLedger&) = delete;
  CreditLedger& operator=(const CreditLedger&) = delete;

  void Reset(Config config) noexcept;
  [[nodiscard]] Reservation TryReserve(std::size_t producer_index,
                                       std::size_t worst_case_bytes) noexcept;
  /// Reserves from the priority pool only. Callers use it after TryReserve failed, so ordinary
  /// reservations never touch the pool's shared counter.
  [[nodiscard]] Reservation TryReservePriority(std::size_t producer_index,
                                               std::size_t worst_case_bytes) noexcept;
  [[nodiscard]] bool TryReconcileProducer(std::size_t producer_index) noexcept;
  /// Ends an evicted Record's ownership and gives its charge to `producer_index` as local credit.
  /// Only that producer's own thread may call it; the Record must not hold priority bytes.
  void TransferOwnership(Ownership& ownership, std::size_t producer_index) noexcept;
  /// Credit `producer_index` could reserve now: its local and returned credit plus the shared
  /// pool. Other producers change the pool concurrently, so it is an estimate.
  [[nodiscard]] std::size_t EstimateAvailableCredit(std::size_t producer_index) const noexcept;
  /// Bytes taken from the shared payload pool, including credit producers cached locally; one
  /// relaxed load, so other producers may change it at once.
  [[nodiscard]] std::size_t SharedOccupancy() const noexcept {
    const std::size_t available = central_available_bytes_.load(std::memory_order_relaxed);
    return available < capacity_bytes_ ? capacity_bytes_ - available : 0U;
  }
  [[nodiscard]] Snapshot GetSnapshot() const noexcept;

 private:
  struct alignas(kAccountingQuantumBytes) ProducerSlot final {
    std::size_t local_credit_bytes{0};
    std::atomic<std::size_t> returned_credit_bytes{0};
    std::atomic<std::size_t> logical_charge_bytes{0};
    std::array<std::byte, kAccountingQuantumBytes - sizeof(std::size_t) -
                              2U * sizeof(std::atomic<std::size_t>)>
        padding{};
  };

  static_assert(sizeof(ProducerSlot) == kAccountingQuantumBytes);

  [[nodiscard]] bool IsValidProducer(std::size_t producer_index) const noexcept;
  void DrainReturnedCredit(std::size_t producer_index) noexcept;
  void ReturnToMailbox(std::size_t producer_index, std::size_t bytes) noexcept;
  /// Returns ordinary bytes to the producer's mailbox and priority bytes to the priority pool.
  void ReturnCredit(std::size_t producer_index, std::size_t bytes, bool priority) noexcept;
  [[nodiscard]] Ownership CommitReservation(Reservation& reservation,
                                            std::size_t retained_bytes) noexcept;
  void AbandonReservation(Reservation& reservation) noexcept;
  void ReleaseOwnership(Ownership& ownership) noexcept;

  std::array<ProducerSlot, kMaximumProducerSlots> producer_slots_{};
  std::size_t capacity_bytes_{0};
  std::size_t producer_count_{0};
  std::atomic<std::size_t> central_available_bytes_{0};
  std::size_t priority_capacity_bytes_{0};
  std::atomic<std::size_t> priority_available_bytes_{0};
};

}  // namespace ulog::detail::producer::credit
