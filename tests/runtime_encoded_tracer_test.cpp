#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>

namespace ulog {

void PrintTo(const OperationReport& report, std::ostream* output) {
  *output << "{watermark=" << report.watermark_records << " processed=" << report.processed_records
          << "/" << report.processed_bytes << " delivered=" << report.delivered_records << "/"
          << report.delivered_bytes << " failed=" << report.failed_records << "/"
          << report.failed_bytes << " unfinished=" << report.unfinished_records << "/"
          << report.unfinished_bytes << "}";
}

}  // namespace ulog

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;

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

[[nodiscard]] ulog::RuntimeConfig SmallRuntimeConfig() {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
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

void ExpectSucceeded(ulog::OperationStartResult& started) {
  ASSERT_TRUE(started) << (started.failure ? started.failure->Message() : "missing Operation");
  const auto completed = started.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, ulog::OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion.has_value());
  EXPECT_EQ(RequireValue(completed.completion).Outcome(), ulog::OperationOutcome::kSucceeded);
}

[[nodiscard]] std::optional<ulog::testing::ObservedEncodedRecord> WaitForEncodedRecord(
    ulog::testing::InMemoryEncodedDestination& destination,
    std::chrono::steady_clock::time_point deadline) noexcept {
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto record = destination.TryTake()) {
      return record;
    }
    std::this_thread::yield();
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<ulog::testing::PendingEncodedDelivery> WaitForPendingDelivery(
    ulog::testing::InMemoryEncodedDestination& destination,
    std::chrono::steady_clock::time_point deadline) noexcept {
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto delivery = destination.TryTakePendingDelivery()) {
      return delivery;
    }
    std::this_thread::yield();
  }
  return std::nullopt;
}

[[nodiscard]] ulog::OperationResult WaitForResult(ulog::OperationStartResult& started) {
  if (!started) {
    ADD_FAILURE() << (started.failure ? started.failure->Message() : "missing Operation");
    return {};
  }
  const auto completed = started.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  if (completed.status != ulog::OperationWaitStatus::kCompleted || !completed.completion) {
    ADD_FAILURE() << "Operation did not complete: " << completed.Message();
    return {};
  }
  return *completed.completion;
}

[[nodiscard]] bool WaitForProcessed(const ulog::Runtime& runtime, std::uint64_t records) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runtime.GetSnapshot().processed_records == records) {
      return true;
    }
    std::this_thread::yield();
  }
  return runtime.GetSnapshot().processed_records == records;
}

[[nodiscard]] ulog::RuntimeConfig DeferredRuntimeConfig(std::size_t ingress_cells) {
  auto config = SmallRuntimeConfig();
  config.payload_capacity_bytes = ingress_cells * 512U;
  config.ingress_cells = ingress_cells;
  config.control_operations = 4;
  return config;
}

TEST(RuntimeEncodedTracer, PublicLoggerDeliversBaselineRawFrame) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");

  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "hello");

  auto shutdown = created.runtime->Shutdown();
  ASSERT_FALSE(shutdown.failure.has_value());
  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, ulog::OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion.has_value());
  EXPECT_EQ(RequireValue(completed.completion).Outcome(), ulog::OperationOutcome::kSucceeded);

  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(RequireValue(frame).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(frame).Bytes(), "tskv\ttext=hello\n");
  EXPECT_FALSE(destination.TryTake().has_value());
}

TEST(RuntimeEncodedTracer, EmptyAndUnicodeControlMessagesMatchIndependentRawLiterals) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  auto config = SmallRuntimeConfig();
  config.payload_capacity_bytes = 1'024;
  config.ingress_cells = 2;
  auto created = ulog::Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");

  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "");
  constexpr std::string_view kMessage =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|\r\n\0\t\\|literal:\\r\\n\\0\\t\\\\"sv;
  constexpr std::string_view kExpectedFrame =
      "tskv\ttext=\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|"
      "\\r\\n\\0\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\n"sv;
  LOG_INFO_TO(logger, "{}", kMessage);

  auto shutdown = created.runtime->Shutdown();
  ASSERT_TRUE(shutdown);
  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, ulog::OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion.has_value());
  EXPECT_EQ(RequireValue(completed.completion).Outcome(), ulog::OperationOutcome::kSucceeded);

  auto empty = destination.TryTake();
  auto controls = destination.TryTake();
  ASSERT_TRUE(empty.has_value());
  ASSERT_TRUE(controls.has_value());
  EXPECT_EQ(RequireValue(empty).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(empty).Bytes(), "tskv\ttext=\n");
  EXPECT_EQ(RequireValue(controls).AdmissionSequence(), 1U);
  EXPECT_EQ(RequireValue(controls).Bytes(), kExpectedFrame);
}

TEST(RuntimeEncodedTracer, TruncationMarkerIsEncodedBeforeTheRetainedMessagePrefix) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();
  const std::string oversized(1'024, 'x');

  LOG_INFO_TO(logger, "{}", oversized);
  auto shutdown = created.runtime->Shutdown();
  ASSERT_TRUE(shutdown);
  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, ulog::OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion.has_value());
  ASSERT_EQ(RequireValue(completed.completion).Outcome(), ulog::OperationOutcome::kSucceeded);

  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  const std::string_view encoded = RequireValue(frame).Bytes();
  constexpr std::string_view kPrefix{"tskv\tulog.truncated=1\ttext="};
  ASSERT_TRUE(encoded.starts_with(kPrefix));
  ASSERT_TRUE(encoded.ends_with("\n"));
  const std::string_view retained =
      encoded.substr(kPrefix.size(), encoded.size() - kPrefix.size() - 1U);
  EXPECT_LT(retained.size(), oversized.size());
  EXPECT_TRUE(std::string_view{oversized}.starts_with(retained));
}

TEST(RuntimeEncodedTracer, PausedAndHeldSlotBackpressurePreservesFifoAndExactAccounting) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  auto config = SmallRuntimeConfig();
  config.payload_capacity_bytes = 1'024;
  config.ingress_cells = 2;
  auto created = ulog::Runtime::Create(config, destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  LOG_INFO_TO(logger, "second");
  std::size_t rejected_evaluations = 0;
  LOG_INFO_TO(logger, "rejected={}", ++rejected_evaluations);

  const auto saturated = created.runtime->GetSnapshot();
  EXPECT_EQ(rejected_evaluations, 0U);
  EXPECT_EQ(saturated.accepted_records, 2U);
  EXPECT_EQ(saturated.dropped_newest_records, 1U);
  EXPECT_EQ(saturated.retained_records, 2U);

  auto drain = created.runtime->Drain();
  ASSERT_TRUE(drain);
  EXPECT_EQ(drain.operation.Poll().status, ulog::OperationPollStatus::kPending);
  destination.Resume();

  auto first = WaitForEncodedRecord(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(RequireValue(first).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(first).Bytes(), "tskv\ttext=first\n");
  const std::string_view pinned_bytes = RequireValue(first).Bytes();
  const auto blocked = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 50ms);
  EXPECT_EQ(blocked.status, ulog::OperationWaitStatus::kDeadlineExceeded);
  EXPECT_EQ(RequireValue(first).Bytes(), pinned_bytes);

  first.reset();
  ExpectSucceeded(drain);
  auto second = destination.TryTake();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(RequireValue(second).AdmissionSequence(), 1U);
  EXPECT_EQ(RequireValue(second).Bytes(), "tskv\ttext=second\n");

  const auto drained = created.runtime->GetSnapshot();
  EXPECT_EQ(drained.completed_records, 2U);
  EXPECT_EQ(drained.delivered_records, 2U);
  EXPECT_EQ(drained.delivered_bytes, 33U);
  EXPECT_EQ(drained.encoding_failed_records, 0U);
  EXPECT_EQ(drained.retained_records, 0U);

  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeEncodedTracer, PausedDestinationClaimsNoSlotAcrossWorkerRechecks) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "held");
  auto drain = created.runtime->Drain();
  ASSERT_TRUE(drain);
  // The deadline spans several private worker recheck intervals.
  const auto paused = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 100ms);
  EXPECT_EQ(paused.status, ulog::OperationWaitStatus::kDeadlineExceeded);

  const auto held = created.runtime->GetSnapshot();
  EXPECT_EQ(held.accepted_records, 1U);
  EXPECT_EQ(held.completed_records, 0U);
  EXPECT_EQ(held.delivered_records, 0U);
  EXPECT_EQ(held.delivered_bytes, 0U);
  EXPECT_EQ(held.retained_records, 1U);
  EXPECT_FALSE(destination.TryTake().has_value());

  destination.Resume();
  ExpectSucceeded(drain);
  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(RequireValue(frame).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(frame).Bytes(), "tskv\ttext=held\n");

  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeEncodedTracer, ObservationRemainsValidAfterDestinationAndRuntimeDestruction) {
  std::optional<ulog::testing::ObservedEncodedRecord> observed;
  {
    ulog::testing::InMemoryEncodedDestination destination{{
        .capacity_records = 1,
        .maximum_record_bytes = 512,
    }};
    auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
    ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
    const ulog::Logger logger = created.runtime->GetLogger();
    LOG_INFO_TO(logger, "survives");
    auto shutdown = created.runtime->Shutdown();
    ExpectSucceeded(shutdown);
    observed = destination.TryTake();
    ASSERT_TRUE(observed.has_value());
    created.runtime.reset();
  }

  EXPECT_EQ(RequireValue(observed).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(observed).Bytes(), "tskv\ttext=survives\n");
}

TEST(RuntimeEncodedTracer, DestinationBoundIsDerivedAndStateAttachesOnlyOnce) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  EXPECT_EQ(destination.Capacity(), 2U);
  EXPECT_EQ(destination.MaximumRecordBytes(), 512U);
  EXPECT_EQ(destination.MaximumEncodedRecordBytes(), 1'035U);

  auto first = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(first) << (first.failure ? first.failure->Message() : "missing Runtime");
  auto concurrent = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(concurrent);
  ASSERT_TRUE(concurrent.failure.has_value());
  EXPECT_EQ(RequireValue(concurrent.failure).code,
            ulog::RuntimeCreateErrorCode::kInvalidDestination);
  EXPECT_NE(RequireValue(concurrent.failure).HowToFix().find("destination"),
            std::string_view::npos);

  auto shutdown = first.runtime->Shutdown();
  ExpectSucceeded(shutdown);
  first.runtime.reset();

  auto sequential = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(sequential);
  ASSERT_TRUE(sequential.failure.has_value());
  EXPECT_EQ(RequireValue(sequential.failure).code,
            ulog::RuntimeCreateErrorCode::kInvalidDestination);
}

TEST(RuntimeEncodedTracer, RuntimeRejectsDestinationWhoseRecordBoundIsTooSmall) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 256,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(created);
  ASSERT_TRUE(created.failure.has_value());
  EXPECT_EQ(RequireValue(created.failure).code, ulog::RuntimeCreateErrorCode::kInvalidDestination);
  EXPECT_NE(RequireValue(created.failure).HowToFix().find("maximum_record_bytes"),
            std::string_view::npos);
}

TEST(RuntimeEncodedTracer, DestinationConfigurationErrorsAreActionableBeforeAllocation) {
  try {
    ulog::testing::InMemoryEncodedDestination invalid{{
        .capacity_records = 0,
        .maximum_record_bytes = 512,
    }};
    static_cast<void>(invalid);
    FAIL() << "zero capacity must be rejected";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string_view{error.what()}.find("capacity_records"), std::string_view::npos);
    EXPECT_NE(std::string_view{error.what()}.find("Set"), std::string_view::npos);
  }

  try {
    ulog::testing::InMemoryEncodedDestination invalid{{
        .capacity_records = 1,
        .maximum_record_bytes = 129,
    }};
    static_cast<void>(invalid);
    FAIL() << "unaligned Record bound must be rejected";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string_view{error.what()}.find("maximum_record_bytes"), std::string_view::npos);
    EXPECT_NE(std::string_view{error.what()}.find("Set"), std::string_view::npos);
  }

  try {
    ulog::testing::InMemoryEncodedDestination invalid{{
        .capacity_records = std::numeric_limits<std::size_t>::max(),
        .maximum_record_bytes = 512,
    }};
    static_cast<void>(invalid);
    FAIL() << "fixed backing overflow must be rejected";
  } catch (const std::invalid_argument& error) {
    EXPECT_NE(std::string_view{error.what()}.find("overflows"), std::string_view::npos);
    EXPECT_NE(std::string_view{error.what()}.find("Set"), std::string_view::npos);
  }
}

TEST(RuntimeEncodedTracer, HeldDeliveryKeepsDrainPendingUntilItsSingleCompletion) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "held");
  auto drain = created.runtime->Drain();
  ASSERT_TRUE(drain);
  auto pending = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(pending.has_value());
  EXPECT_EQ(RequireValue(pending).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(pending).Bytes(), "tskv\ttext=held\n");

  const auto blocked = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 50ms);
  EXPECT_EQ(blocked.status, ulog::OperationWaitStatus::kDeadlineExceeded);
  EXPECT_FALSE(destination.TryTake().has_value());
  const auto in_flight = created.runtime->GetSnapshot();
  EXPECT_EQ(in_flight.processed_records, 1U);
  EXPECT_EQ(in_flight.processed_bytes, 15U);
  EXPECT_EQ(in_flight.completed_records, 0U);
  EXPECT_EQ(in_flight.retained_records, 0U);

  EXPECT_EQ(RequireValue(pending).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);
  EXPECT_EQ(RequireValue(pending).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kInvalidHandle);
  EXPECT_EQ(RequireValue(pending).Fail(),
            ulog::testing::EncodedDeliveryCompletionStatus::kInvalidHandle);
  EXPECT_FALSE(static_cast<bool>(RequireValue(pending)));
  EXPECT_TRUE(RequireValue(pending).Bytes().empty());

  const ulog::OperationResult drained = WaitForResult(drain);
  EXPECT_EQ(drained.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(drained.Report(), (ulog::OperationReport{.watermark_records = 1,
                                                     .processed_records = 1,
                                                     .processed_bytes = 15,
                                                     .delivered_records = 1,
                                                     .delivered_bytes = 15}));
  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(RequireValue(frame).Bytes(), "tskv\ttext=held\n");

  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(stopped.Report(), drained.Report());
}

TEST(RuntimeEncodedTracer, FailedDeliveriesAreReportedWhileTheRouteContinues) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kFailInline);
  auto created = ulog::Runtime::Create(DeferredRuntimeConfig(2), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "lost");
  auto first_drain = created.runtime->Drain();
  const ulog::OperationResult first = WaitForResult(first_drain);
  EXPECT_EQ(first.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(first.Report(), (ulog::OperationReport{.watermark_records = 1,
                                                   .processed_records = 1,
                                                   .processed_bytes = 15,
                                                   .failed_records = 1,
                                                   .failed_bytes = 15}));
  EXPECT_FALSE(destination.TryTake().has_value());

  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kCompleteInline);
  LOG_INFO_TO(logger, "kept");
  auto second_drain = created.runtime->Drain();
  const ulog::OperationResult second = WaitForResult(second_drain);
  EXPECT_EQ(second.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(second.Report(), (ulog::OperationReport{.watermark_records = 2,
                                                    .processed_records = 2,
                                                    .processed_bytes = 30,
                                                    .delivered_records = 1,
                                                    .delivered_bytes = 15,
                                                    .failed_records = 1,
                                                    .failed_bytes = 15}));
  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(RequireValue(frame).AdmissionSequence(), 1U);
  EXPECT_EQ(RequireValue(frame).Bytes(), "tskv\ttext=kept\n");

  const auto snapshot = created.runtime->GetSnapshot();
  EXPECT_EQ(snapshot.completed_records, 2U);
  EXPECT_EQ(snapshot.delivered_records, 1U);
  EXPECT_EQ(snapshot.delivery_failed_records, 1U);
  EXPECT_EQ(snapshot.delivery_failed_bytes, 15U);
  EXPECT_EQ(snapshot.encoding_failed_records, 0U);
  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeEncodedTracer, MovedDuplicateAndAbandonedCompletionsAreConsumedOnce) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto created = ulog::Runtime::Create(DeferredRuntimeConfig(2), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  LOG_INFO_TO(logger, "second");
  auto first = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  auto second = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(RequireValue(first).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(second).AdmissionSequence(), 1U);

  ulog::testing::PendingEncodedDelivery moved = std::move(RequireValue(first));
  // NOLINTNEXTLINE(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
  EXPECT_EQ(RequireValue(first).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kInvalidHandle);
  EXPECT_EQ(moved.AdmissionSequence(), 0U);
  EXPECT_EQ(moved.Fail(), ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);
  EXPECT_EQ(moved.Complete(), ulog::testing::EncodedDeliveryCompletionStatus::kInvalidHandle);
  second.reset();

  auto drain = created.runtime->Drain();
  const ulog::OperationResult drained = WaitForResult(drain);
  EXPECT_EQ(drained.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(drained.Report(), (ulog::OperationReport{.watermark_records = 2,
                                                     .processed_records = 2,
                                                     .processed_bytes = 33,
                                                     .failed_records = 2,
                                                     .failed_bytes = 33}));
  EXPECT_FALSE(destination.TryTake().has_value());
  EXPECT_FALSE(destination.TryTakePendingDelivery().has_value());
  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeEncodedTracer, OutOfOrderCompletionsRetireInAdmissionOrderPerWatermark) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 3,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto created = ulog::Runtime::Create(DeferredRuntimeConfig(3), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "r0");
  auto early_drain = created.runtime->Drain();
  LOG_INFO_TO(logger, "r1");
  LOG_INFO_TO(logger, "r2");
  auto late_drain = created.runtime->Drain();
  auto r0 = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  auto r1 = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  auto r2 = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(r0.has_value());
  ASSERT_TRUE(r1.has_value());
  ASSERT_TRUE(r2.has_value());

  EXPECT_EQ(RequireValue(r2).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);
  EXPECT_EQ(RequireValue(r1).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);
  const auto blocked = early_drain.operation.WaitUntil(std::chrono::steady_clock::now() + 50ms);
  EXPECT_EQ(blocked.status, ulog::OperationWaitStatus::kDeadlineExceeded);
  EXPECT_EQ(late_drain.operation.Poll().status, ulog::OperationPollStatus::kPending);
  EXPECT_EQ(created.runtime->GetSnapshot().completed_records, 0U);

  EXPECT_EQ(RequireValue(r0).Fail(), ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);
  const ulog::OperationResult early = WaitForResult(early_drain);
  const ulog::OperationResult late = WaitForResult(late_drain);
  EXPECT_EQ(early.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(early.Report(), (ulog::OperationReport{.watermark_records = 1,
                                                   .processed_records = 1,
                                                   .processed_bytes = 13,
                                                   .failed_records = 1,
                                                   .failed_bytes = 13}));
  EXPECT_EQ(late.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(late.Report(), (ulog::OperationReport{.watermark_records = 3,
                                                  .processed_records = 3,
                                                  .processed_bytes = 39,
                                                  .delivered_records = 2,
                                                  .delivered_bytes = 26,
                                                  .failed_records = 1,
                                                  .failed_bytes = 13}));

  auto first_frame = destination.TryTake();
  auto second_frame = destination.TryTake();
  ASSERT_TRUE(first_frame.has_value());
  ASSERT_TRUE(second_frame.has_value());
  EXPECT_EQ(RequireValue(first_frame).Bytes(), "tskv\ttext=r1\n");
  EXPECT_EQ(RequireValue(second_frame).Bytes(), "tskv\ttext=r2\n");
  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeEncodedTracer, HeldDeliveryAppliesBoundedPressureUntilCompletionReleasesIt) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  auto first = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(first.has_value());
  LOG_INFO_TO(logger, "second");
  std::size_t rejected_evaluations = 0;
  LOG_INFO_TO(logger, "third={}", ++rejected_evaluations);

  const auto pressured = created.runtime->GetSnapshot();
  EXPECT_EQ(rejected_evaluations, 0U);
  EXPECT_EQ(pressured.accepted_records, 2U);
  EXPECT_EQ(pressured.processed_records, 1U);
  EXPECT_EQ(pressured.retained_records, 1U);
  EXPECT_EQ(pressured.dropped_newest_records, 1U);
  EXPECT_FALSE(destination.TryTakePendingDelivery().has_value());

  EXPECT_EQ(RequireValue(first).Fail(), ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);
  auto second = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(RequireValue(second).AdmissionSequence(), 1U);
  EXPECT_EQ(RequireValue(second).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);

  auto drain = created.runtime->Drain();
  const ulog::OperationResult drained = WaitForResult(drain);
  EXPECT_EQ(drained.Report(), (ulog::OperationReport{.watermark_records = 2,
                                                     .processed_records = 2,
                                                     .processed_bytes = 33,
                                                     .delivered_records = 1,
                                                     .delivered_bytes = 17,
                                                     .failed_records = 1,
                                                     .failed_bytes = 16}));
  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(RequireValue(frame).Bytes(), "tskv\ttext=second\n");
  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

TEST(RuntimeEncodedTracer, ShutdownFinishesHeldDeliveryBeforeItSucceeds) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "last");
  auto pending = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(pending.has_value());
  auto shutdown = created.runtime->Shutdown();
  ASSERT_TRUE(shutdown);
  const auto blocked = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 50ms);
  EXPECT_EQ(blocked.status, ulog::OperationWaitStatus::kDeadlineExceeded);
  const auto closing = created.runtime->GetSnapshot();
  EXPECT_FALSE(closing.admission_open);
  EXPECT_TRUE(closing.worker_running);

  EXPECT_EQ(RequireValue(pending).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kCompleted);
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(stopped.Report(), (ulog::OperationReport{.watermark_records = 1,
                                                     .processed_records = 1,
                                                     .processed_bytes = 15,
                                                     .delivered_records = 1,
                                                     .delivered_bytes = 15}));
  EXPECT_FALSE(created.runtime->GetSnapshot().worker_running);
}

TEST(RuntimeEncodedTracer, DestructionCancelsHeldDeliveriesAndRejectsLateCompletion) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto created = ulog::Runtime::Create(DeferredRuntimeConfig(2), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "taken");
  LOG_INFO_TO(logger, "queued");
  auto taken = WaitForPendingDelivery(destination, std::chrono::steady_clock::now() + 1s);
  ASSERT_TRUE(taken.has_value());
  ASSERT_TRUE(WaitForProcessed(*created.runtime, 2U));
  auto drain = created.runtime->Drain();
  ASSERT_TRUE(drain);

  created.runtime.reset();
  const ulog::OperationResult cancelled = WaitForResult(drain);
  EXPECT_EQ(cancelled.Outcome(), ulog::OperationOutcome::kCancelled);
  EXPECT_EQ(cancelled.Report(), (ulog::OperationReport{.watermark_records = 2,
                                                       .processed_records = 2,
                                                       .processed_bytes = 33,
                                                       .unfinished_records = 2,
                                                       .unfinished_bytes = 33}));
  EXPECT_EQ(RequireValue(taken).AdmissionSequence(), 0U);
  EXPECT_EQ(RequireValue(taken).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kCancelled);
  EXPECT_EQ(RequireValue(taken).Complete(),
            ulog::testing::EncodedDeliveryCompletionStatus::kInvalidHandle);
  EXPECT_FALSE(destination.TryTakePendingDelivery().has_value());
  EXPECT_FALSE(destination.TryTake().has_value());
}

TEST(RuntimeEncodedTracer, CompletionCallbackReceivesTheOperationReport) {
  struct CallbackState final {
    ulog::OperationReport report{};
    std::atomic<bool> done{false};
  };
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "callback");

  CallbackState state;
  auto drain = created.runtime->Drain();
  ASSERT_TRUE(drain);
  const auto registered =
      drain.operation.OnComplete([&state](const ulog::OperationResult& result) noexcept {
        state.report = result.Report();
        state.done.store(true, std::memory_order_release);
      });
  ASSERT_EQ(registered.status, ulog::OperationCallbackStatus::kRegistered);
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!state.done.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(state.done.load(std::memory_order_acquire));
  EXPECT_EQ(state.report, (ulog::OperationReport{.watermark_records = 1,
                                                 .processed_records = 1,
                                                 .processed_bytes = 19,
                                                 .delivered_records = 1,
                                                 .delivered_bytes = 19}));
  auto shutdown = created.runtime->Shutdown();
  ExpectSucceeded(shutdown);
}

}  // namespace
