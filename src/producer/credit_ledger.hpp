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
  };

  struct Snapshot final {
    std::size_t capacity_bytes{0};
    std::size_t central_available_bytes{0};
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

    [[nodiscard]] Ownership Commit(std::size_t retained_bytes) && noexcept;
    void Abandon() noexcept;

   private:
    friend class CreditLedger;
    Reservation(CreditLedger& ledger, std::size_t producer_index, std::size_t requested_bytes,
                std::size_t charge_bytes) noexcept
        : ledger_(&ledger),
          producer_index_(producer_index),
          requested_bytes_(requested_bytes),
          charge_bytes_(charge_bytes) {}

    CreditLedger* ledger_{nullptr};
    std::size_t producer_index_{0};
    std::size_t requested_bytes_{0};
    std::size_t charge_bytes_{0};
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

    void Release() noexcept;

   private:
    friend class CreditLedger;
    Ownership(CreditLedger& ledger, std::size_t producer_index, std::size_t retained_bytes,
              std::size_t charge_bytes) noexcept
        : ledger_(&ledger),
          producer_index_(producer_index),
          retained_bytes_(retained_bytes),
          charge_bytes_(charge_bytes) {}

    CreditLedger* ledger_{nullptr};
    std::size_t producer_index_{0};
    std::size_t retained_bytes_{0};
    std::size_t charge_bytes_{0};
  };

  CreditLedger() noexcept = default;
  CreditLedger(const CreditLedger&) = delete;
  CreditLedger& operator=(const CreditLedger&) = delete;

  void Reset(Config config) noexcept;
  [[nodiscard]] Reservation TryReserve(std::size_t producer_index,
                                       std::size_t worst_case_bytes) noexcept;
  [[nodiscard]] bool TryReconcileProducer(std::size_t producer_index) noexcept;
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
  [[nodiscard]] Ownership CommitReservation(Reservation& reservation,
                                            std::size_t retained_bytes) noexcept;
  void AbandonReservation(Reservation& reservation) noexcept;
  void ReleaseOwnership(Ownership& ownership) noexcept;

  std::array<ProducerSlot, kMaximumProducerSlots> producer_slots_{};
  std::size_t capacity_bytes_{0};
  std::size_t producer_count_{0};
  std::atomic<std::size_t> central_available_bytes_{0};
};

}  // namespace ulog::detail::producer::credit
