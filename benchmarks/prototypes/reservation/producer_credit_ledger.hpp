#pragma once

#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace ulog::benchmark_support::reservation {

// The ledger outlives its tokens, and one producer thread owns each configured producer index.
class ProducerCreditLedger final {
 public:
  static constexpr std::size_t kMaxProducerCount = 32;
  static constexpr std::size_t kAccountingQuantumBytes = 64;

  struct Snapshot final {
    std::size_t capacity_bytes;
    std::size_t baseline_bytes;
    std::size_t producer_count;
    std::size_t central_available_bytes;
    std::size_t cached_credit_bytes;
    std::array<std::size_t, kMaxProducerCount> local_credit_bytes;
    std::array<std::size_t, kMaxProducerCount> returned_credit_bytes;
    std::size_t active_charge_bytes;
    std::size_t logical_retained_bytes;
    std::size_t physical_retained_bytes;
  };

  class Ownership;

  class Reservation final {
   public:
    Reservation() noexcept = default;
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;
    Reservation(Reservation&& other) noexcept;
    Reservation& operator=(Reservation&& other) noexcept;
    ~Reservation();

    [[nodiscard]] explicit operator bool() const noexcept { return ledger_ != nullptr; }
    [[nodiscard]] std::size_t producer_index() const noexcept { return producer_index_; }
    [[nodiscard]] std::size_t requested_bytes() const noexcept { return requested_bytes_; }
    [[nodiscard]] std::size_t charge_bytes() const noexcept { return charge_bytes_; }

    [[nodiscard]] Ownership Commit(std::size_t retained_bytes) &&;
    void Abandon() noexcept;

   private:
    friend class ProducerCreditLedger;

    Reservation(ProducerCreditLedger& ledger, std::size_t producer_index,
                std::size_t requested_bytes, std::size_t charge_bytes) noexcept;

    ProducerCreditLedger* ledger_{nullptr};
    std::size_t producer_index_{0};
    std::size_t requested_bytes_{0};
    std::size_t charge_bytes_{0};
  };

  class Ownership final {
   public:
    Ownership() noexcept = default;
    Ownership(const Ownership&) = delete;
    Ownership& operator=(const Ownership&) = delete;
    Ownership(Ownership&& other) noexcept;
    Ownership& operator=(Ownership&& other) noexcept;
    ~Ownership();

    [[nodiscard]] explicit operator bool() const noexcept { return ledger_ != nullptr; }
    [[nodiscard]] std::size_t producer_index() const noexcept { return producer_index_; }
    [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }
    [[nodiscard]] std::size_t charge_bytes() const noexcept { return charge_bytes_; }

    void Release() noexcept;

   private:
    friend class ProducerCreditLedger;

    Ownership(ProducerCreditLedger& ledger, std::size_t producer_index, std::size_t retained_bytes,
              std::size_t charge_bytes) noexcept;

    ProducerCreditLedger* ledger_{nullptr};
    std::size_t producer_index_{0};
    std::size_t retained_bytes_{0};
    std::size_t charge_bytes_{0};
  };

  ProducerCreditLedger() noexcept = default;
  ProducerCreditLedger(const ProducerCreditLedger&) = delete;
  ProducerCreditLedger& operator=(const ProducerCreditLedger&) = delete;
  ProducerCreditLedger(ProducerCreditLedger&&) = delete;
  ProducerCreditLedger& operator=(ProducerCreditLedger&&) = delete;

  // Call quiescently, while no producer or ownership transition can run.
  void Reset(std::size_t capacity_bytes, std::size_t baseline_bytes, std::size_t producer_count);

  [[nodiscard]] Reservation TryReserve(std::size_t producer_index,
                                       std::size_t worst_case_bytes) noexcept;

  template <typename Builder>
    requires std::invocable<Builder&&, std::size_t> &&
             std::same_as<std::remove_cvref_t<std::invoke_result_t<Builder&&, std::size_t>>,
                          std::size_t>
  [[nodiscard]] Ownership TryBuild(std::size_t producer_index, std::size_t worst_case_bytes,
                                   Builder&& builder) {
    auto reservation = TryReserve(producer_index, worst_case_bytes);
    if (!reservation) {
      return {};
    }

    const std::size_t retained_bytes =
        std::invoke(std::forward<Builder>(builder), reservation.requested_bytes());
    return std::move(reservation).Commit(retained_bytes);
  }

  // These owner-thread transfers expose the exact credit state to the randomized oracle.
  [[nodiscard]] bool TryRefillCredit(std::size_t producer_index, std::size_t bytes) noexcept;
  [[nodiscard]] bool ReturnCredit(std::size_t producer_index, std::size_t bytes) noexcept;

  // Call quiescently, while no producer or ownership transition can run.
  void ReturnAllCredits();
  // Call quiescently, while no producer or ownership transition can run.
  [[nodiscard]] Snapshot GetSnapshot() const noexcept;

 private:
  static constexpr std::size_t kProducerSlotAlignment = 64;

  struct alignas(kProducerSlotAlignment) ProducerSlot final {
    std::size_t local_credit_bytes{0};
    std::atomic<std::size_t> returned_credit_bytes{0};
    std::atomic<std::size_t> logical_charge_bytes{0};
    std::array<std::byte,
               kProducerSlotAlignment - sizeof(std::size_t) - 2U * sizeof(std::atomic<std::size_t>)>
        cache_line_padding{};
  };

  static_assert(sizeof(ProducerSlot) == kProducerSlotAlignment);

  [[nodiscard]] static bool TryChargeFor(std::size_t bytes, std::size_t& charge_bytes) noexcept;
  [[nodiscard]] bool IsValidProducer(std::size_t producer_index) const noexcept;
  void DrainReturnedCredit(std::size_t producer_index) noexcept;
  void ReturnToMailbox(std::size_t producer_index, std::size_t bytes) noexcept;
  [[nodiscard]] Ownership CommitReservation(Reservation& reservation, std::size_t retained_bytes);
  void AbandonReservation(Reservation& reservation) noexcept;
  void ReleaseOwnership(Ownership& ownership) noexcept;

  std::array<ProducerSlot, kMaxProducerCount> producer_slots_{};
  std::size_t capacity_bytes_{0};
  std::size_t baseline_bytes_{0};
  std::size_t producer_count_{0};
  std::atomic<std::size_t> central_available_bytes_{0};
};

}  // namespace ulog::benchmark_support::reservation
