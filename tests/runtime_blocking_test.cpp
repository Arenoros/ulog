#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <utility>

namespace ulog::test {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr SourceLocation kSource = SourceLocation::Custom("blocking.cpp", "Blocking", 1);

/// One ingress cell and a budget of one worst-case Record: a second call finds no capacity until
/// the worker hands the first Record to the paused destination.
[[nodiscard]] RuntimeConfig FullAfterOneRecord() {
  return RuntimeConfig{
      .threshold = Level::kTrace,
      .payload_capacity_bytes = 512,
      .maximum_record_bytes = 512,
      .producer_slots = 1,
      .ingress_cells = 1,
      .control_operations = 2,
      .startup_timeout = 1s,
      .destruction_timeout = 1s,
  };
}

[[nodiscard]] testing::InMemoryDestination PausedDestination() {
  return testing::InMemoryDestination{{
      .capacity_records = 2,
      .maximum_record_bytes = 512,
      .start_paused = true,
  }};
}

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

void ExpectSucceeded(OperationStartResult& started) {
  ASSERT_TRUE(started) << (started.failure ? started.failure.value().Message()
                                           : "missing Operation");
  const auto result = started.operation.WaitUntil(Clock::now() + 5s);
  ASSERT_EQ(result.status, OperationWaitStatus::kCompleted) << result.Message();
  EXPECT_EQ(RequireValue(result.completion).Outcome(), OperationOutcome::kSucceeded);
}

TEST(RuntimeBlocking, LogUntilWaitsForTheWorkerAndIsDeliveredInOrder) {
  auto destination = PausedDestination();
  auto created = Runtime::Create(FullAfterOneRecord(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  LOG_INFO_TO(logger, "first");
  std::size_t evaluations = 0;
  std::thread resume{[&] {
    std::this_thread::sleep_for(50ms);
    destination.Resume();
  }};
  logger.LogUntil<Level::kInfo>(Clock::now() + 10s, kSource, [&] {
    ++evaluations;
    return std::string_view{"waited"};
  });
  resume.join();

  EXPECT_EQ(evaluations, 1U);
  const RuntimeSnapshot snapshot = runtime.GetSnapshot();
  EXPECT_EQ(snapshot.accepted_records, 2U);
  EXPECT_EQ(snapshot.dropped_newest_records, 0U);
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
  auto first = destination.TryTake();
  auto second = destination.TryTake();
  EXPECT_EQ(RequireValue(first).Message(), "first");
  EXPECT_EQ(RequireValue(second).Message(), "waited");
  EXPECT_EQ(RequireValue(second).AdmissionSequence(), 1U);
}

TEST(RuntimeBlocking, AnExpiredOrPassedDeadlineDropsTheNewestCallUnevaluated) {
  auto destination = PausedDestination();
  auto created = Runtime::Create(FullAfterOneRecord(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();
  LOG_INFO_TO(logger, "first");

  std::size_t evaluations = 0;
  const auto count = [&] {
    ++evaluations;
    return std::string_view{"dropped"};
  };
  logger.LogUntil<Level::kInfo>(Clock::now() - 1ms, kSource, count);
  EXPECT_EQ(runtime.GetSnapshot().dropped_newest_records, 1U);

  const auto started = Clock::now();
  logger.LogUntil<Level::kInfo>(started + 50ms, kSource, count);
  EXPECT_GE(Clock::now() - started, 50ms);
  const RuntimeSnapshot snapshot = runtime.GetSnapshot();
  EXPECT_EQ(snapshot.dropped_newest_records, 2U);
  EXPECT_EQ(snapshot.rejected_lane_full, 2U);
  EXPECT_EQ(snapshot.accepted_records, 1U);
  EXPECT_EQ(evaluations, 0U);
  destination.Resume();
}

TEST(RuntimeBlocking, ShutdownReleasesAWaitingCallAsRejected) {
  auto destination = PausedDestination();
  auto created = Runtime::Create(FullAfterOneRecord(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();
  LOG_INFO_TO(logger, "first");

  std::optional<OperationStartResult> shutdown;
  std::thread stopper{[&] {
    std::this_thread::sleep_for(50ms);
    shutdown.emplace(runtime.Shutdown());
  }};
  std::size_t evaluations = 0;
  const auto started = Clock::now();
  logger.LogUntil<Level::kInfo>(started + 30s, kSource, [&] {
    ++evaluations;
    return std::string_view{"released"};
  });
  const auto waited = Clock::now() - started;
  stopper.join();

  EXPECT_LT(waited, 10s);
  EXPECT_EQ(evaluations, 0U);
  const RuntimeSnapshot snapshot = runtime.GetSnapshot();
  EXPECT_EQ(snapshot.rejected_no_producer, 1U);
  EXPECT_EQ(snapshot.dropped_newest_records, 1U);
  EXPECT_EQ(snapshot.accepted_records, 1U);
  destination.Resume();
  ExpectSucceeded(RequireValue(shutdown));
}

TEST(RuntimeBlocking, DestructionReleasesAWaitingCallPromptly) {
  auto destination = PausedDestination();
  auto created = Runtime::Create(FullAfterOneRecord(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  std::unique_ptr<Runtime> runtime = std::move(created.runtime);
  const Logger logger = runtime->GetLogger();
  LOG_INFO_TO(logger, "first");

  std::thread destroyer{[&] {
    std::this_thread::sleep_for(50ms);
    runtime.reset();
  }};
  std::size_t evaluations = 0;
  const auto started = Clock::now();
  logger.LogUntil<Level::kInfo>(started + 30s, kSource, [&] {
    ++evaluations;
    return std::string_view{"released"};
  });
  const auto waited = Clock::now() - started;
  destroyer.join();

  EXPECT_LT(waited, 10s);
  EXPECT_EQ(evaluations, 0U);
  EXPECT_FALSE(runtime);
}

struct CallbackLog final {
  Logger logger;
  std::atomic<std::size_t>* evaluations;
  std::binary_semaphore* done;

  void operator()(const OperationResult&) noexcept {
    logger.LogUntil<Level::kInfo>(Clock::now() + 10s, kSource, [this] {
      evaluations->fetch_add(1, std::memory_order_relaxed);
      return std::string_view{"from-callback"};
    });
    done->release();
  }
};

TEST(RuntimeBlocking, AnOperationCallbackIsRefusedWithoutWaiting) {
  testing::InMemoryDestination destination{{.capacity_records = 2, .maximum_record_bytes = 512}};
  auto created = Runtime::Create(FullAfterOneRecord(), destination);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  Runtime& runtime = *created.runtime;
  const Logger logger = runtime.GetLogger();

  std::atomic<std::size_t> evaluations{0};
  std::binary_semaphore done{0};
  auto drain = runtime.Drain();
  ASSERT_TRUE(drain);
  const auto registered = drain.operation.OnComplete(CallbackLog{logger, &evaluations, &done});
  EXPECT_EQ(registered.status, OperationCallbackStatus::kRegistered);
  ASSERT_TRUE(done.try_acquire_for(5s));

  const RuntimeSnapshot snapshot = runtime.GetSnapshot();
  EXPECT_EQ(snapshot.rejected_blocking_refused, 1U);
  EXPECT_EQ(snapshot.dropped_newest_records, 1U);
  EXPECT_EQ(snapshot.accepted_records, 0U);
  EXPECT_EQ(evaluations.load(), 0U);
  auto shutdown = runtime.Shutdown();
  ExpectSucceeded(shutdown);
}

}  // namespace
}  // namespace ulog::test
