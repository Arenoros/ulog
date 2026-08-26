#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string_view>
#include <ulog/log.hpp>

#include "producer/producer_kernel.hpp"
#include "support/allocation_interposer.hpp"

namespace {

std::uint64_t default_logger_load_count = 0;

}  // namespace

namespace ulog {

Logger CountedGetDefaultLogger() noexcept {
  ++default_logger_load_count;
  return GetDefaultLogger();
}

}  // namespace ulog

// Macro rescan redirects only this translation unit's expanded default-target calls.
#define GetDefaultLogger() CountedGetDefaultLogger()

namespace {

namespace producer = ulog::detail::producer;
namespace allocation_tracking = ulog::benchmark_support::allocation_tracking;

constexpr std::uint64_t kCycles = 1'024;
constexpr std::string_view kNativeMessage = "frontend";
constexpr std::size_t kMaximumRecordBytes = producer::kMaximumRecordBytes;
constexpr std::size_t kTwoRecordCapacityBytes = 2 * kMaximumRecordBytes;

int failure_count = 0;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    ++failure_count;
    std::cerr << "frontend performance test: " << message << '\n';
  }
}

class KernelFixture final {
 public:
  KernelFixture(std::size_t payload_capacity_bytes, std::size_t ingress_cells,
                bool register_producer)
      : kernel(producer::KernelConfig{
            .threshold = ulog::Level::kTrace,
            .payload_capacity_bytes = payload_capacity_bytes,
            .maximum_record_bytes = kMaximumRecordBytes,
            .producer_slots = 1,
            .ingress_cells = ingress_cells,
        }) {
    if (register_producer) {
      registration = kernel.TryRegisterProducer();
      Check(static_cast<bool>(registration), "failed to register a producer fixture");
    }
  }

  producer::ProducerKernel kernel;
  producer::ProducerKernel::ProducerRegistration registration;
};

KernelFixture& ActiveFixture() {
  static KernelFixture fixture{kTwoRecordCapacityBytes, 2, true};
  return fixture;
}

KernelFixture& UnregisteredFixture() {
  static KernelFixture fixture{kTwoRecordCapacityBytes, 2, false};
  return fixture;
}

KernelFixture& BudgetFixture() {
  static KernelFixture fixture{kMaximumRecordBytes, 2, true};
  return fixture;
}

KernelFixture& LaneFixture() {
  static KernelFixture fixture{kTwoRecordCapacityBytes, 1, true};
  return fixture;
}

class ScopedDefaultLogger final {
 public:
  explicit ScopedDefaultLogger(ulog::Logger logger) noexcept
      : previous_(ulog::ExchangeDefaultLogger(logger)) {}

  ScopedDefaultLogger(const ScopedDefaultLogger&) = delete;
  ScopedDefaultLogger& operator=(const ScopedDefaultLogger&) = delete;

  ~ScopedDefaultLogger() { static_cast<void>(ulog::ExchangeDefaultLogger(previous_)); }

 private:
  ulog::Logger previous_;
};

struct Observation final {
  std::uint64_t records{0};
  std::uint64_t invalid_records{0};
  bool expect_formatted{false};
};

void ObserveRecord(void* context, std::uint64_t, const producer::RecordView& record) noexcept {
  auto& observation = *static_cast<Observation*>(context);
  ++observation.records;
  const bool valid = observation.expect_formatted ? record.message().starts_with("value=")
                                                  : record.message() == kNativeMessage;
  if (!valid) {
    ++observation.invalid_records;
  }
}

void DiscardRecord(void*, std::uint64_t, const producer::RecordView&) noexcept {}

std::string_view CountMessage(std::uint64_t& evaluations) noexcept {
  ++evaluations;
  return kNativeMessage;
}

std::uint64_t CountValue(std::uint64_t& evaluations, std::uint64_t value) noexcept {
  ++evaluations;
  return value;
}

struct MeasurementBaseline final {
  std::uint64_t allocations;
  std::uint64_t allocation_failures;
  std::uint64_t default_loads;
  producer::KernelSnapshot snapshot;
};

struct ExpectedOutcomeDelta final {
  std::uint64_t attempted_records{0};
  std::uint64_t accepted_records{0};
  std::uint64_t consumed_records{0};
  std::uint64_t rejected_no_producer{0};
  std::uint64_t rejected_lane_full{0};
  std::uint64_t rejected_budget{0};
  std::uint64_t abandoned_builds{0};
  std::uint64_t invalid_records{0};
  std::uint64_t truncated_records{0};
};

MeasurementBaseline BeginMeasurement(producer::ProducerKernel& kernel) noexcept {
  return {
      .allocations = allocation_tracking::allocation_count.load(std::memory_order_relaxed),
      .allocation_failures =
          allocation_tracking::allocation_failure_count.load(std::memory_order_relaxed),
      .default_loads = default_logger_load_count,
      .snapshot = kernel.GetSnapshot(),
  };
}

void CheckNoAllocations(const MeasurementBaseline& baseline, std::string_view path) {
  Check(
      allocation_tracking::allocation_count.load(std::memory_order_relaxed) == baseline.allocations,
      path);
  Check(allocation_tracking::allocation_failure_count.load(std::memory_order_relaxed) ==
            baseline.allocation_failures,
        "an allocation failure occurred inside a measured frontend path");
}

void CheckOutcomeDelta(const producer::KernelSnapshot& after,
                       const producer::KernelSnapshot& before, const ExpectedOutcomeDelta& expected,
                       std::string_view failure) {
  Check(after.attempted_records == before.attempted_records + expected.attempted_records &&
            after.accepted_records == before.accepted_records + expected.accepted_records &&
            after.consumed_records == before.consumed_records + expected.consumed_records &&
            after.rejected_no_producer ==
                before.rejected_no_producer + expected.rejected_no_producer &&
            after.rejected_lane_full == before.rejected_lane_full + expected.rejected_lane_full &&
            after.rejected_budget == before.rejected_budget + expected.rejected_budget &&
            after.abandoned_builds == before.abandoned_builds + expected.abandoned_builds &&
            after.invalid_records == before.invalid_records + expected.invalid_records &&
            after.truncated_records == before.truncated_records + expected.truncated_records,
        failure);
}

void TestCompileErasedPath() {
  auto& fixture = ActiveFixture();
  const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
  std::uint64_t evaluations = 0;
  const auto baseline = BeginMeasurement(fixture.kernel);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    LOG_INFO(CountMessage(evaluations));
  }
  const auto snapshot = fixture.kernel.GetSnapshot();
  CheckNoAllocations(baseline, "the compile-erased path allocated");
  Check(evaluations == 0, "the compile-erased path evaluated its message");
  Check(default_logger_load_count == baseline.default_loads,
        "the compile-erased path loaded the Default Logger");
  CheckOutcomeDelta(snapshot, baseline.snapshot, {},
                    "the compile-erased path changed producer outcome accounting");
}

void TestRuntimeFilteredPath() {
  auto& fixture = ActiveFixture();
  fixture.kernel.SetLevel(ulog::Level::kWarning);
  const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
  std::uint64_t evaluations = 0;
  const auto baseline = BeginMeasurement(fixture.kernel);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    LOG(ulog::Level::kInfo, CountMessage(evaluations));
  }
  const auto snapshot = fixture.kernel.GetSnapshot();
  fixture.kernel.SetLevel(ulog::Level::kTrace);
  CheckNoAllocations(baseline, "the runtime-filtered path allocated");
  Check(evaluations == 0, "the runtime-filtered path evaluated its message");
  Check(default_logger_load_count - baseline.default_loads == kCycles,
        "a runtime-filtered call did not load the Default Logger exactly once");
  CheckOutcomeDelta(snapshot, baseline.snapshot, {},
                    "the runtime-filtered path changed producer outcome accounting");
}

void TestNullLoggerPath() {
  auto& fixture = ActiveFixture();
  const ScopedDefaultLogger default_logger{ulog::GetNullLogger()};
  std::uint64_t evaluations = 0;
  const auto baseline = BeginMeasurement(fixture.kernel);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    LOG_CRITICAL(CountMessage(evaluations));
  }
  const auto snapshot = fixture.kernel.GetSnapshot();
  CheckNoAllocations(baseline, "the Null Logger path allocated");
  Check(evaluations == 0, "the Null Logger path evaluated its message");
  Check(default_logger_load_count - baseline.default_loads == kCycles,
        "a Null Logger call did not load the Default Logger exactly once");
  CheckOutcomeDelta(snapshot, baseline.snapshot, {},
                    "the Null Logger path changed producer outcome accounting");
}

void TestUnregisteredPath() {
  auto& fixture = UnregisteredFixture();
  const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
  std::uint64_t evaluations = 0;
  const auto baseline = BeginMeasurement(fixture.kernel);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    LOG_CRITICAL(CountMessage(evaluations));
  }
  const auto snapshot = fixture.kernel.GetSnapshot();
  CheckNoAllocations(baseline, "the unregistered-producer path allocated");
  Check(evaluations == 0, "the unregistered-producer path evaluated its message");
  Check(default_logger_load_count - baseline.default_loads == kCycles,
        "an unregistered-producer call did not load the Default Logger exactly once");
  CheckOutcomeDelta(snapshot, baseline.snapshot,
                    {.attempted_records = kCycles, .rejected_no_producer = kCycles},
                    "unregistered calls did not report exact no-producer accounting");
}

void TestBudgetRejectedPath() {
  auto& fixture = BudgetFixture();
  {
    const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
    LOG_CRITICAL(kNativeMessage);
  }
  std::uint64_t evaluations = 0;
  const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
  const auto baseline = BeginMeasurement(fixture.kernel);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    LOG_CRITICAL(CountMessage(evaluations));
  }
  const auto snapshot = fixture.kernel.GetSnapshot();
  CheckNoAllocations(baseline, "the budget-rejected path allocated");
  Check(evaluations == 0, "the budget-rejected path evaluated its message");
  Check(default_logger_load_count - baseline.default_loads == kCycles,
        "a budget-rejected call did not load the Default Logger exactly once");
  CheckOutcomeDelta(snapshot, baseline.snapshot,
                    {.attempted_records = kCycles, .rejected_budget = kCycles},
                    "budget-rejected calls did not report exact rejection accounting");
  Check(fixture.kernel.TryConsume(nullptr, &DiscardRecord) == producer::ConsumeStatus::kRecord,
        "failed to drain the budget fixture");
}

void TestLaneRejectedPath() {
  auto& fixture = LaneFixture();
  {
    const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
    LOG_CRITICAL(kNativeMessage);
  }
  std::uint64_t evaluations = 0;
  const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
  const auto baseline = BeginMeasurement(fixture.kernel);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    LOG_CRITICAL(CountMessage(evaluations));
  }
  const auto snapshot = fixture.kernel.GetSnapshot();
  CheckNoAllocations(baseline, "the lane-rejected path allocated");
  Check(evaluations == 0, "the lane-rejected path evaluated its message");
  Check(default_logger_load_count - baseline.default_loads == kCycles,
        "a lane-rejected call did not load the Default Logger exactly once");
  CheckOutcomeDelta(snapshot, baseline.snapshot,
                    {.attempted_records = kCycles, .rejected_lane_full = kCycles},
                    "lane-rejected calls did not report exact rejection accounting");
  Check(fixture.kernel.TryConsume(nullptr, &DiscardRecord) == producer::ConsumeStatus::kRecord,
        "failed to drain the lane fixture");
}

void TestWarmAcceptedNativeAndFmtPaths() {
  auto& fixture = ActiveFixture();
  const ScopedDefaultLogger default_logger{fixture.kernel.GetLogger()};
  LOG_CRITICAL(kNativeMessage);
  Check(fixture.kernel.TryConsume(nullptr, &DiscardRecord) == producer::ConsumeStatus::kRecord,
        "failed to warm the accepted native path");
  LOG_CRITICAL("warm={}", 1);
  Check(fixture.kernel.TryConsume(nullptr, &DiscardRecord) == producer::ConsumeStatus::kRecord,
        "failed to warm the accepted fmt path");

  std::uint64_t evaluations = 0;
  Observation native_observation;
  Observation fmt_observation{.expect_formatted = true};
  const auto baseline = BeginMeasurement(fixture.kernel);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    LOG_CRITICAL(CountMessage(evaluations));
    Check(fixture.kernel.TryConsume(&native_observation, &ObserveRecord) ==
              producer::ConsumeStatus::kRecord,
          "failed to consume an accepted native Record");
    LOG_CRITICAL("value={}", CountValue(evaluations, cycle));
    Check(fixture.kernel.TryConsume(&fmt_observation, &ObserveRecord) ==
              producer::ConsumeStatus::kRecord,
          "failed to consume an accepted fmt Record");
  }
  const auto snapshot = fixture.kernel.GetSnapshot();
  constexpr std::uint64_t kExpectedAccepted = 2 * kCycles;
  CheckNoAllocations(baseline, "a warmed accepted native or fmt path allocated");
  Check(evaluations == kExpectedAccepted,
        "a warmed accepted path did not evaluate each message operand exactly once");
  Check(default_logger_load_count - baseline.default_loads == kExpectedAccepted,
        "an accepted call did not load the Default Logger exactly once");
  CheckOutcomeDelta(snapshot, baseline.snapshot,
                    {.attempted_records = kExpectedAccepted,
                     .accepted_records = kExpectedAccepted,
                     .consumed_records = kExpectedAccepted},
                    "ordinary accepted calls did not report exact outcome accounting");
  Check(native_observation.records == kCycles && native_observation.invalid_records == 0,
        "accepted native Records were invalid");
  Check(fmt_observation.records == kCycles && fmt_observation.invalid_records == 0,
        "accepted fmt Records were invalid");
  Check(snapshot.retained_records == 0 && snapshot.logical_retained_bytes == 0 &&
            snapshot.accounting_sample_consistent,
        "accepted path did not finish with consistent empty retained accounting");
}

bool Run() {
  static_assert(ULOG_COMPILE_TIME_MIN_LEVEL == 5);
  static_cast<void>(ActiveFixture());
  static_cast<void>(UnregisteredFixture());
  static_cast<void>(BudgetFixture());
  static_cast<void>(LaneFixture());

  TestCompileErasedPath();
  TestRuntimeFilteredPath();
  TestNullLoggerPath();
  TestUnregisteredPath();
  TestBudgetRejectedPath();
  TestLaneRejectedPath();
  TestWarmAcceptedNativeAndFmtPaths();
  return failure_count == 0;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "frontend performance test failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("frontend performance test failed with an unknown exception\n", stderr);
    return 1;
  }
}
