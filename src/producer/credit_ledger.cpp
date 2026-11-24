#include "producer/credit_ledger.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <utility>

#include "producer/record_storage.hpp"

namespace ulog::detail::producer::credit {

CreditLedger::Reservation::Reservation(Reservation&& other) noexcept
    : ledger_(std::exchange(other.ledger_, nullptr)),
      producer_index_(std::exchange(other.producer_index_, 0)),
      requested_bytes_(std::exchange(other.requested_bytes_, 0)),
      charge_bytes_(std::exchange(other.charge_bytes_, 0)),
      priority_(std::exchange(other.priority_, false)) {}

CreditLedger::Reservation& CreditLedger::Reservation::operator=(Reservation&& other) noexcept {
  if (this != &other) {
    Abandon();
    ledger_ = std::exchange(other.ledger_, nullptr);
    producer_index_ = std::exchange(other.producer_index_, 0);
    requested_bytes_ = std::exchange(other.requested_bytes_, 0);
    charge_bytes_ = std::exchange(other.charge_bytes_, 0);
    priority_ = std::exchange(other.priority_, false);
  }
  return *this;
}

CreditLedger::Reservation::~Reservation() { Abandon(); }

CreditLedger::Ownership CreditLedger::Reservation::Commit(std::size_t retained_bytes) && noexcept {
  return ledger_ != nullptr ? ledger_->CommitReservation(*this, retained_bytes) : Ownership{};
}

void CreditLedger::Reservation::Abandon() noexcept {
  if (ledger_ != nullptr) {
    ledger_->AbandonReservation(*this);
  }
}

CreditLedger::Ownership::Ownership(Ownership&& other) noexcept
    : ledger_(std::exchange(other.ledger_, nullptr)),
      producer_index_(std::exchange(other.producer_index_, 0)),
      retained_bytes_(std::exchange(other.retained_bytes_, 0)),
      charge_bytes_(std::exchange(other.charge_bytes_, 0)),
      priority_(std::exchange(other.priority_, false)) {}

CreditLedger::Ownership& CreditLedger::Ownership::operator=(Ownership&& other) noexcept {
  if (this != &other) {
    Release();
    ledger_ = std::exchange(other.ledger_, nullptr);
    producer_index_ = std::exchange(other.producer_index_, 0);
    retained_bytes_ = std::exchange(other.retained_bytes_, 0);
    charge_bytes_ = std::exchange(other.charge_bytes_, 0);
    priority_ = std::exchange(other.priority_, false);
  }
  return *this;
}

CreditLedger::Ownership::~Ownership() { Release(); }

void CreditLedger::Ownership::Release() noexcept {
  if (ledger_ != nullptr) {
    ledger_->ReleaseOwnership(*this);
  }
}

void CreditLedger::Reset(Config config) noexcept {
  for (auto& slot : producer_slots_) {
    slot.local_credit_bytes = 0;
    slot.returned_credit_bytes.store(0, std::memory_order_relaxed);
    slot.logical_charge_bytes.store(0, std::memory_order_relaxed);
  }
  capacity_bytes_ = config.capacity_bytes;
  producer_count_ = config.producer_count;
  central_available_bytes_.store(config.capacity_bytes, std::memory_order_relaxed);
  priority_capacity_bytes_ = config.priority_capacity_bytes;
  priority_available_bytes_.store(config.priority_capacity_bytes, std::memory_order_relaxed);
}

CreditLedger::Reservation CreditLedger::TryReserve(std::size_t producer_index,
                                                   std::size_t worst_case_bytes) noexcept {
  const std::size_t charge_bytes = record::AccountingCharge(worst_case_bytes);
  if (!IsValidProducer(producer_index) || charge_bytes == 0U || charge_bytes > capacity_bytes_) {
    return {};
  }

  DrainReturnedCredit(producer_index);
  auto& local_credit = producer_slots_[producer_index].local_credit_bytes;
  if (local_credit < charge_bytes) {
    const std::size_t deficit = charge_bytes - local_credit;
    std::size_t available = central_available_bytes_.load(std::memory_order_relaxed);
    bool refilled = false;
    while (available >= deficit) {
      if (central_available_bytes_.compare_exchange_weak(available, available - deficit,
                                                         std::memory_order_relaxed)) {
        local_credit += deficit;
        refilled = true;
        break;
      }
    }
    if (!refilled) {
      return {};
    }
  }

  local_credit -= charge_bytes;
  producer_slots_[producer_index].logical_charge_bytes.fetch_add(worst_case_bytes,
                                                                 std::memory_order_relaxed);
  return Reservation{*this, producer_index, worst_case_bytes, charge_bytes, false};
}

CreditLedger::Reservation CreditLedger::TryReservePriority(std::size_t producer_index,
                                                           std::size_t worst_case_bytes) noexcept {
  const std::size_t charge_bytes = record::AccountingCharge(worst_case_bytes);
  if (!IsValidProducer(producer_index) || charge_bytes == 0U ||
      charge_bytes > priority_capacity_bytes_) {
    return {};
  }
  std::size_t available = priority_available_bytes_.load(std::memory_order_relaxed);
  while (available >= charge_bytes) {
    if (priority_available_bytes_.compare_exchange_weak(available, available - charge_bytes,
                                                        std::memory_order_relaxed)) {
      producer_slots_[producer_index].logical_charge_bytes.fetch_add(worst_case_bytes,
                                                                     std::memory_order_relaxed);
      return Reservation{*this, producer_index, worst_case_bytes, charge_bytes, true};
    }
  }
  return {};
}

bool CreditLedger::TryReconcileProducer(std::size_t producer_index) noexcept {
  if (!IsValidProducer(producer_index) ||
      producer_slots_[producer_index].logical_charge_bytes.load(std::memory_order_acquire) != 0U) {
    return false;
  }
  DrainReturnedCredit(producer_index);
  auto& local_credit = producer_slots_[producer_index].local_credit_bytes;
  if (local_credit != 0U) {
    central_available_bytes_.fetch_add(local_credit, std::memory_order_relaxed);
    local_credit = 0;
  }
  return true;
}

void CreditLedger::TransferOwnership(Ownership& ownership, std::size_t producer_index) noexcept {
  if (ownership.ledger_ != this || ownership.priority_ || !IsValidProducer(producer_index)) {
    ownership.Release();
    return;
  }
  producer_slots_[ownership.producer_index_].logical_charge_bytes.fetch_sub(
      ownership.retained_bytes_, std::memory_order_relaxed);
  producer_slots_[producer_index].local_credit_bytes += ownership.charge_bytes_;
  ownership.ledger_ = nullptr;
  ownership.producer_index_ = 0;
  ownership.retained_bytes_ = 0;
  ownership.charge_bytes_ = 0;
}

std::size_t CreditLedger::EstimateAvailableCredit(std::size_t producer_index) const noexcept {
  if (!IsValidProducer(producer_index)) {
    return 0;
  }
  const auto& slot = producer_slots_[producer_index];
  return slot.local_credit_bytes + slot.returned_credit_bytes.load(std::memory_order_relaxed) +
         central_available_bytes_.load(std::memory_order_relaxed);
}

CreditLedger::Snapshot CreditLedger::GetSnapshot() const noexcept {
  std::size_t logical_retained = 0;
  for (std::size_t index = 0; index < producer_count_; ++index) {
    const auto& slot = producer_slots_[index];
    logical_retained += slot.logical_charge_bytes.load(std::memory_order_relaxed);
  }
  const std::size_t sampled_central_available =
      central_available_bytes_.load(std::memory_order_relaxed);
  const std::size_t central_available = std::min(sampled_central_available, capacity_bytes_);
  const std::size_t sampled_priority_available =
      priority_available_bytes_.load(std::memory_order_relaxed);
  const std::size_t priority_available =
      std::min(sampled_priority_available, priority_capacity_bytes_);
  const std::size_t physical_retained =
      capacity_bytes_ - central_available + priority_capacity_bytes_ - priority_available;
  return {
      .capacity_bytes = capacity_bytes_,
      .central_available_bytes = central_available,
      .priority_capacity_bytes = priority_capacity_bytes_,
      .priority_available_bytes = priority_available,
      .logical_retained_bytes = std::min(logical_retained, physical_retained),
      .physical_retained_bytes = physical_retained,
      .sample_consistent = sampled_central_available <= capacity_bytes_ &&
                           sampled_priority_available <= priority_capacity_bytes_ &&
                           logical_retained <= physical_retained,
  };
}

bool CreditLedger::IsValidProducer(std::size_t producer_index) const noexcept {
  return producer_index < producer_count_;
}

void CreditLedger::DrainReturnedCredit(std::size_t producer_index) noexcept {
  auto& slot = producer_slots_[producer_index];
  slot.local_credit_bytes += slot.returned_credit_bytes.exchange(0, std::memory_order_acquire);
}

void CreditLedger::ReturnCredit(std::size_t producer_index, std::size_t bytes,
                                bool priority) noexcept {
  if (!priority) {
    ReturnToMailbox(producer_index, bytes);
  } else if (bytes != 0U) {
    priority_available_bytes_.fetch_add(bytes, std::memory_order_relaxed);
  }
}

void CreditLedger::ReturnToMailbox(std::size_t producer_index, std::size_t bytes) noexcept {
  if (bytes != 0U) {
    producer_slots_[producer_index].returned_credit_bytes.fetch_add(bytes,
                                                                    std::memory_order_release);
  }
}

CreditLedger::Ownership CreditLedger::CommitReservation(Reservation& reservation,
                                                        std::size_t retained_bytes) noexcept {
  const std::size_t retained_charge = record::AccountingCharge(retained_bytes);
  if (retained_bytes > reservation.requested_bytes_ || retained_charge == 0U ||
      retained_charge > reservation.charge_bytes_) {
    AbandonReservation(reservation);
    return {};
  }

  ReturnCredit(reservation.producer_index_, reservation.charge_bytes_ - retained_charge,
               reservation.priority_);
  producer_slots_[reservation.producer_index_].logical_charge_bytes.fetch_sub(
      reservation.requested_bytes_ - retained_bytes, std::memory_order_relaxed);
  CreditLedger* const ledger = std::exchange(reservation.ledger_, nullptr);
  const std::size_t producer_index = std::exchange(reservation.producer_index_, 0);
  const bool priority = std::exchange(reservation.priority_, false);
  reservation.requested_bytes_ = 0;
  reservation.charge_bytes_ = 0;
  return Ownership{*ledger, producer_index, retained_bytes, retained_charge, priority};
}

void CreditLedger::AbandonReservation(Reservation& reservation) noexcept {
  producer_slots_[reservation.producer_index_].logical_charge_bytes.fetch_sub(
      reservation.requested_bytes_, std::memory_order_relaxed);
  ReturnCredit(reservation.producer_index_, reservation.charge_bytes_, reservation.priority_);
  reservation.ledger_ = nullptr;
  reservation.priority_ = false;
  reservation.producer_index_ = 0;
  reservation.requested_bytes_ = 0;
  reservation.charge_bytes_ = 0;
}

void CreditLedger::ReleaseOwnership(Ownership& ownership) noexcept {
  producer_slots_[ownership.producer_index_].logical_charge_bytes.fetch_sub(
      ownership.retained_bytes_, std::memory_order_relaxed);
  ReturnCredit(ownership.producer_index_, ownership.charge_bytes_, ownership.priority_);
  ownership.ledger_ = nullptr;
  ownership.priority_ = false;
  ownership.producer_index_ = 0;
  ownership.retained_bytes_ = 0;
  ownership.charge_bytes_ = 0;
}

}  // namespace ulog::detail::producer::credit
