#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <ios>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "prototypes/reservation/central_reservation_ledger.hpp"
#include "prototypes/reservation/producer_credit_ledger.hpp"

namespace {

using ulog::benchmark_support::reservation::CentralReservationLedger;
using ulog::benchmark_support::reservation::ProducerCreditLedger;

static_assert(!std::is_copy_constructible_v<CentralReservationLedger::Reservation>);
static_assert(!std::is_copy_assignable_v<CentralReservationLedger::Reservation>);
static_assert(std::is_nothrow_move_constructible_v<CentralReservationLedger::Reservation>);
static_assert(std::is_nothrow_move_assignable_v<CentralReservationLedger::Reservation>);
static_assert(!std::is_copy_constructible_v<CentralReservationLedger::Ownership>);
static_assert(!std::is_copy_assignable_v<CentralReservationLedger::Ownership>);
static_assert(std::is_nothrow_move_constructible_v<CentralReservationLedger::Ownership>);
static_assert(std::is_nothrow_move_assignable_v<CentralReservationLedger::Ownership>);

static_assert(!std::is_copy_constructible_v<ProducerCreditLedger::Reservation>);
static_assert(!std::is_copy_assignable_v<ProducerCreditLedger::Reservation>);
static_assert(std::is_nothrow_move_constructible_v<ProducerCreditLedger::Reservation>);
static_assert(std::is_nothrow_move_assignable_v<ProducerCreditLedger::Reservation>);
static_assert(!std::is_copy_constructible_v<ProducerCreditLedger::Ownership>);
static_assert(!std::is_copy_assignable_v<ProducerCreditLedger::Ownership>);
static_assert(std::is_nothrow_move_constructible_v<ProducerCreditLedger::Ownership>);
static_assert(std::is_nothrow_move_assignable_v<ProducerCreditLedger::Ownership>);

inline constexpr std::size_t kTraceCapacityBytes = 4'096;
inline constexpr std::size_t kTraceBaselineBytes = 64;
inline constexpr std::size_t kTraceProducerCount = 4;
inline constexpr std::size_t kTraceStepCount = 4'000;
inline constexpr std::uint64_t kCentralTraceSeed = 0x5A17'4C3D'92E0'118BULL;
inline constexpr std::uint64_t kCreditTraceSeed = 0xC43D'17A5'B80E'219FULL;

bool Check(bool condition, std::string_view test, std::string_view message) {
  if (!condition) {
    std::cerr << test << ": " << message << '\n';
  }
  return condition;
}

template <typename Value>
bool CheckEqual(Value actual, Value expected, std::string_view test, std::string_view field) {
  if (actual == expected) {
    return true;
  }
  std::cerr << test << ": " << field << " is " << actual << ", expected " << expected << '\n';
  return false;
}

struct TraceLocation final {
  std::string_view candidate;
  std::uint64_t seed;
  std::size_t step;
  std::string_view operation;
};

template <typename Value>
bool CheckTraceEqual(Value actual, Value expected, const TraceLocation& location,
                     std::string_view field) {
  if (actual == expected) {
    return true;
  }
  std::cerr << location.candidate << " randomized oracle mismatch: seed=0x" << std::hex
            << location.seed << std::dec << " step=" << location.step
            << " operation=" << location.operation << " field=" << field << " actual=" << actual
            << " expected=" << expected << '\n';
  return false;
}

void PrintRecentOperations(const std::vector<std::string>& history,
                           std::string_view current_operation) {
  constexpr std::size_t kHistoryCount = 16;
  const std::size_t first = history.size() > kHistoryCount ? history.size() - kHistoryCount : 0U;
  std::cerr << "recent operations:" << '\n';
  for (std::size_t index = first; index < history.size(); ++index) {
    std::cerr << "  " << index << ": " << history[index] << '\n';
  }
  std::cerr << "  current: " << current_operation << '\n';
}

class SplitMix64 final {
 public:
  explicit SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

  [[nodiscard]] std::uint64_t Next() noexcept {
    std::uint64_t value = (state_ += 0x9E37'79B9'7F4A'7C15ULL);
    value = (value ^ (value >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D0'49BB'1331'11EBULL;
    return value ^ (value >> 31U);
  }

  [[nodiscard]] std::size_t Index(std::size_t upper_bound) noexcept {
    return static_cast<std::size_t>(Next() % static_cast<std::uint64_t>(upper_bound));
  }

 private:
  std::uint64_t state_;
};

std::size_t RandomReservationSize(SplitMix64& random) noexcept {
  constexpr std::array kEdgeSizes{
      std::size_t{0},
      std::size_t{1},
      std::size_t{63},
      std::size_t{64},
      std::size_t{65},
      std::size_t{255},
      std::size_t{256},
      std::size_t{4'095},
      std::size_t{4'096},
      std::size_t{4'097},
      std::numeric_limits<std::size_t>::max() - 63U,
      std::numeric_limits<std::size_t>::max(),
  };
  if (random.Index(4) != 0U) {
    return kEdgeSizes[random.Index(kEdgeSizes.size())];
  }
  return random.Index(kTraceCapacityBytes + 257U);
}

bool CheckCentralSnapshot(const CentralReservationLedger& ledger, std::size_t current_bytes,
                          std::size_t limit_bytes, const TraceLocation& location) {
  const auto snapshot = ledger.GetSnapshot();
  bool success = true;
  success &= CheckTraceEqual(snapshot.current_bytes, current_bytes, location, "current_bytes");
  success &= CheckTraceEqual(snapshot.limit_bytes, limit_bytes, location, "limit_bytes");
  return success;
}

struct CentralOpen final {
  std::size_t reserved_bytes;
  CentralReservationLedger::Reservation handle;
};

struct CentralOwned final {
  std::size_t retained_bytes;
  CentralReservationLedger::Ownership handle;
};

bool TestCentralTransitionsAndBuilders() {
  constexpr std::string_view kTest = "central transitions";
  CentralReservationLedger ledger{1'024, 64};

  auto reservation = ledger.TryReserve(256);
  bool success = Check(static_cast<bool>(reservation), kTest, "256-byte reserve must succeed");
  success &= CheckEqual(reservation.reserved_bytes(), std::size_t{256}, kTest, "reserved bytes");
  success &= CheckEqual(ledger.GetSnapshot().current_bytes, std::size_t{320}, kTest,
                        "retained after reserve");
  bool reset_with_live_charge_rejected = false;
  try {
    ledger.Reset(1'024, 64);
  } catch (const std::logic_error&) {
    reset_with_live_charge_rejected = true;
  }
  success &=
      Check(reset_with_live_charge_rejected, kTest, "reset must reject a live central charge");

  auto ownership = std::move(reservation).Commit(96);
  success &= Check(!reservation, kTest, "commit must consume the reservation");
  success &= Check(static_cast<bool>(ownership), kTest, "partial commit must transfer ownership");
  success &= CheckEqual(ownership.retained_bytes(), std::size_t{96}, kTest, "committed bytes");
  success &= CheckEqual(ledger.GetSnapshot().current_bytes, std::size_t{160}, kTest,
                        "partial commit must return only slack");
  ownership.Release();
  success &= CheckEqual(ledger.GetSnapshot().current_bytes, std::size_t{64}, kTest,
                        "ownership release must restore the baseline");

  auto full_reservation = ledger.TryReserve(512);
  const std::size_t retained_after_full_reserve = ledger.GetSnapshot().current_bytes;
  auto full_ownership = std::move(full_reservation).Commit(512);
  success &= CheckEqual(ledger.GetSnapshot().current_bytes, retained_after_full_reserve, kTest,
                        "full commit must not reacquire capacity");
  full_ownership.Release();

  auto zero_reservation = ledger.TryReserve(64);
  auto zero_ownership = std::move(zero_reservation).Commit(0);
  success &= Check(!zero_ownership, kTest, "zero-byte commit must return an inert token");
  ledger.Reset(1'024, 64);

  auto abandoned = ledger.TryReserve(128);
  abandoned.Abandon();
  success &= CheckEqual(ledger.GetSnapshot().current_bytes, std::size_t{64}, kTest,
                        "abandon must restore the complete reservation");

  int callback_count = 0;
  auto built = ledger.TryBuild(192, [&callback_count](std::size_t reserved_bytes) noexcept {
    ++callback_count;
    return reserved_bytes - 32U;
  });
  success &= Check(static_cast<bool>(built), kTest, "builder must commit after admission");
  success &= CheckEqual(callback_count, 1, kTest, "accepted builder callback count");
  success &= CheckEqual(ledger.GetSnapshot().current_bytes, std::size_t{224}, kTest,
                        "builder commit accounting");
  built.Release();

  ledger.Reset(256, 256);
  callback_count = 0;
  auto rejected = ledger.TryBuild(1, [&callback_count](std::size_t) noexcept {
    ++callback_count;
    return std::size_t{1};
  });
  success &= Check(!rejected, kTest, "saturated builder must be rejected");
  success &= CheckEqual(callback_count, 0, kTest, "rejected builder callback count");

  ledger.Reset(512, 64);
  bool builder_error_observed = false;
  std::atomic<bool> inject_builder_error{true};
  try {
    [[maybe_unused]] auto ignored = ledger.TryBuild(128, [&](std::size_t reserved_bytes) {
      if (inject_builder_error.load(std::memory_order_relaxed)) {
        throw std::runtime_error("injected builder failure");
      }
      return reserved_bytes;
    });
  } catch (const std::runtime_error& error) {
    builder_error_observed = std::string_view{error.what()} == "injected builder failure";
  }
  success &= Check(builder_error_observed, kTest, "builder exception must propagate unchanged");
  success &= CheckEqual(ledger.GetSnapshot().current_bytes, std::size_t{64}, kTest,
                        "throwing builder must abandon its reservation");

  success &= Check(!ledger.TryReserve(0), kTest, "zero-byte central reserve must be rejected");
  success &= Check(!ledger.TryReserve(std::numeric_limits<std::size_t>::max()), kTest,
                   "overflow-sized central reserve must be rejected");
  return success;
}

bool RunCentralRandomizedOracle() {
  CentralReservationLedger ledger{kTraceCapacityBytes, kTraceBaselineBytes};
  SplitMix64 random{kCentralTraceSeed};
  std::vector<CentralOpen> open_reservations;
  std::vector<CentralOwned> ownerships;
  std::size_t current_bytes = kTraceBaselineBytes;

  for (std::size_t step = 0; step < kTraceStepCount; ++step) {
    std::string operation;
    std::size_t action = random.Index(4);
    if (action == 1U && open_reservations.empty()) {
      action = 0;
    } else if (action == 2U && open_reservations.empty()) {
      action = ownerships.empty() ? 0U : 3U;
    } else if (action == 3U && ownerships.empty()) {
      action = open_reservations.empty() ? 0U : 2U;
    }

    if (action == 0U) {
      const std::size_t bytes = RandomReservationSize(random);
      operation = "reserve(" + std::to_string(bytes) + ")";
      auto handle = ledger.TryReserve(bytes);
      const bool expected = bytes != 0U && current_bytes <= kTraceCapacityBytes &&
                            bytes <= kTraceCapacityBytes - current_bytes;
      if (static_cast<bool>(handle) != expected) {
        const TraceLocation location{"central-reservation", kCentralTraceSeed, step, operation};
        CheckTraceEqual(static_cast<bool>(handle), expected, location, "reserve result");
        return false;
      }
      if (expected) {
        current_bytes += bytes;
        open_reservations.push_back(CentralOpen{bytes, std::move(handle)});
      }
    } else if (action == 1U) {
      const std::size_t index = random.Index(open_reservations.size());
      const std::size_t reserved_bytes = open_reservations[index].reserved_bytes;
      const std::size_t retained_bytes = random.Index(reserved_bytes + 1U);
      operation =
          "commit(" + std::to_string(reserved_bytes) + "->" + std::to_string(retained_bytes) + ")";
      auto handle = std::move(open_reservations[index].handle).Commit(retained_bytes);
      current_bytes -= reserved_bytes - retained_bytes;
      open_reservations.erase(open_reservations.begin() + static_cast<std::ptrdiff_t>(index));
      ownerships.push_back(CentralOwned{retained_bytes, std::move(handle)});
    } else if (action == 2U) {
      const std::size_t index = random.Index(open_reservations.size());
      const std::size_t reserved_bytes = open_reservations[index].reserved_bytes;
      operation = "abandon(" + std::to_string(reserved_bytes) + ")";
      open_reservations[index].handle.Abandon();
      current_bytes -= reserved_bytes;
      open_reservations.erase(open_reservations.begin() + static_cast<std::ptrdiff_t>(index));
    } else {
      const std::size_t index = random.Index(ownerships.size());
      const std::size_t retained_bytes = ownerships[index].retained_bytes;
      operation = "release(" + std::to_string(retained_bytes) + ")";
      ownerships[index].handle.Release();
      current_bytes -= retained_bytes;
      ownerships.erase(ownerships.begin() + static_cast<std::ptrdiff_t>(index));
    }

    const TraceLocation location{"central-reservation", kCentralTraceSeed, step, operation};
    if (!CheckCentralSnapshot(ledger, current_bytes, kTraceCapacityBytes, location)) {
      return false;
    }
  }

  for (auto& open : open_reservations) {
    open.handle.Abandon();
  }
  for (auto& owned : ownerships) {
    owned.handle.Release();
  }
  const TraceLocation cleanup{"central-reservation", kCentralTraceSeed, kTraceStepCount, "cleanup"};
  return CheckCentralSnapshot(ledger, kTraceBaselineBytes, kTraceCapacityBytes, cleanup);
}

struct CreditModel final {
  std::size_t capacity_bytes{kTraceCapacityBytes};
  std::size_t baseline_bytes{kTraceBaselineBytes};
  std::size_t producer_count{kTraceProducerCount};
  std::size_t central_available_bytes{kTraceCapacityBytes - kTraceBaselineBytes};
  std::array<std::size_t, ProducerCreditLedger::kMaxProducerCount> local_credit_bytes{};
  std::array<std::size_t, ProducerCreditLedger::kMaxProducerCount> returned_credit_bytes{};
  std::size_t active_charge_bytes{0};
  std::size_t logical_retained_bytes{kTraceBaselineBytes};

  [[nodiscard]] std::size_t CachedCreditBytes() const noexcept {
    std::size_t result = 0;
    for (std::size_t producer = 0; producer < producer_count; ++producer) {
      result += local_credit_bytes[producer] + returned_credit_bytes[producer];
    }
    return result;
  }

  [[nodiscard]] std::size_t PhysicalRetainedBytes() const noexcept {
    return capacity_bytes - central_available_bytes;
  }
};

std::optional<std::size_t> ChargeFor(std::size_t bytes) noexcept {
  constexpr std::size_t kQuantum = ProducerCreditLedger::kAccountingQuantumBytes;
  constexpr std::size_t kRoundingMask = kQuantum - 1U;
  const std::size_t at_least_one_quantum = std::max(bytes, kQuantum);
  if (at_least_one_quantum > std::numeric_limits<std::size_t>::max() - kRoundingMask) {
    return std::nullopt;
  }
  return (at_least_one_quantum + kRoundingMask) & ~kRoundingMask;
}

bool ModelTryRefill(CreditModel& model, std::size_t producer, std::size_t bytes) noexcept {
  constexpr std::size_t kQuantum = ProducerCreditLedger::kAccountingQuantumBytes;
  if (producer >= model.producer_count || bytes == 0U || bytes % kQuantum != 0U ||
      model.central_available_bytes < bytes) {
    return false;
  }
  model.central_available_bytes -= bytes;
  model.local_credit_bytes[producer] += bytes;
  return true;
}

bool ModelReturnCredit(CreditModel& model, std::size_t producer, std::size_t bytes) noexcept {
  constexpr std::size_t kQuantum = ProducerCreditLedger::kAccountingQuantumBytes;
  if (producer >= model.producer_count || bytes == 0U || bytes % kQuantum != 0U) {
    return false;
  }
  model.local_credit_bytes[producer] += model.returned_credit_bytes[producer];
  model.returned_credit_bytes[producer] = 0;
  if (model.local_credit_bytes[producer] < bytes) {
    return false;
  }
  model.local_credit_bytes[producer] -= bytes;
  model.central_available_bytes += bytes;
  return true;
}

bool CheckCreditSnapshot(const ProducerCreditLedger& ledger, const CreditModel& model,
                         const TraceLocation& location) {
  const auto snapshot = ledger.GetSnapshot();
  bool success = true;
  success &=
      CheckTraceEqual(snapshot.capacity_bytes, model.capacity_bytes, location, "capacity_bytes");
  success &=
      CheckTraceEqual(snapshot.baseline_bytes, model.baseline_bytes, location, "baseline_bytes");
  success &=
      CheckTraceEqual(snapshot.producer_count, model.producer_count, location, "producer_count");
  success &= CheckTraceEqual(snapshot.central_available_bytes, model.central_available_bytes,
                             location, "central_available_bytes");
  success &= CheckTraceEqual(snapshot.cached_credit_bytes, model.CachedCreditBytes(), location,
                             "cached_credit_bytes");
  for (std::size_t producer = 0; producer < model.producer_count; ++producer) {
    success &=
        CheckTraceEqual(snapshot.local_credit_bytes[producer], model.local_credit_bytes[producer],
                        location, "producer local credit");
    success &= CheckTraceEqual(snapshot.returned_credit_bytes[producer],
                               model.returned_credit_bytes[producer], location,
                               "producer returned credit");
  }
  success &= CheckTraceEqual(snapshot.active_charge_bytes, model.active_charge_bytes, location,
                             "active_charge_bytes");
  success &= CheckTraceEqual(snapshot.logical_retained_bytes, model.logical_retained_bytes,
                             location, "logical_retained_bytes");
  success &= CheckTraceEqual(snapshot.physical_retained_bytes, model.PhysicalRetainedBytes(),
                             location, "physical_retained_bytes");
  const std::size_t conserved = model.baseline_bytes + model.central_available_bytes +
                                model.CachedCreditBytes() + model.active_charge_bytes;
  success &= CheckTraceEqual(conserved, model.capacity_bytes, location, "oracle conservation");
  success &= CheckTraceEqual(snapshot.logical_retained_bytes <= snapshot.physical_retained_bytes,
                             true, location, "logical <= physical");
  return success;
}

struct CreditOpen final {
  std::size_t producer;
  std::size_t requested_bytes;
  std::size_t charge_bytes;
  ProducerCreditLedger::Reservation handle;
};

struct CreditOwned final {
  std::size_t producer;
  std::size_t retained_bytes;
  std::size_t charge_bytes;
  ProducerCreditLedger::Ownership handle;
};

bool TestProducerCreditTransitionsAndBuilders() {
  constexpr std::string_view kTest = "producer-credit transitions";
  ProducerCreditLedger ledger;
  ledger.Reset(1'024, 64, 2);
  bool success = Check(ledger.TryRefillCredit(0, 128), kTest, "explicit refill must succeed");
  success &= Check(ledger.ReturnCredit(0, 64), kTest, "explicit credit return must succeed");

  auto reservation = ledger.TryReserve(0, 65);
  success &= Check(static_cast<bool>(reservation), kTest, "65-byte reserve must succeed");
  bool reset_with_live_charge_rejected = false;
  try {
    ledger.Reset(1'024, 64, 2);
  } catch (const std::logic_error&) {
    reset_with_live_charge_rejected = true;
  }
  success &=
      Check(reset_with_live_charge_rejected, kTest, "reset must reject a live producer charge");
  success &=
      CheckEqual(reservation.charge_bytes(), std::size_t{128}, kTest, "rounded reservation charge");
  auto ownership = std::move(reservation).Commit(33);
  success &= CheckEqual(ownership.charge_bytes(), std::size_t{64}, kTest, "partial commit charge");
  auto snapshot = ledger.GetSnapshot();
  success &= CheckEqual(snapshot.logical_retained_bytes, std::size_t{97}, kTest,
                        "logical bytes after partial commit");
  success &= CheckEqual(snapshot.cached_credit_bytes, std::size_t{64}, kTest,
                        "refunded partial-commit credit");
  ownership.Release();
  snapshot = ledger.GetSnapshot();
  success &= CheckEqual(snapshot.logical_retained_bytes, std::size_t{64}, kTest,
                        "logical bytes after ownership release");
  success &= CheckEqual(snapshot.cached_credit_bytes, std::size_t{128}, kTest,
                        "released ownership mailbox credit");

  auto abandoned = ledger.TryReserve(0, 64);
  abandoned.Abandon();
  success &= Check(ledger.ReturnCredit(0, 64), kTest,
                   "drained local credit must be returnable to central");
  ledger.ReturnAllCredits();
  snapshot = ledger.GetSnapshot();
  success &= CheckEqual(snapshot.central_available_bytes, std::size_t{960}, kTest,
                        "credit cleanup central bytes");
  success &= CheckEqual(snapshot.physical_retained_bytes, std::size_t{64}, kTest,
                        "credit cleanup physical bytes");

  ledger.Reset(512, 512, 1);
  int callback_count = 0;
  auto rejected = ledger.TryBuild(0, 64, [&callback_count](std::size_t) {
    ++callback_count;
    return std::size_t{64};
  });
  success &= Check(!rejected, kTest, "saturated builder must be rejected");
  success &= CheckEqual(callback_count, 0, kTest, "rejected builder callback count");

  ledger.Reset(512, 64, 1);
  bool builder_error_observed = false;
  std::atomic<bool> inject_builder_error{true};
  try {
    [[maybe_unused]] auto ignored = ledger.TryBuild(0, 65, [&](std::size_t reserved_bytes) {
      if (inject_builder_error.load(std::memory_order_relaxed)) {
        throw std::runtime_error("injected credit builder failure");
      }
      return reserved_bytes;
    });
  } catch (const std::runtime_error& error) {
    builder_error_observed = std::string_view{error.what()} == "injected credit builder failure";
  }
  success &= Check(builder_error_observed, kTest, "builder exception must propagate unchanged");
  snapshot = ledger.GetSnapshot();
  success &= CheckEqual(snapshot.logical_retained_bytes, std::size_t{64}, kTest,
                        "throwing builder logical rollback");
  success &= CheckEqual(snapshot.active_charge_bytes, std::size_t{0}, kTest,
                        "throwing builder reservation rollback");
  ledger.ReturnAllCredits();

  auto full_reservation = ledger.TryReserve(0, 192);
  const std::size_t central_available_after_reserve = ledger.GetSnapshot().central_available_bytes;
  auto full_ownership = std::move(full_reservation).Commit(192);
  success &=
      CheckEqual(ledger.GetSnapshot().central_available_bytes, central_available_after_reserve,
                 kTest, "full commit must not refill central capacity");
  full_ownership.Release();
  ledger.ReturnAllCredits();

  success &= Check(!ledger.TryReserve(0, std::numeric_limits<std::size_t>::max()), kTest,
                   "overflow-sized credit reserve must be rejected");
  success &= Check(!ledger.TryReserve(ProducerCreditLedger::kMaxProducerCount, 64), kTest,
                   "out-of-range producer must be rejected");
  return success;
}

bool TestCrossThreadOwnershipReturn() {
  constexpr std::string_view kTest = "producer-credit cross-thread release";
  ProducerCreditLedger ledger;
  ledger.Reset(1'024, 64, 1);
  auto reservation = ledger.TryReserve(0, 128);
  auto ownership = std::move(reservation).Commit(96);
  const std::size_t central_after_reserve = ledger.GetSnapshot().central_available_bytes;

  std::thread consumer{[owned = std::move(ownership)]() mutable { owned.Release(); }};
  consumer.join();
  auto snapshot = ledger.GetSnapshot();
  bool success = CheckEqual(snapshot.logical_retained_bytes, std::size_t{64}, kTest,
                            "logical bytes after consumer release");
  success &= CheckEqual(snapshot.cached_credit_bytes, std::size_t{128}, kTest,
                        "mailbox bytes after consumer release");

  auto reused = ledger.TryReserve(0, 128);
  success &= Check(static_cast<bool>(reused), kTest, "producer must drain and reuse mailbox");
  success &= CheckEqual(ledger.GetSnapshot().central_available_bytes, central_after_reserve, kTest,
                        "mailbox reuse must not refill from central");
  reused.Abandon();
  ledger.ReturnAllCredits();
  success &= CheckEqual(ledger.GetSnapshot().physical_retained_bytes, std::size_t{64}, kTest,
                        "cross-thread test cleanup");
  return success;
}

bool RunProducerCreditRandomizedOracle() {
  ProducerCreditLedger ledger;
  ledger.Reset(kTraceCapacityBytes, kTraceBaselineBytes, kTraceProducerCount);
  CreditModel model;
  SplitMix64 random{kCreditTraceSeed};
  std::vector<CreditOpen> open_reservations;
  std::vector<CreditOwned> ownerships;
  std::vector<std::string> operation_history;
  operation_history.reserve(kTraceStepCount);

  for (std::size_t step = 0; step < kTraceStepCount; ++step) {
    std::string operation;
    std::size_t action = random.Index(6);
    if (action == 1U && open_reservations.empty()) {
      action = 0;
    } else if (action == 2U && open_reservations.empty()) {
      action = 4;
    } else if (action == 3U && ownerships.empty()) {
      action = 5;
    }

    if (action == 0U) {
      const std::size_t producer = random.Index(kTraceProducerCount + 1U);
      const std::size_t bytes = RandomReservationSize(random);
      operation = "reserve(p=" + std::to_string(producer) + ",bytes=" + std::to_string(bytes) + ")";
      auto handle = ledger.TryReserve(producer, bytes);

      const auto charge = ChargeFor(bytes);
      bool expected = false;
      if (producer < model.producer_count && charge && *charge <= model.capacity_bytes) {
        model.local_credit_bytes[producer] += model.returned_credit_bytes[producer];
        model.returned_credit_bytes[producer] = 0;
        if (model.local_credit_bytes[producer] < *charge) {
          const std::size_t deficit = *charge - model.local_credit_bytes[producer];
          static_cast<void>(ModelTryRefill(model, producer, deficit));
        }
        if (model.local_credit_bytes[producer] >= *charge) {
          expected = true;
          model.local_credit_bytes[producer] -= *charge;
          model.active_charge_bytes += *charge;
          model.logical_retained_bytes += bytes;
        }
      }
      if (static_cast<bool>(handle) != expected) {
        const TraceLocation location{"producer-credit-reservation", kCreditTraceSeed, step,
                                     operation};
        CheckTraceEqual(static_cast<bool>(handle), expected, location, "reserve result");
        PrintRecentOperations(operation_history, operation);
        return false;
      }
      if (expected) {
        open_reservations.push_back(CreditOpen{producer, bytes, *charge, std::move(handle)});
      }
    } else if (action == 1U) {
      const std::size_t index = random.Index(open_reservations.size());
      const CreditOpen& open = open_reservations[index];
      const std::size_t retained_bytes = random.Index(open.requested_bytes + 1U);
      const std::size_t retained_charge = *ChargeFor(retained_bytes);
      operation = "commit(p=" + std::to_string(open.producer) +
                  ",reserved=" + std::to_string(open.requested_bytes) +
                  ",retained=" + std::to_string(retained_bytes) + ")";
      auto handle = std::move(open_reservations[index].handle).Commit(retained_bytes);
      const std::size_t refunded_charge = open.charge_bytes - retained_charge;
      model.active_charge_bytes -= refunded_charge;
      model.returned_credit_bytes[open.producer] += refunded_charge;
      model.logical_retained_bytes -= open.requested_bytes - retained_bytes;
      const std::size_t producer = open.producer;
      open_reservations.erase(open_reservations.begin() + static_cast<std::ptrdiff_t>(index));
      ownerships.push_back(
          CreditOwned{producer, retained_bytes, retained_charge, std::move(handle)});
    } else if (action == 2U) {
      const std::size_t index = random.Index(open_reservations.size());
      const CreditOpen& open = open_reservations[index];
      operation = "abandon(p=" + std::to_string(open.producer) +
                  ",bytes=" + std::to_string(open.requested_bytes) + ")";
      open_reservations[index].handle.Abandon();
      model.logical_retained_bytes -= open.requested_bytes;
      model.active_charge_bytes -= open.charge_bytes;
      model.returned_credit_bytes[open.producer] += open.charge_bytes;
      open_reservations.erase(open_reservations.begin() + static_cast<std::ptrdiff_t>(index));
    } else if (action == 3U) {
      const std::size_t index = random.Index(ownerships.size());
      const CreditOwned& owned = ownerships[index];
      operation = "release(p=" + std::to_string(owned.producer) +
                  ",bytes=" + std::to_string(owned.retained_bytes) + ")";
      ownerships[index].handle.Release();
      model.logical_retained_bytes -= owned.retained_bytes;
      model.active_charge_bytes -= owned.charge_bytes;
      model.returned_credit_bytes[owned.producer] += owned.charge_bytes;
      ownerships.erase(ownerships.begin() + static_cast<std::ptrdiff_t>(index));
    } else if (action == 4U) {
      const std::size_t producer = random.Index(kTraceProducerCount + 1U);
      const std::size_t bytes = RandomReservationSize(random);
      operation = "refill(p=" + std::to_string(producer) + ",bytes=" + std::to_string(bytes) + ")";
      const bool actual = ledger.TryRefillCredit(producer, bytes);
      const bool expected = ModelTryRefill(model, producer, bytes);
      if (actual != expected) {
        const TraceLocation location{"producer-credit-reservation", kCreditTraceSeed, step,
                                     operation};
        CheckTraceEqual(actual, expected, location, "refill result");
        PrintRecentOperations(operation_history, operation);
        return false;
      }
    } else {
      const std::size_t producer = random.Index(kTraceProducerCount + 1U);
      const std::size_t bytes = RandomReservationSize(random);
      operation = "return(p=" + std::to_string(producer) + ",bytes=" + std::to_string(bytes) + ")";
      const bool actual = ledger.ReturnCredit(producer, bytes);
      const bool expected = ModelReturnCredit(model, producer, bytes);
      if (actual != expected) {
        const TraceLocation location{"producer-credit-reservation", kCreditTraceSeed, step,
                                     operation};
        CheckTraceEqual(actual, expected, location, "return result");
        PrintRecentOperations(operation_history, operation);
        return false;
      }
    }

    const TraceLocation location{"producer-credit-reservation", kCreditTraceSeed, step, operation};
    if (!CheckCreditSnapshot(ledger, model, location)) {
      PrintRecentOperations(operation_history, operation);
      return false;
    }
    operation_history.push_back(operation);
  }

  for (auto& open : open_reservations) {
    model.logical_retained_bytes -= open.requested_bytes;
    model.active_charge_bytes -= open.charge_bytes;
    model.returned_credit_bytes[open.producer] += open.charge_bytes;
    open.handle.Abandon();
  }
  for (auto& owned : ownerships) {
    model.logical_retained_bytes -= owned.retained_bytes;
    model.active_charge_bytes -= owned.charge_bytes;
    model.returned_credit_bytes[owned.producer] += owned.charge_bytes;
    owned.handle.Release();
  }
  const TraceLocation before_cleanup{"producer-credit-reservation", kCreditTraceSeed,
                                     kTraceStepCount, "release-live-handles"};
  if (!CheckCreditSnapshot(ledger, model, before_cleanup)) {
    return false;
  }

  ledger.ReturnAllCredits();
  for (std::size_t producer = 0; producer < model.producer_count; ++producer) {
    model.local_credit_bytes[producer] = 0;
    model.returned_credit_bytes[producer] = 0;
  }
  model.central_available_bytes = model.capacity_bytes - model.baseline_bytes;
  const TraceLocation cleanup{"producer-credit-reservation", kCreditTraceSeed, kTraceStepCount + 1U,
                              "return-all-credits"};
  return CheckCreditSnapshot(ledger, model, cleanup);
}

}  // namespace

int main() {
  try {
    const bool success = TestCentralTransitionsAndBuilders() && RunCentralRandomizedOracle() &&
                         TestProducerCreditTransitionsAndBuilders() &&
                         TestCrossThreadOwnershipReturn() && RunProducerCreditRandomizedOracle();
    return success ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "reservation ledger test threw unexpectedly: " << error.what() << '\n';
    return 1;
  }
}
