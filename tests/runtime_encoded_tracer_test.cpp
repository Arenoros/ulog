#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;

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
  EXPECT_EQ(completed.completion->Outcome(), ulog::OperationOutcome::kSucceeded);
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

TEST(RuntimeEncodedTracer, PublicLoggerDeliversBaselineRawFrame) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << created.failure->Message();

  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "hello");

  auto shutdown = created.runtime->Shutdown();
  ASSERT_FALSE(shutdown.failure.has_value());
  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, ulog::OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion.has_value());
  EXPECT_EQ(completed.completion->Outcome(), ulog::OperationOutcome::kSucceeded);

  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->AdmissionSequence(), 0U);
  EXPECT_EQ(frame->Bytes(), "tskv\ttext=hello\n");
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
  ASSERT_TRUE(created) << created.failure->Message();

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
  EXPECT_EQ(completed.completion->Outcome(), ulog::OperationOutcome::kSucceeded);

  auto empty = destination.TryTake();
  auto controls = destination.TryTake();
  ASSERT_TRUE(empty.has_value());
  ASSERT_TRUE(controls.has_value());
  EXPECT_EQ(empty->AdmissionSequence(), 0U);
  EXPECT_EQ(empty->Bytes(), "tskv\ttext=\n");
  EXPECT_EQ(controls->AdmissionSequence(), 1U);
  EXPECT_EQ(controls->Bytes(), kExpectedFrame);
}

TEST(RuntimeEncodedTracer, TruncationMarkerIsEncodedBeforeTheRetainedMessagePrefix) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_TRUE(created) << created.failure->Message();
  const ulog::Logger logger = created.runtime->GetLogger();
  const std::string oversized(1'024, 'x');

  LOG_INFO_TO(logger, "{}", oversized);
  auto shutdown = created.runtime->Shutdown();
  ASSERT_TRUE(shutdown);
  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, ulog::OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion.has_value());
  ASSERT_EQ(completed.completion->Outcome(), ulog::OperationOutcome::kSucceeded);

  auto frame = destination.TryTake();
  ASSERT_TRUE(frame.has_value());
  const std::string_view encoded = frame->Bytes();
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
  ASSERT_TRUE(created) << created.failure->Message();
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
  EXPECT_EQ(first->AdmissionSequence(), 0U);
  EXPECT_EQ(first->Bytes(), "tskv\ttext=first\n");
  const std::string_view pinned_bytes = first->Bytes();
  const auto blocked = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 50ms);
  EXPECT_EQ(blocked.status, ulog::OperationWaitStatus::kDeadlineExceeded);
  EXPECT_EQ(first->Bytes(), pinned_bytes);

  first.reset();
  ExpectSucceeded(drain);
  auto second = destination.TryTake();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->AdmissionSequence(), 1U);
  EXPECT_EQ(second->Bytes(), "tskv\ttext=second\n");

  const auto drained = created.runtime->GetSnapshot();
  EXPECT_EQ(drained.completed_records, 2U);
  EXPECT_EQ(drained.delivered_records, 2U);
  EXPECT_EQ(drained.delivered_bytes, 33U);
  EXPECT_EQ(drained.encoding_failed_records, 0U);
  EXPECT_EQ(drained.retained_records, 0U);

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
    ASSERT_TRUE(created) << created.failure->Message();
    const ulog::Logger logger = created.runtime->GetLogger();
    LOG_INFO_TO(logger, "survives");
    auto shutdown = created.runtime->Shutdown();
    ExpectSucceeded(shutdown);
    observed = destination.TryTake();
    ASSERT_TRUE(observed.has_value());
    created.runtime.reset();
  }

  EXPECT_EQ(observed->AdmissionSequence(), 0U);
  EXPECT_EQ(observed->Bytes(), "tskv\ttext=survives\n");
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
  ASSERT_TRUE(first) << first.failure->Message();
  auto concurrent = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(concurrent);
  ASSERT_TRUE(concurrent.failure.has_value());
  EXPECT_EQ(concurrent.failure->code, ulog::RuntimeCreateErrorCode::kInvalidDestination);
  EXPECT_NE(concurrent.failure->HowToFix().find("destination"), std::string_view::npos);

  auto shutdown = first.runtime->Shutdown();
  ExpectSucceeded(shutdown);
  first.runtime.reset();

  auto sequential = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(sequential);
  ASSERT_TRUE(sequential.failure.has_value());
  EXPECT_EQ(sequential.failure->code, ulog::RuntimeCreateErrorCode::kInvalidDestination);
}

TEST(RuntimeEncodedTracer, RuntimeRejectsDestinationWhoseRecordBoundIsTooSmall) {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 256,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  ASSERT_FALSE(created);
  ASSERT_TRUE(created.failure.has_value());
  EXPECT_EQ(created.failure->code, ulog::RuntimeCreateErrorCode::kInvalidDestination);
  EXPECT_NE(created.failure->HowToFix().find("maximum_record_bytes"), std::string_view::npos);
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

}  // namespace
