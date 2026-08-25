#include "prototypes/reservation/producer_credit_ledger.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace ulog::benchmark_support::reservation {
namespace {

[[noreturn]] void ThrowInvalidResetConfiguration(const std::string& detail) {
  throw std::invalid_argument("Invalid producer-credit ledger configuration: " + detail +
                              " Use a positive 64-byte-aligned capacity, a 64-byte-aligned "
                              "baseline within that capacity, and between 1 and 32 producers.");
}

}  // namespace

ProducerCreditLedger::Reservation::Reservation(ProducerCreditLedger& ledger,
                                               std::size_t producer_index,
                                               std::size_t requested_bytes,
                                               std::size_t charge_bytes) noexcept
    : ledger_(&ledger),
      producer_index_(producer_index),
      requested_bytes_(requested_bytes),
      charge_bytes_(charge_bytes) {}

ProducerCreditLedger::Reservation::Reservation(Reservation&& other) noexcept
    : ledger_(std::exchange(other.ledger_, nullptr)),
      producer_index_(std::exchange(other.producer_index_, 0)),
      requested_bytes_(std::exchange(other.requested_bytes_, 0)),
      charge_bytes_(std::exchange(other.charge_bytes_, 0)) {}

ProducerCreditLedger::Reservation& ProducerCreditLedger::Reservation::operator=(
    Reservation&& other) noexcept {
  if (this != &other) {
    Abandon();
    ledger_ = std::exchange(other.ledger_, nullptr);
    producer_index_ = std::exchange(other.producer_index_, 0);
    requested_bytes_ = std::exchange(other.requested_bytes_, 0);
    charge_bytes_ = std::exchange(other.charge_bytes_, 0);
  }
  return *this;
}

ProducerCreditLedger::Reservation::~Reservation() { Abandon(); }

ProducerCreditLedger::Ownership ProducerCreditLedger::Reservation::Commit(
    std::size_t retained_bytes) && {
  if (ledger_ == nullptr) {
    throw std::logic_error(
        "Cannot commit an empty producer-credit reservation; reserve capacity first.");
  }
  return ledger_->CommitReservation(*this, retained_bytes);
}

void ProducerCreditLedger::Reservation::Abandon() noexcept {
  if (ledger_ != nullptr) {
    ledger_->AbandonReservation(*this);
  }
}

ProducerCreditLedger::Ownership::Ownership(ProducerCreditLedger& ledger, std::size_t producer_index,
                                           std::size_t retained_bytes,
                                           std::size_t charge_bytes) noexcept
    : ledger_(&ledger),
      producer_index_(producer_index),
      retained_bytes_(retained_bytes),
      charge_bytes_(charge_bytes) {}

ProducerCreditLedger::Ownership::Ownership(Ownership&& other) noexcept
    : ledger_(std::exchange(other.ledger_, nullptr)),
      producer_index_(std::exchange(other.producer_index_, 0)),
      retained_bytes_(std::exchange(other.retained_bytes_, 0)),
      charge_bytes_(std::exchange(other.charge_bytes_, 0)) {}

ProducerCreditLedger::Ownership& ProducerCreditLedger::Ownership::operator=(
    Ownership&& other) noexcept {
  if (this != &other) {
    Release();
    ledger_ = std::exchange(other.ledger_, nullptr);
    producer_index_ = std::exchange(other.producer_index_, 0);
    retained_bytes_ = std::exchange(other.retained_bytes_, 0);
    charge_bytes_ = std::exchange(other.charge_bytes_, 0);
  }
  return *this;
}

ProducerCreditLedger::Ownership::~Ownership() { Release(); }

void ProducerCreditLedger::Ownership::Release() noexcept {
  if (ledger_ != nullptr) {
    ledger_->ReleaseOwnership(*this);
  }
}

void ProducerCreditLedger::Reset(std::size_t capacity_bytes, std::size_t baseline_bytes,
                                 std::size_t producer_count) {
  if (capacity_bytes_ != 0U && GetSnapshot().active_charge_bytes != 0U) {
    throw std::logic_error(
        "Cannot reset the producer-credit ledger while Reservation or Ownership tokens are "
        "live; destroy or release every token first.");
  }
  if (capacity_bytes == 0U || capacity_bytes % kAccountingQuantumBytes != 0U) {
    ThrowInvalidResetConfiguration("capacity_bytes is zero or is not 64-byte aligned.");
  }
  if (baseline_bytes > capacity_bytes || baseline_bytes % kAccountingQuantumBytes != 0U) {
    ThrowInvalidResetConfiguration(
        "baseline_bytes exceeds capacity_bytes or is not 64-byte aligned.");
  }
  if (producer_count == 0U || producer_count > kMaxProducerCount) {
    ThrowInvalidResetConfiguration("producer_count is outside the supported range.");
  }

  for (auto& slot : producer_slots_) {
    slot.local_credit_bytes = 0;
    slot.returned_credit_bytes.store(0, std::memory_order_relaxed);
    slot.logical_charge_bytes.store(0, std::memory_order_relaxed);
  }
  capacity_bytes_ = capacity_bytes;
  baseline_bytes_ = baseline_bytes;
  producer_count_ = producer_count;
  central_available_bytes_.store(capacity_bytes - baseline_bytes, std::memory_order_relaxed);
}

ProducerCreditLedger::Reservation ProducerCreditLedger::TryReserve(
    std::size_t producer_index, std::size_t worst_case_bytes) noexcept {
  std::size_t charge_bytes = 0;
  if (!IsValidProducer(producer_index) || !TryChargeFor(worst_case_bytes, charge_bytes) ||
      charge_bytes > capacity_bytes_) {
    return {};
  }

  DrainReturnedCredit(producer_index);
  auto& local_credit = producer_slots_[producer_index].local_credit_bytes;
  if (local_credit < charge_bytes &&
      !TryRefillCredit(producer_index, charge_bytes - local_credit)) {
    return {};
  }

  local_credit -= charge_bytes;
  producer_slots_[producer_index].logical_charge_bytes.fetch_add(worst_case_bytes,
                                                                 std::memory_order_relaxed);
  return Reservation{*this, producer_index, worst_case_bytes, charge_bytes};
}

bool ProducerCreditLedger::TryRefillCredit(std::size_t producer_index, std::size_t bytes) noexcept {
  if (!IsValidProducer(producer_index) || bytes == 0U || bytes % kAccountingQuantumBytes != 0U) {
    return false;
  }

  std::size_t available = central_available_bytes_.load(std::memory_order_relaxed);
  while (available >= bytes) {
    const std::size_t remaining = available - bytes;
    if (central_available_bytes_.compare_exchange_weak(available, remaining,
                                                       std::memory_order_relaxed)) {
      producer_slots_[producer_index].local_credit_bytes += bytes;
      return true;
    }
  }
  return false;
}

bool ProducerCreditLedger::ReturnCredit(std::size_t producer_index, std::size_t bytes) noexcept {
  if (!IsValidProducer(producer_index) || bytes == 0U || bytes % kAccountingQuantumBytes != 0U) {
    return false;
  }

  DrainReturnedCredit(producer_index);
  if (producer_slots_[producer_index].local_credit_bytes < bytes) {
    return false;
  }
  producer_slots_[producer_index].local_credit_bytes -= bytes;
  central_available_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  return true;
}

void ProducerCreditLedger::ReturnAllCredits() {
  const auto snapshot = GetSnapshot();
  if (snapshot.active_charge_bytes != 0U || snapshot.logical_retained_bytes != baseline_bytes_) {
    throw std::logic_error(
        "Cannot return all producer credits while retained work is live; abandon open writers "
        "and release transferred ownership first.");
  }

  if (snapshot.central_available_bytes > capacity_bytes_ - baseline_bytes_ ||
      snapshot.cached_credit_bytes !=
          capacity_bytes_ - baseline_bytes_ - snapshot.central_available_bytes) {
    throw std::logic_error(
        "Producer-credit conservation failed during quiescent cleanup; inspect credit refill, "
        "commit, abandon, and release transitions.");
  }

  for (std::size_t index = 0; index < producer_count_; ++index) {
    auto& slot = producer_slots_[index];
    slot.local_credit_bytes = 0;
    slot.returned_credit_bytes.store(0, std::memory_order_relaxed);
  }
  central_available_bytes_.store(capacity_bytes_ - baseline_bytes_, std::memory_order_relaxed);
}

ProducerCreditLedger::Snapshot ProducerCreditLedger::GetSnapshot() const noexcept {
  std::size_t cached_credit = 0;
  std::size_t logical_charge = 0;
  std::array<std::size_t, kMaxProducerCount> local_credit{};
  std::array<std::size_t, kMaxProducerCount> returned_credit{};
  for (std::size_t index = 0; index < producer_count_; ++index) {
    const auto& slot = producer_slots_[index];
    local_credit[index] = slot.local_credit_bytes;
    returned_credit[index] = slot.returned_credit_bytes.load(std::memory_order_relaxed);
    logical_charge += slot.logical_charge_bytes.load(std::memory_order_relaxed);
    cached_credit += local_credit[index];
    cached_credit += returned_credit[index];
  }
  const std::size_t central_available = central_available_bytes_.load(std::memory_order_relaxed);
  const std::size_t physical_retained = capacity_bytes_ - central_available;
  const std::size_t retained_after_baseline = physical_retained - baseline_bytes_;
  return Snapshot{
      .capacity_bytes = capacity_bytes_,
      .baseline_bytes = baseline_bytes_,
      .producer_count = producer_count_,
      .central_available_bytes = central_available,
      .cached_credit_bytes = cached_credit,
      .local_credit_bytes = local_credit,
      .returned_credit_bytes = returned_credit,
      .active_charge_bytes = retained_after_baseline - cached_credit,
      .logical_retained_bytes = baseline_bytes_ + logical_charge,
      .physical_retained_bytes = physical_retained,
  };
}

bool ProducerCreditLedger::TryChargeFor(std::size_t bytes, std::size_t& charge_bytes) noexcept {
  const std::size_t at_least_one_quantum = std::max(bytes, kAccountingQuantumBytes);
  constexpr std::size_t kRoundingMask = kAccountingQuantumBytes - 1U;
  if (at_least_one_quantum > std::numeric_limits<std::size_t>::max() - kRoundingMask) {
    return false;
  }
  charge_bytes = (at_least_one_quantum + kRoundingMask) & ~kRoundingMask;
  return true;
}

bool ProducerCreditLedger::IsValidProducer(std::size_t producer_index) const noexcept {
  return producer_index < producer_count_;
}

void ProducerCreditLedger::DrainReturnedCredit(std::size_t producer_index) noexcept {
  auto& slot = producer_slots_[producer_index];
  slot.local_credit_bytes += slot.returned_credit_bytes.exchange(0, std::memory_order_acquire);
}

void ProducerCreditLedger::ReturnToMailbox(std::size_t producer_index, std::size_t bytes) noexcept {
  if (bytes != 0U) {
    producer_slots_[producer_index].returned_credit_bytes.fetch_add(bytes,
                                                                    std::memory_order_release);
  }
}

ProducerCreditLedger::Ownership ProducerCreditLedger::CommitReservation(
    Reservation& reservation, std::size_t retained_bytes) {
  if (retained_bytes > reservation.requested_bytes_) {
    throw std::invalid_argument(
        "Cannot commit more retained bytes than the successful worst-case reservation; "
        "truncate the writer output to its reserved capacity first.");
  }

  std::size_t retained_charge_bytes = 0;
  if (!TryChargeFor(retained_bytes, retained_charge_bytes) ||
      retained_charge_bytes > reservation.charge_bytes_) {
    throw std::logic_error(
        "Committed producer-credit charge does not fit its reservation; inspect accounting "
        "quantum rounding.");
  }
  const std::size_t refunded_charge_bytes = reservation.charge_bytes_ - retained_charge_bytes;
  if (refunded_charge_bytes != 0U) {
    ReturnToMailbox(reservation.producer_index_, refunded_charge_bytes);
  }
  if (retained_bytes < reservation.requested_bytes_) {
    producer_slots_[reservation.producer_index_].logical_charge_bytes.fetch_sub(
        reservation.requested_bytes_ - retained_bytes, std::memory_order_relaxed);
  }

  ProducerCreditLedger* const ledger = std::exchange(reservation.ledger_, nullptr);
  const std::size_t producer_index = std::exchange(reservation.producer_index_, 0);
  reservation.requested_bytes_ = 0;
  reservation.charge_bytes_ = 0;
  return Ownership{*ledger, producer_index, retained_bytes, retained_charge_bytes};
}

void ProducerCreditLedger::AbandonReservation(Reservation& reservation) noexcept {
  producer_slots_[reservation.producer_index_].logical_charge_bytes.fetch_sub(
      reservation.requested_bytes_, std::memory_order_relaxed);
  ReturnToMailbox(reservation.producer_index_, reservation.charge_bytes_);
  reservation.ledger_ = nullptr;
  reservation.producer_index_ = 0;
  reservation.requested_bytes_ = 0;
  reservation.charge_bytes_ = 0;
}

void ProducerCreditLedger::ReleaseOwnership(Ownership& ownership) noexcept {
  producer_slots_[ownership.producer_index_].logical_charge_bytes.fetch_sub(
      ownership.retained_bytes_, std::memory_order_relaxed);
  ReturnToMailbox(ownership.producer_index_, ownership.charge_bytes_);
  ownership.ledger_ = nullptr;
  ownership.producer_index_ = 0;
  ownership.retained_bytes_ = 0;
  ownership.charge_bytes_ = 0;
}

}  // namespace ulog::benchmark_support::reservation
