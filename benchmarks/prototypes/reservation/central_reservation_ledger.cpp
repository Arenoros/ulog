#include "prototypes/reservation/central_reservation_ledger.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <utility>

namespace ulog::benchmark_support::reservation {

CentralReservationLedger::CentralReservationLedger(std::size_t limit_bytes,
                                                   std::size_t baseline_bytes) {
  Reset(limit_bytes, baseline_bytes);
}

void CentralReservationLedger::Reset(std::size_t limit_bytes, std::size_t baseline_bytes) {
  if (baseline_bytes > limit_bytes) {
    throw std::invalid_argument(
        "Central reservation ledger baseline exceeds its limit; choose baseline_bytes less "
        "than or equal to limit_bytes.");
  }
  if (retained_bytes_.load(std::memory_order_relaxed) != baseline_bytes_) {
    throw std::logic_error(
        "Central reservation ledger cannot be reset while reservations or ownership charges "
        "remain; release all handles before calling Reset.");
  }

  limit_bytes_ = limit_bytes;
  baseline_bytes_ = baseline_bytes;
  retained_bytes_.store(baseline_bytes, std::memory_order_relaxed);
}

CentralReservationLedger::Reservation CentralReservationLedger::TryReserve(
    std::size_t worst_case_bytes) noexcept {
  if (worst_case_bytes == 0U) {
    return Reservation{};
  }

  std::size_t retained_bytes = retained_bytes_.load(std::memory_order_relaxed);
  for (;;) {
    if (retained_bytes > limit_bytes_ || worst_case_bytes > limit_bytes_ - retained_bytes) {
      return Reservation{};
    }

    const std::size_t retained_after_reservation = retained_bytes + worst_case_bytes;
    if (retained_bytes_.compare_exchange_weak(retained_bytes, retained_after_reservation,
                                              std::memory_order_relaxed)) {
      return Reservation{*this, worst_case_bytes};
    }
  }
}

CentralReservationLedger::Snapshot CentralReservationLedger::GetSnapshot() const noexcept {
  return Snapshot{
      .current_bytes = retained_bytes_.load(std::memory_order_relaxed),
      .limit_bytes = limit_bytes_,
  };
}

void CentralReservationLedger::FailInvariant() noexcept { std::terminate(); }

void CentralReservationLedger::Release(std::size_t bytes) noexcept {
  if (bytes == 0U) {
    return;
  }

  std::size_t retained_bytes = retained_bytes_.load(std::memory_order_relaxed);
  for (;;) {
    if (retained_bytes < baseline_bytes_ || bytes > retained_bytes - baseline_bytes_) {
      FailInvariant();
    }
    const std::size_t retained_after_release = retained_bytes - bytes;
    if (retained_bytes_.compare_exchange_weak(retained_bytes, retained_after_release,
                                              std::memory_order_relaxed)) {
      return;
    }
  }
}

CentralReservationLedger::Reservation::Reservation(Reservation&& other) noexcept
    : ledger_(std::exchange(other.ledger_, nullptr)),
      reserved_bytes_(std::exchange(other.reserved_bytes_, 0U)) {}

CentralReservationLedger::Reservation& CentralReservationLedger::Reservation::operator=(
    Reservation&& other) noexcept {
  if (this != &other) {
    Abandon();
    ledger_ = std::exchange(other.ledger_, nullptr);
    reserved_bytes_ = std::exchange(other.reserved_bytes_, 0U);
  }
  return *this;
}

CentralReservationLedger::Reservation::~Reservation() { Abandon(); }

CentralReservationLedger::Ownership CentralReservationLedger::Reservation::Commit(
    std::size_t retained_bytes) && noexcept {
  if (ledger_ == nullptr || retained_bytes > reserved_bytes_) {
    CentralReservationLedger::FailInvariant();
  }

  CentralReservationLedger* const ledger = std::exchange(ledger_, nullptr);
  const std::size_t reserved_bytes = std::exchange(reserved_bytes_, 0U);
  ledger->Release(reserved_bytes - retained_bytes);
  if (retained_bytes == 0U) {
    return Ownership{};
  }
  return Ownership{*ledger, retained_bytes};
}

void CentralReservationLedger::Reservation::Abandon() noexcept {
  CentralReservationLedger* const ledger = std::exchange(ledger_, nullptr);
  const std::size_t reserved_bytes = std::exchange(reserved_bytes_, 0U);
  if (ledger != nullptr) {
    ledger->Release(reserved_bytes);
  }
}

CentralReservationLedger::Ownership::Ownership(Ownership&& other) noexcept
    : ledger_(std::exchange(other.ledger_, nullptr)),
      retained_bytes_(std::exchange(other.retained_bytes_, 0U)) {}

CentralReservationLedger::Ownership& CentralReservationLedger::Ownership::operator=(
    Ownership&& other) noexcept {
  if (this != &other) {
    Release();
    ledger_ = std::exchange(other.ledger_, nullptr);
    retained_bytes_ = std::exchange(other.retained_bytes_, 0U);
  }
  return *this;
}

CentralReservationLedger::Ownership::~Ownership() { Release(); }

void CentralReservationLedger::Ownership::Release() noexcept {
  CentralReservationLedger* const ledger = std::exchange(ledger_, nullptr);
  const std::size_t retained_bytes = std::exchange(retained_bytes_, 0U);
  if (ledger != nullptr) {
    ledger->Release(retained_bytes);
  }
}

}  // namespace ulog::benchmark_support::reservation
