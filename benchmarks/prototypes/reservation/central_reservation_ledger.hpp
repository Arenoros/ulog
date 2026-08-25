#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <type_traits>
#include <utility>

namespace ulog::benchmark_support::reservation {

class CentralReservationLedger final {
 public:
  class Reservation;
  class Ownership;

  struct Snapshot final {
    std::size_t current_bytes;
    std::size_t limit_bytes;
  };

  CentralReservationLedger() noexcept = default;
  explicit CentralReservationLedger(std::size_t limit_bytes, std::size_t baseline_bytes = 0);

  // Call quiescently after releasing every charge-bearing handle.
  void Reset(std::size_t limit_bytes, std::size_t baseline_bytes = 0);

  [[nodiscard]] Reservation TryReserve(std::size_t worst_case_bytes) noexcept;

  // The builder runs only after admission and returns the bytes retained at commit.
  template <typename Builder>
    requires std::is_invocable_r_v<std::size_t, Builder&&, std::size_t>
  [[nodiscard]] Ownership TryBuild(std::size_t worst_case_bytes, Builder&& builder) noexcept(
      std::is_nothrow_invocable_r_v<std::size_t, Builder&&, std::size_t>);

  // Call while Reset cannot run concurrently.
  [[nodiscard]] Snapshot GetSnapshot() const noexcept;

 private:
  friend class Reservation;
  friend class Ownership;

  [[noreturn]] static void FailInvariant() noexcept;
  void Release(std::size_t bytes) noexcept;

  std::size_t limit_bytes_{0};
  std::size_t baseline_bytes_{0};
  std::atomic<std::size_t> retained_bytes_{0};
};

class CentralReservationLedger::Reservation final {
 public:
  Reservation() noexcept = default;
  Reservation(Reservation&& other) noexcept;
  Reservation& operator=(Reservation&& other) noexcept;
  Reservation(const Reservation&) = delete;
  Reservation& operator=(const Reservation&) = delete;
  ~Reservation();

  [[nodiscard]] explicit operator bool() const noexcept { return ledger_ != nullptr; }
  [[nodiscard]] std::size_t reserved_bytes() const noexcept { return reserved_bytes_; }

  [[nodiscard]] Ownership Commit(std::size_t retained_bytes) && noexcept;
  void Abandon() noexcept;

 private:
  friend class CentralReservationLedger;

  Reservation(CentralReservationLedger& ledger, std::size_t reserved_bytes) noexcept
      : ledger_(&ledger), reserved_bytes_(reserved_bytes) {}

  CentralReservationLedger* ledger_{nullptr};
  std::size_t reserved_bytes_{0};
};

class CentralReservationLedger::Ownership final {
 public:
  Ownership() noexcept = default;
  Ownership(Ownership&& other) noexcept;
  Ownership& operator=(Ownership&& other) noexcept;
  Ownership(const Ownership&) = delete;
  Ownership& operator=(const Ownership&) = delete;
  ~Ownership();

  [[nodiscard]] explicit operator bool() const noexcept { return ledger_ != nullptr; }
  [[nodiscard]] std::size_t retained_bytes() const noexcept { return retained_bytes_; }

  void Release() noexcept;

 private:
  friend class Reservation;

  Ownership(CentralReservationLedger& ledger, std::size_t retained_bytes) noexcept
      : ledger_(&ledger), retained_bytes_(retained_bytes) {}

  CentralReservationLedger* ledger_{nullptr};
  std::size_t retained_bytes_{0};
};

template <typename Builder>
  requires std::is_invocable_r_v<std::size_t, Builder&&, std::size_t>
CentralReservationLedger::Ownership
CentralReservationLedger::TryBuild(std::size_t worst_case_bytes, Builder&& builder) noexcept(
    std::is_nothrow_invocable_r_v<std::size_t, Builder&&, std::size_t>) {
  auto reservation = TryReserve(worst_case_bytes);
  if (!reservation) {
    return Ownership{};
  }

  const auto retained_bytes = static_cast<std::size_t>(
      std::invoke(std::forward<Builder>(builder), reservation.reserved_bytes()));
  return std::move(reservation).Commit(retained_bytes);
}

}  // namespace ulog::benchmark_support::reservation
