#include <gtest/gtest.h>

#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <utility>

namespace ulog::test {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kRecordBytes = 512;
constexpr Level kLevels[] = {Level::kTrace, Level::kDebug,    Level::kInfo, Level::kWarning,
                             Level::kError, Level::kCritical, Level::kNone};

void ExpectSucceeded(OperationStartResult started) {
  ASSERT_TRUE(started);
  const auto completed = started.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion.has_value());
  // NOLINTNEXTLINE(bugprone-unchecked-optional-access): asserted above.
  EXPECT_EQ(completed.completion->Outcome(), OperationOutcome::kSucceeded);
}

class RuntimeThreshold : public ::testing::Test {
 protected:
  void SetUp() override {
    auto created = Runtime::Create(
        RuntimeConfig{
            .threshold = Level::kInfo,
            .payload_capacity_bytes = 2 * kRecordBytes,
            .maximum_record_bytes = kRecordBytes,
            .producer_slots = 1,
            .ingress_cells = 2,
            .control_operations = 2,
            .worker_threads = 1,
            .startup_timeout = 1s,
            .destruction_timeout = 1s,
        },
        destination_);
    ASSERT_TRUE(created);
    runtime_ = std::move(created.runtime);
    logger_ = runtime_->GetLogger();
  }

  // Drains the Runtime and returns how many Records reached the destination.
  [[nodiscard]] std::size_t DrainAndCount() {
    ExpectSucceeded(runtime_->Drain());
    std::size_t delivered = 0;
    while (destination_.TryTake()) {
      ++delivered;
    }
    return delivered;
  }

  testing::InMemoryDestination destination_{
      {.capacity_records = 2, .maximum_record_bytes = kRecordBytes}};
  std::unique_ptr<Runtime> runtime_;
  Logger logger_;
};

// Installs a Logger as the Default Logger for one test. The Runtime outlives the test body.
class ScopedDefaultLogger final {
 public:
  explicit ScopedDefaultLogger(Logger logger) noexcept : previous_(ExchangeDefaultLogger(logger)) {}
  ScopedDefaultLogger(const ScopedDefaultLogger&) = delete;
  ScopedDefaultLogger& operator=(const ScopedDefaultLogger&) = delete;
  ~ScopedDefaultLogger() { static_cast<void>(ExchangeDefaultLogger(previous_)); }

 private:
  Logger previous_;
};

TEST_F(RuntimeThreshold, ChangesAreVisibleToLaterCallsAndQueries) {
  EXPECT_EQ(logger_.GetLevel(), Level::kInfo);
  EXPECT_EQ(logger_.SetLevel(Level::kError), LevelChangeStatus::kChanged);
  EXPECT_EQ(logger_.GetLevel(), Level::kError);
  EXPECT_FALSE(logger_.ShouldLog(Level::kWarning));
  EXPECT_TRUE(logger_.ShouldLog(Level::kError));

  int evaluations = 0;
  LOG_WARNING_TO(logger_, "suppressed {}", ++evaluations);
  LOG_ERROR_TO(logger_, "delivered {}", ++evaluations);
  EXPECT_EQ(evaluations, 1);
  EXPECT_EQ(DrainAndCount(), 1U);

  EXPECT_EQ(logger_.SetLevel(Level::kTrace), LevelChangeStatus::kChanged);
  LOG_TRACE_TO(logger_, "trace {}", ++evaluations);
  EXPECT_EQ(evaluations, 2);
  EXPECT_EQ(DrainAndCount(), 1U);
}

TEST_F(RuntimeThreshold, RejectsInvalidLevelsAndChangesAfterShutdown) {
  EXPECT_EQ(logger_.SetLevel(std::bit_cast<Level>(std::uint8_t{7})),
            LevelChangeStatus::kInvalidLevel);
  EXPECT_EQ(logger_.GetLevel(), Level::kInfo);

  ExpectSucceeded(runtime_->Shutdown());
  EXPECT_EQ(logger_.GetLevel(), Level::kNone);
  EXPECT_EQ(logger_.SetLevel(Level::kTrace), LevelChangeStatus::kClosed);
  EXPECT_EQ(logger_.GetLevel(), Level::kNone);
}

TEST(NullLoggerThreshold, RejectsEveryLevelChange) {
  const Logger null_logger = GetNullLogger();
  for (const Level level : kLevels) {
    SCOPED_TRACE(ToString(level));
    EXPECT_EQ(null_logger.SetLevel(level), LevelChangeStatus::kNullLogger);
    EXPECT_EQ(null_logger.GetLevel(), Level::kNone);
  }
  EXPECT_EQ(null_logger.SetLevel(std::bit_cast<Level>(std::uint8_t{9})),
            LevelChangeStatus::kInvalidLevel);
}

TEST_F(RuntimeThreshold, DefaultLevelScopeRestoresThePreviousThreshold) {
  const ScopedDefaultLogger installed{logger_};
  {
    const DefaultLoggerLevelScope outer{Level::kWarning};
    EXPECT_EQ(outer.Status(), LevelChangeStatus::kChanged);
    EXPECT_EQ(outer.Target(), logger_);
    EXPECT_EQ(GetDefaultLogger().GetLevel(), Level::kWarning);
    {
      const DefaultLoggerLevelScope inner{Level::kNone};
      EXPECT_EQ(logger_.GetLevel(), Level::kNone);
    }
    EXPECT_EQ(logger_.GetLevel(), Level::kWarning);
  }
  EXPECT_EQ(logger_.GetLevel(), Level::kInfo);

  Level level_during_unwinding = Level::kNone;
  try {
    const DefaultLoggerLevelScope scope{Level::kCritical};
    EXPECT_EQ(logger_.GetLevel(), Level::kCritical);
    throw std::runtime_error{"unwind"};
  } catch (const std::runtime_error&) {
    level_during_unwinding = logger_.GetLevel();
  }
  EXPECT_EQ(level_during_unwinding, Level::kInfo);
  EXPECT_EQ(logger_.GetLevel(), Level::kInfo);
}

TEST_F(RuntimeThreshold, DefaultLevelScopeRestoresTheCapturedTargetAfterAnExchange) {
  const ScopedDefaultLogger installed{logger_};
  {
    const DefaultLoggerLevelScope scope{Level::kError};
    const ScopedDefaultLogger replaced{GetNullLogger()};
    EXPECT_EQ(logger_.GetLevel(), Level::kError);
  }
  EXPECT_EQ(logger_.GetLevel(), Level::kInfo);
  EXPECT_EQ(GetDefaultLogger(), logger_);
}

TEST(NullLoggerThreshold, DefaultLevelScopeOnTheNullLoggerChangesNothing) {
  const ScopedDefaultLogger installed{GetNullLogger()};
  const DefaultLoggerLevelScope scope{Level::kTrace};
  EXPECT_EQ(scope.Status(), LevelChangeStatus::kNullLogger);
  EXPECT_EQ(GetDefaultLogger().GetLevel(), Level::kNone);
}

// runtime-filter-threshold-matrix: every threshold x message level, for an explicit and the
// default target, through preflight, generic and named macros, and delivered Records.
void LogNamed(Logger target, Level level, int& evaluations) {
  switch (level) {
    case Level::kTrace:
      LOG_TRACE_TO(target, "{}", ++evaluations);
      break;
    case Level::kDebug:
      LOG_DEBUG_TO(target, "{}", ++evaluations);
      break;
    case Level::kInfo:
      LOG_INFO_TO(target, "{}", ++evaluations);
      break;
    case Level::kWarning:
      LOG_WARNING_TO(target, "{}", ++evaluations);
      break;
    case Level::kError:
      LOG_ERROR_TO(target, "{}", ++evaluations);
      break;
    case Level::kCritical:
      LOG_CRITICAL_TO(target, "{}", ++evaluations);
      break;
    case Level::kNone:
      break;
  }
}

void LogNamedDefault(Level level, int& evaluations) {
  switch (level) {
    case Level::kTrace:
      LOG_TRACE("{}", ++evaluations);
      break;
    case Level::kDebug:
      LOG_DEBUG("{}", ++evaluations);
      break;
    case Level::kInfo:
      LOG_INFO("{}", ++evaluations);
      break;
    case Level::kWarning:
      LOG_WARNING("{}", ++evaluations);
      break;
    case Level::kError:
      LOG_ERROR("{}", ++evaluations);
      break;
    case Level::kCritical:
      LOG_CRITICAL("{}", ++evaluations);
      break;
    case Level::kNone:
      break;
  }
}

TEST_F(RuntimeThreshold, FilterThresholdMatrixAgreesAcrossPreflightMacrosAndDelivery) {
  for (const bool use_default : {false, true}) {
    const ScopedDefaultLogger installed{use_default ? logger_ : GetNullLogger()};
    for (const Level threshold : kLevels) {
      ASSERT_EQ(logger_.SetLevel(threshold), LevelChangeStatus::kChanged);
      for (const Level message : kLevels) {
        SCOPED_TRACE(std::string{use_default ? "default " : "explicit "} +
                     std::string{ToString(threshold)} + " x " + std::string{ToString(message)});
        const bool expected =
            message != Level::kNone && threshold != Level::kNone && message >= threshold;
        const Logger target = use_default ? GetDefaultLogger() : logger_;
        EXPECT_EQ(target.ShouldLog(message), expected);

        int evaluations = 0;
        if (use_default) {
          LOG(message, "{}", ++evaluations);
          LogNamedDefault(message, evaluations);
        } else {
          LOG_TO(logger_, message, "{}", ++evaluations);
          LogNamed(logger_, message, evaluations);
        }
        const int calls = message == Level::kNone ? 1 : 2;
        EXPECT_EQ(evaluations, expected ? calls : 0);
        EXPECT_EQ(DrainAndCount(), expected ? static_cast<std::size_t>(calls) : 0U);
      }
    }
  }
  // Critical is filtered like any other level at a None threshold.
  ASSERT_EQ(logger_.SetLevel(Level::kNone), LevelChangeStatus::kChanged);
  EXPECT_FALSE(logger_.ShouldLog(Level::kCritical));
}

}  // namespace
}  // namespace ulog::test
