#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <utility>
#include <vector>

namespace ulog::test {
namespace {

using namespace std::chrono_literals;

template <typename T>
[[nodiscard]] T& RequireValue(std::optional<T>& value) {
  if (!value.has_value()) {
    throw std::logic_error{"expected an optional value"};
  }
  return *value;
}

template <typename T>
[[nodiscard]] const T& RequireValue(const std::optional<T>& value) {
  if (!value.has_value()) {
    throw std::logic_error{"expected an optional value"};
  }
  return *value;
}

[[nodiscard]] RuntimeConfig SmallRuntimeConfig() {
  return RuntimeConfig{
      .threshold = Level::kTrace,
      .payload_capacity_bytes = 512,
      .maximum_record_bytes = 512,
      .producer_slots = 1,
      .ingress_cells = 1,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 1s,
      .destruction_timeout = 1s,
  };
}

using ConfigMutation = void (*)(RuntimeConfig&) noexcept;

struct InvalidRuntimeConfigCase final {
  std::string_view name;
  ConfigMutation mutate;
  RuntimeCreateErrorCode expected_code;
  std::string_view correction_field;
};

void SetInvalidThreshold(RuntimeConfig& config) noexcept {
  config.threshold = std::bit_cast<Level>(std::uint8_t{255});
}

void SetInvalidMaximumRecordBytes(RuntimeConfig& config) noexcept {
  config.maximum_record_bytes = 513;
}

void SetInvalidPayloadCapacity(RuntimeConfig& config) noexcept {
  config.payload_capacity_bytes = 448;
}

void SetInvalidProducerSlots(RuntimeConfig& config) noexcept { config.producer_slots = 0; }

void SetInvalidIngressCells(RuntimeConfig& config) noexcept {
  config.producer_slots = 2;
  config.ingress_cells = 1;
}

void SetInvalidControlCapacity(RuntimeConfig& config) noexcept { config.control_operations = 0; }

void SetInvalidWorkerCount(RuntimeConfig& config) noexcept { config.worker_threads = 2; }

void SetInvalidStartupTimeout(RuntimeConfig& config) noexcept {
  config.startup_timeout = std::chrono::milliseconds::zero();
}

void SetInvalidDestructionTimeout(RuntimeConfig& config) noexcept {
  config.destruction_timeout = std::chrono::milliseconds::zero();
}

void SetTooLargeStartupTimeout(RuntimeConfig& config) noexcept { config.startup_timeout = 25h; }

void SetTooLargeDestructionTimeout(RuntimeConfig& config) noexcept {
  config.destruction_timeout = 25h;
}

void SetInvalidAdmissionPolicy(RuntimeConfig& config) noexcept {
  config.admission_policy = std::bit_cast<AdmissionPolicy>(std::uint8_t{7});
}

void SetUndersizedPriorityReserve(RuntimeConfig& config) noexcept {
  config.priority_reserve_bytes = config.maximum_record_bytes - 64U;
}

void SetUnalignedPriorityReserve(RuntimeConfig& config) noexcept {
  config.priority_reserve_bytes = config.maximum_record_bytes + 1U;
}

void SetInvalidThinningWatermark(RuntimeConfig& config) noexcept {
  config.admission_policy = AdmissionPolicy::kAdaptiveThinning;
  config.thinning_low_watermark_percent = 100;
}

void SetInvalidDestinationBound(RuntimeConfig& config) noexcept {
  config.maximum_record_bytes = 1'024;
  config.payload_capacity_bytes = 1'024;
}

constexpr std::array kInvalidRuntimeConfigs{
    InvalidRuntimeConfigCase{"threshold", &SetInvalidThreshold,
                             RuntimeCreateErrorCode::kInvalidThreshold, "threshold"},
    InvalidRuntimeConfigCase{"maximum record bytes", &SetInvalidMaximumRecordBytes,
                             RuntimeCreateErrorCode::kInvalidMaximumRecordBytes,
                             "maximum_record_bytes"},
    InvalidRuntimeConfigCase{"payload capacity", &SetInvalidPayloadCapacity,
                             RuntimeCreateErrorCode::kInvalidPayloadCapacity,
                             "payload_capacity_bytes"},
    InvalidRuntimeConfigCase{"producer slots", &SetInvalidProducerSlots,
                             RuntimeCreateErrorCode::kInvalidProducerSlots, "producer_slots"},
    InvalidRuntimeConfigCase{"ingress cells", &SetInvalidIngressCells,
                             RuntimeCreateErrorCode::kInvalidIngressCells, "ingress_cells"},
    InvalidRuntimeConfigCase{"control capacity", &SetInvalidControlCapacity,
                             RuntimeCreateErrorCode::kInvalidControlCapacity, "control_operations"},
    InvalidRuntimeConfigCase{"worker count", &SetInvalidWorkerCount,
                             RuntimeCreateErrorCode::kInvalidWorkerCount, "worker_threads"},
    InvalidRuntimeConfigCase{"startup timeout", &SetInvalidStartupTimeout,
                             RuntimeCreateErrorCode::kInvalidStartupTimeout, "startup_timeout"},
    InvalidRuntimeConfigCase{"destruction timeout", &SetInvalidDestructionTimeout,
                             RuntimeCreateErrorCode::kInvalidDestructionTimeout,
                             "destruction_timeout"},
    InvalidRuntimeConfigCase{"startup timeout upper bound", &SetTooLargeStartupTimeout,
                             RuntimeCreateErrorCode::kInvalidStartupTimeout, "startup_timeout"},
    InvalidRuntimeConfigCase{"destruction timeout upper bound", &SetTooLargeDestructionTimeout,
                             RuntimeCreateErrorCode::kInvalidDestructionTimeout,
                             "destruction_timeout"},
    InvalidRuntimeConfigCase{"destination bound", &SetInvalidDestinationBound,
                             RuntimeCreateErrorCode::kInvalidDestination, "destination"},
    InvalidRuntimeConfigCase{"admission policy", &SetInvalidAdmissionPolicy,
                             RuntimeCreateErrorCode::kInvalidAdmissionPolicy, "admission_policy"},
    InvalidRuntimeConfigCase{"undersized priority reserve", &SetUndersizedPriorityReserve,
                             RuntimeCreateErrorCode::kInvalidPriorityReserve,
                             "priority_reserve_bytes"},
    InvalidRuntimeConfigCase{"unaligned priority reserve", &SetUnalignedPriorityReserve,
                             RuntimeCreateErrorCode::kInvalidPriorityReserve,
                             "priority_reserve_bytes"},
    InvalidRuntimeConfigCase{"thinning watermark", &SetInvalidThinningWatermark,
                             RuntimeCreateErrorCode::kInvalidThinningWatermark,
                             "thinning_low_watermark_percent"},
};

void ExpectSucceeded(OperationStartResult& started) {
  ASSERT_TRUE(started) << (started.failure ? started.failure.value().Message()
                                           : "missing Operation");
  const auto completed = started.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion);
  EXPECT_EQ(RequireValue(completed.completion).Outcome(), OperationOutcome::kSucceeded);
}

/// Deterministic thread-safe clock that counts every read.
class CountingClock final {
 public:
  static constexpr std::int64_t kFirst = 1'704'067'199'999'999;  // 2023-12-31T23:59:59.999999Z
  static constexpr std::int64_t kStep = 1'000'001;

  [[nodiscard]] EventClock Seam() noexcept { return EventClock{this, &CountingClock::Now}; }
  [[nodiscard]] std::uint64_t Reads() const noexcept {
    return reads_.load(std::memory_order_relaxed);
  }

 private:
  static std::int64_t Now(void* context) noexcept {
    auto& clock = *static_cast<CountingClock*>(context);
    const auto read = clock.reads_.fetch_add(1, std::memory_order_relaxed);
    return kFirst + static_cast<std::int64_t>(read) * kStep;
  }

  std::atomic<std::uint64_t> reads_{0};
};

[[nodiscard]] std::int64_t SystemMicroseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

[[nodiscard]] std::optional<testing::ObservedRecord> WaitForRecord(
    testing::InMemoryDestination& destination,
    const std::chrono::steady_clock::time_point deadline) noexcept {
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto record = destination.TryTake()) {
      return record;
    }
    std::this_thread::yield();
  }
  return std::nullopt;
}

TEST(RuntimeTracer, CreationRejectsEveryInvalidConfigurationFieldWithGuidance) {
  for (const auto& test_case : kInvalidRuntimeConfigs) {
    SCOPED_TRACE(test_case.name);
    auto config = SmallRuntimeConfig();
    test_case.mutate(config);
    testing::InMemoryDestination destination{{
        .capacity_records = 2,
        .maximum_record_bytes = 512,
    }};

    auto created = Runtime::Create(config, destination);

    ASSERT_FALSE(created);
    ASSERT_TRUE(created.failure);
    const auto& failure = RequireValue(created.failure);
    EXPECT_EQ(failure.code, test_case.expected_code);
    EXPECT_FALSE(failure.Message().empty());
    EXPECT_NE(failure.HowToFix().find(test_case.correction_field), std::string_view::npos);
  }
}

static_assert(RuntimeConfig{}.admission_policy == AdmissionPolicy::kDropNewest);
static_assert(RuntimeConfig{}.priority_reserve_bytes == 0U);

TEST(RuntimeTracer, SnapshotReportsTheConfiguredPriorityReserveAndItsRejections) {
  for (const std::size_t reserve : {std::size_t{0}, std::size_t{1'024}}) {
    SCOPED_TRACE(reserve);
    testing::InMemoryDestination destination{{
        .capacity_records = 1,
        .maximum_record_bytes = 512,
    }};
    auto config = SmallRuntimeConfig();
    config.priority_reserve_bytes = reserve;
    auto created = Runtime::Create(config, destination);
    ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
    const RuntimeSnapshot snapshot = created.runtime->GetSnapshot();
    EXPECT_EQ(snapshot.priority_reserve_bytes, reserve);
    EXPECT_EQ(snapshot.payload_capacity_bytes, 512U);
    EXPECT_EQ(snapshot.rejected_priority_reserve, 0U);
    EXPECT_EQ(snapshot.physical_retained_bytes, 0U);
  }
}

TEST(RuntimeTracer, ShutdownDeliversOneInfoRecordAndClosesAdmission) {
  testing::InMemoryDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();
  const std::size_t fixed_backing_bytes = runtime.GetSnapshot().fixed_backing_bytes;
  ASSERT_GT(fixed_backing_bytes, 0U);

  LOG_INFO_TO(logger, "answer={}", 42);
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);

  auto record = destination.TryTake();
  ASSERT_TRUE(record);
  EXPECT_EQ(RequireValue(record).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(record).GetLevel(), Level::kInfo);
  EXPECT_EQ(RequireValue(record).Message(), "answer=42");
  EXPECT_FALSE(destination.TryTake());

  std::size_t late_evaluations = 0;
  LOG_INFO_TO(logger, "late={}", ++late_evaluations);
  EXPECT_EQ(late_evaluations, 0U);
  EXPECT_EQ(runtime.GetSnapshot().accepted_records, 1U);
  EXPECT_EQ(runtime.GetSnapshot().fixed_backing_bytes, fixed_backing_bytes);
}

TEST(RuntimeTracer, DrainPublishesPriorRecordsInAdmissionOrderAndLeavesRuntimeOpen) {
  testing::InMemoryDestination destination{{
      .capacity_records = 4,
      .maximum_record_bytes = 512,
  }};
  auto config = SmallRuntimeConfig();
  config.payload_capacity_bytes = 1'536;
  config.ingress_cells = 3;
  auto created = Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_INFO_TO(logger, "zero");
  LOG_INFO_TO(logger, "one");
  LOG_INFO_TO(logger, "two");
  auto drain = runtime.Drain();
  ExpectSucceeded(drain);

  auto zero = destination.TryTake();
  auto one = destination.TryTake();
  auto two = destination.TryTake();
  ASSERT_TRUE(zero);
  ASSERT_TRUE(one);
  ASSERT_TRUE(two);
  EXPECT_EQ(RequireValue(zero).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(one).AdmissionSequence(), 1U);
  EXPECT_EQ(RequireValue(two).AdmissionSequence(), 2U);
  EXPECT_EQ(RequireValue(zero).Message(), "zero");
  EXPECT_EQ(RequireValue(one).Message(), "one");
  EXPECT_EQ(RequireValue(two).Message(), "two");

  LOG_INFO_TO(logger, "after-drain");
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
  auto after = destination.TryTake();
  ASSERT_TRUE(after);
  EXPECT_EQ(RequireValue(after).AdmissionSequence(), 3U);
  EXPECT_EQ(RequireValue(after).Message(), "after-drain");
}

TEST(RuntimeTracer, SaturationDropsNewestBeforeEvaluationWhileControlReserveStillWorks) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  std::size_t accepted_evaluations = 0;
  LOG_INFO_TO(logger, "accepted={}", ++accepted_evaluations);
  std::size_t rejected_evaluations = 0;
  LOG_INFO_TO(logger, "rejected={}", ++rejected_evaluations);

  const auto saturated = runtime.GetSnapshot();
  EXPECT_EQ(accepted_evaluations, 1U);
  EXPECT_EQ(rejected_evaluations, 0U);
  EXPECT_EQ(saturated.accepted_records, 1U);
  EXPECT_EQ(saturated.dropped_newest_records, 1U);
  EXPECT_EQ(saturated.retained_records, 1U);

  auto drain = runtime.Drain();
  ASSERT_TRUE(drain);
  EXPECT_EQ(drain.operation.Poll().status, OperationPollStatus::kPending);
  destination.Resume();
  ExpectSucceeded(drain);

  auto record = destination.TryTake();
  ASSERT_TRUE(record);
  EXPECT_EQ(RequireValue(record).Message(), "accepted=1");
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeTracer, DurableFlushStartsWhileSaturatedAndClaimsNoInMemoryDurability) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  auto config = SmallRuntimeConfig();
  // The flush, Shutdown, and late flush handles each retain a control slot.
  config.control_operations = 3;
  auto created = Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_INFO_TO(logger, "accepted");
  LOG_INFO_TO(logger, "rejected");
  const auto saturated = runtime.GetSnapshot();
  EXPECT_EQ(saturated.accepted_records, 1U);
  EXPECT_EQ(saturated.dropped_newest_records, 1U);

  // The flush uses the control reserve, so it starts while the payload budget is exhausted.
  auto flush = runtime.DurableFlush();
  ASSERT_TRUE(flush);
  EXPECT_EQ(flush.operation.Poll().status, OperationPollStatus::kPending);
  destination.Resume();
  const auto flushed = flush.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(flushed.status, OperationWaitStatus::kCompleted);
  EXPECT_EQ(RequireValue(flushed.completion).Outcome(), OperationOutcome::kSucceeded);
  EXPECT_EQ(RequireValue(flushed.completion).Report(),
            (OperationReport{.watermark_records = 1,
                             .processed_records = 1,
                             .delivered_records = 1,
                             .durability = OperationDurability::kNotApplicable}));

  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
  // After the route stopped, an in-memory flush still only reports the barrier.
  auto late = runtime.DurableFlush();
  ASSERT_TRUE(late);
  const auto late_result = late.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(late_result.status, OperationWaitStatus::kCompleted);
  EXPECT_EQ(RequireValue(late_result.completion).Outcome(), OperationOutcome::kSucceeded);
  EXPECT_EQ(RequireValue(late_result.completion).Report().durability,
            OperationDurability::kNotApplicable);
}

/// Restores the previous Default Logger even when an assertion returns early.
class DefaultLoggerScope final {
 public:
  explicit DefaultLoggerScope(Logger logger) noexcept : previous_(ExchangeDefaultLogger(logger)) {}
  DefaultLoggerScope(const DefaultLoggerScope&) = delete;
  DefaultLoggerScope& operator=(const DefaultLoggerScope&) = delete;
  ~DefaultLoggerScope() { static_cast<void>(ExchangeDefaultLogger(previous_)); }

 private:
  Logger previous_;
};

TEST(RuntimeTracer, ShutdownDeadlineStartsWhileSaturatedAndLeavesLoggersSafe) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();
  const DefaultLoggerScope default_scope{logger};

  LOG_INFO_TO(logger, "accepted");
  LOG_INFO_TO(logger, "rejected");
  EXPECT_EQ(runtime.GetSnapshot().dropped_newest_records, 1U);

  // Shutdown uses the control reserve, so it starts while the payload budget is exhausted.
  auto shutdown = runtime.Shutdown(std::chrono::steady_clock::now() + 30ms);
  ASSERT_TRUE(shutdown);
  const auto stopped = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(stopped.status, OperationWaitStatus::kCompleted);
  EXPECT_EQ(RequireValue(stopped.completion).Outcome(), OperationOutcome::kCancelled);
  EXPECT_EQ(RequireValue(stopped.completion).Report(),
            (OperationReport{.watermark_records = 1, .unfinished_records = 1}));

  // Later calls through the Runtime Logger and the Default Logger are rejected before evaluation.
  std::size_t late_evaluations = 0;
  LOG_INFO_TO(logger, "late={}", ++late_evaluations);
  LOG_INFO("late default={}", ++late_evaluations);
  EXPECT_EQ(late_evaluations, 0U);
  EXPECT_EQ(GetDefaultLogger(), logger);
  EXPECT_EQ(logger.SetLevel(Level::kTrace), LevelChangeStatus::kClosed);
  const auto snapshot = runtime.GetSnapshot();
  EXPECT_EQ(snapshot.accepted_records, 1U);
  EXPECT_FALSE(snapshot.admission_open);
  EXPECT_FALSE(snapshot.worker_running);

  // The discarded Record never reaches the destination.
  destination.Resume();
  EXPECT_FALSE(destination.TryTake());
}

TEST(RuntimeTracer, ShutdownWithAnExpiredDeadlineSucceedsWhenNothingIsPending) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_INFO_TO(logger, "delivered");
  auto drain = runtime.Drain();
  ExpectSucceeded(drain);
  // The worker finishes a Shutdown whose barrier is already reached before checking the deadline.
  auto shutdown = runtime.Shutdown(std::chrono::steady_clock::now() - 1s);
  ExpectSucceeded(shutdown);
}

TEST(RuntimeTracer, PausedDestinationClaimsNoSlotAcrossWorkerRechecks) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_INFO_TO(logger, "held");
  auto drain = runtime.Drain();
  ASSERT_TRUE(drain);
  // The deadline spans several private worker recheck intervals.
  const auto paused = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 100ms);
  EXPECT_EQ(paused.status, OperationWaitStatus::kDeadlineExceeded);

  const auto held = runtime.GetSnapshot();
  EXPECT_EQ(held.accepted_records, 1U);
  EXPECT_EQ(held.completed_records, 0U);
  EXPECT_EQ(held.delivered_records, 0U);
  EXPECT_EQ(held.retained_records, 1U);
  EXPECT_FALSE(destination.TryTake());

  destination.Resume();
  ExpectSucceeded(drain);
  auto record = destination.TryTake();
  ASSERT_TRUE(record);
  EXPECT_EQ(RequireValue(record).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(record).Message(), "held");

  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeTracer, DrainReportCountsStructuredRecordsWithoutEncodedBytes) {
  testing::InMemoryDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  auto config = SmallRuntimeConfig();
  config.payload_capacity_bytes = 1'024;
  config.ingress_cells = 2;
  auto created = Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_INFO_TO(logger, "first");
  LOG_INFO_TO(logger, "second");
  auto drain = runtime.Drain();
  ASSERT_TRUE(drain);
  const auto drained = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(drained.status, OperationWaitStatus::kCompleted);
  ASSERT_TRUE(drained.completion);
  EXPECT_EQ(RequireValue(drained.completion).Outcome(), OperationOutcome::kSucceeded);
  const OperationReport& report = RequireValue(drained.completion).Report();
  EXPECT_EQ(report.watermark_records, 2U);
  EXPECT_EQ(report.processed_records, 2U);
  EXPECT_EQ(report.delivered_records, 2U);
  EXPECT_EQ(report.failed_records, 0U);
  EXPECT_EQ(report.unfinished_records, 0U);
  EXPECT_EQ(report.processed_bytes, 0U);
  EXPECT_EQ(report.delivered_bytes, 0U);

  const auto snapshot = runtime.GetSnapshot();
  EXPECT_EQ(snapshot.processed_records, 2U);
  EXPECT_EQ(snapshot.completed_records, 2U);
  EXPECT_EQ(snapshot.delivery_failed_records, 0U);
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeTracer, OversizedUtf8MessageIsCapturedAsAnImmutableTruncatedPrefix) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const Logger logger = created.runtime->GetLogger();
  std::string oversized{"prefix-"};
  for (std::size_t index = 0; index < 256; ++index) {
    oversized.append("\xE2\x82\xAC");
  }

  LOG_INFO_TO(logger, "{}", oversized);
  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);

  auto record = destination.TryTake();
  ASSERT_TRUE(record);
  const auto& captured = RequireValue(record);
  const std::string_view observed = captured.Message();
  EXPECT_TRUE(captured.Truncated());
  ASSERT_GE(observed.size(), std::string_view{"prefix-"}.size());
  EXPECT_LT(observed.size(), oversized.size());
  EXPECT_TRUE(std::string_view{oversized}.starts_with(observed));
  EXPECT_EQ((observed.size() - std::string_view{"prefix-"}.size()) % 3U, 0U);
}

TEST(RuntimeTracer, HeldObservationPinsItsSlotAndBackpressuresDrainWithoutMutation) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto config = SmallRuntimeConfig();
  config.payload_capacity_bytes = 1'024;
  config.ingress_cells = 2;
  auto created = Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  auto first = WaitForRecord(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first);
  const auto& pinned = RequireValue(first);
  const std::string_view pinned_message = pinned.Message();
  const std::uint64_t pinned_sequence = pinned.AdmissionSequence();

  LOG_INFO_TO(logger, "second");
  auto drain = created.runtime->Drain();
  ASSERT_TRUE(drain);
  const auto blocked = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 50ms);
  EXPECT_EQ(blocked.status, OperationWaitStatus::kDeadlineExceeded);
  EXPECT_EQ(pinned.Message(), pinned_message);
  EXPECT_EQ(pinned.Message(), "first");
  EXPECT_EQ(pinned.AdmissionSequence(), pinned_sequence);

  first.reset();
  ExpectSucceeded(drain);
  auto second = destination.TryTake();
  ASSERT_TRUE(second);
  EXPECT_EQ(RequireValue(second).AdmissionSequence(), 1U);
  EXPECT_EQ(RequireValue(second).Message(), "second");

  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeTracer, DestructionCancelsPendingWorkWithoutWaitingForThePausedDestination) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "pending");
  auto drain = created.runtime->Drain();
  ASSERT_TRUE(drain);

  created.runtime.reset();

  const auto cancelled = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(cancelled.status, OperationWaitStatus::kCompleted);
  ASSERT_TRUE(cancelled.completion);
  EXPECT_EQ(RequireValue(cancelled.completion).Outcome(), OperationOutcome::kCancelled);
}

TEST(RuntimeTracer, LongLivedProducerCanRegisterAcrossCrossThreadRuntimeRestarts) {
  constexpr std::size_t kRuntimeRestarts = 40;
  std::vector<std::unique_ptr<Runtime>> stopped_runtimes;
  stopped_runtimes.reserve(kRuntimeRestarts);

  for (std::size_t restart = 0; restart < kRuntimeRestarts; ++restart) {
    SCOPED_TRACE(restart);
    testing::InMemoryDestination destination{{
        .capacity_records = 1,
        .maximum_record_bytes = 512,
    }};
    auto created = Runtime::Create(SmallRuntimeConfig(), destination);
    ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
    const Logger logger = created.runtime->GetLogger();
    LOG_INFO_TO(logger, "restart={}", restart);

    bool shutdown_succeeded = false;
    std::thread controller{[runtime = created.runtime.get(), &shutdown_succeeded]() noexcept {
      auto shutdown = runtime->Shutdown();
      if (!shutdown) {
        return;
      }
      const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
      shutdown_succeeded = completed.status == OperationWaitStatus::kCompleted &&
                           completed.completion &&
                           completed.completion->Outcome() == OperationOutcome::kSucceeded;
    }};
    controller.join();

    ASSERT_TRUE(shutdown_succeeded);
    auto record = destination.TryTake();
    ASSERT_TRUE(record);
    EXPECT_EQ(RequireValue(record).AdmissionSequence(), 0U);
    stopped_runtimes.push_back(std::move(created.runtime));
  }
}

TEST(RuntimeTracer, DestinationRejectsConcurrentAndSequentialRuntimeAttachment) {
  testing::InMemoryDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  auto first = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(first) << (first.failure ? first.failure->Message() : "missing Runtime");

  auto concurrent = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(concurrent);
  ASSERT_TRUE(concurrent.failure);
  const auto& concurrent_failure = RequireValue(concurrent.failure);
  EXPECT_EQ(concurrent_failure.code, RuntimeCreateErrorCode::kInvalidDestination);
  EXPECT_NE(concurrent_failure.HowToFix().find("destination"), std::string_view::npos);

  auto shutdown = first.runtime->Shutdown();
  ExpectSucceeded(shutdown);
  first.runtime.reset();

  auto reused = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(reused);
  ASSERT_TRUE(reused.failure);
  EXPECT_EQ(RequireValue(reused.failure).code, RuntimeCreateErrorCode::kInvalidDestination);
}

TEST(RuntimeTracer, InjectedClockStampsOnlyAdmittedRecordsWithTheProducerReadValue) {
  testing::InMemoryDestination destination{{
      .capacity_records = 4,
      .maximum_record_bytes = 512,
  }};
  CountingClock clock;
  auto config = SmallRuntimeConfig();
  config.threshold = Level::kInfo;
  config.payload_capacity_bytes = 1'536;
  config.ingress_cells = 3;
  config.event_clock = clock.Seam();
  auto created = Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_DEBUG_TO(logger, "filtered");
  LOG_INFO_TO(GetNullLogger(), "null");
  std::thread unregistered{[logger] { LOG_INFO_TO(logger, "unregistered"); }};
  unregistered.join();
  EXPECT_EQ(clock.Reads(), 0U);
  EXPECT_EQ(runtime.GetSnapshot().rejected_no_producer, 1U);

  LOG_INFO_TO(logger, "zero");
  LOG_WARNING_TO(logger, "one");
  LOG_ERROR_TO(logger, "two");
  EXPECT_EQ(clock.Reads(), 3U);
  auto drain = runtime.Drain();
  ExpectSucceeded(drain);

  for (std::uint64_t sequence = 0; sequence < 3U; ++sequence) {
    auto record = destination.TryTake();
    ASSERT_TRUE(record);
    EXPECT_EQ(RequireValue(record).AdmissionSequence(), sequence);
    EXPECT_EQ(RequireValue(record).EventTimestamp(),
              CountingClock::kFirst + static_cast<std::int64_t>(sequence) * CountingClock::kStep);
  }
  // The worker copied the stored values; delivery and Drain read the clock no further.
  EXPECT_EQ(clock.Reads(), 3U);
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
  EXPECT_EQ(clock.Reads(), 3U);
}

TEST(RuntimeTracer, AdmissionRejectedAndClosedCallsReadNoClock) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  CountingClock clock;
  auto config = SmallRuntimeConfig();
  config.event_clock = clock.Seam();
  auto created = Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_INFO_TO(logger, "accepted");
  LOG_INFO_TO(logger, "lane-full");
  EXPECT_EQ(runtime.GetSnapshot().dropped_newest_records, 1U);
  EXPECT_EQ(clock.Reads(), 1U);

  auto shutdown = runtime.Shutdown();
  ASSERT_TRUE(shutdown);
  LOG_INFO_TO(logger, "closed");
  destination.Resume();
  ExpectSucceeded(shutdown);
  EXPECT_EQ(clock.Reads(), 1U);
  auto record = destination.TryTake();
  ASSERT_TRUE(record);
  EXPECT_EQ(RequireValue(record).Message(), "accepted");
  EXPECT_EQ(RequireValue(record).EventTimestamp(), CountingClock::kFirst);
}

TEST(RuntimeTracer, DefaultClockStampsTheSystemTimeOfTheCall) {
  testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto created = Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  const std::int64_t before = SystemMicroseconds();
  LOG_INFO_TO(logger, "now");
  const std::int64_t after = SystemMicroseconds();
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);

  auto record = destination.TryTake();
  ASSERT_TRUE(record);
  EXPECT_GE(RequireValue(record).EventTimestamp(), before);
  EXPECT_LE(RequireValue(record).EventTimestamp(), after);
}

}  // namespace
}  // namespace ulog::test
