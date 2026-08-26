#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <latch>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

#include "logger_state.hpp"
#include "producer/producer_kernel.hpp"

namespace ulog::test {

Logger LoadDefaultLoggerFromAnotherTranslationUnit() noexcept;

namespace {

namespace producer = detail::producer;

struct CallGate final {
  std::latch entered{1};
  std::latch release{1};
};

class StableTarget final {
 public:
  StableTarget()
      : state_{std::atomic<std::uint8_t>{static_cast<std::uint8_t>(Level::kInfo)}, &kOperations,
               this} {}

  [[nodiscard]] Logger GetLogger() const noexcept {
    return detail::LoggerAccess::FromState(&state_);
  }

 private:
  static void DiscardText(void*, Level, const SourceLocation&, void*, detail::TextBuilder) {}

  inline static constexpr detail::ProducerOperations kOperations{&DiscardText};

  detail::LoggerState state_;
};

StableTarget& FirstTarget() {
  static StableTarget target;
  return target;
}

StableTarget& SecondTarget() {
  static StableTarget target;
  return target;
}

[[nodiscard]] producer::KernelConfig DefaultLoggerKernelConfig() {
  return {
      .threshold = Level::kInfo,
      .payload_capacity_bytes = 512,
      .maximum_record_bytes = 256,
      .producer_slots = 1,
      .ingress_cells = 2,
  };
}

producer::ProducerKernel& FirstKernel() {
  static producer::ProducerKernel kernel{DefaultLoggerKernelConfig()};
  return kernel;
}

producer::ProducerKernel& SecondKernel() {
  static producer::ProducerKernel kernel{DefaultLoggerKernelConfig()};
  return kernel;
}

struct RecordObservation final {
  std::string_view expected_message;
  std::size_t matches{0};
  std::size_t mismatches{0};
};

void ObserveRecord(void* context, std::uint64_t, const producer::RecordView& record) noexcept {
  auto& observation = *static_cast<RecordObservation*>(context);
  if (record.message() == observation.expected_message) {
    ++observation.matches;
  } else {
    ++observation.mismatches;
  }
}

class DefaultLoggerExchangeTest : public ::testing::Test {
 protected:
  void SetUp() override { original_ = ExchangeDefaultLogger(GetNullLogger()); }

  void TearDown() override { static_cast<void>(ExchangeDefaultLogger(original_)); }

 private:
  Logger original_;
};

TEST_F(DefaultLoggerExchangeTest, ReturnsPreviousTargetAndChangesSubsequentLoads) {
  const Logger first = FirstTarget().GetLogger();
  const Logger second = SecondTarget().GetLogger();

  EXPECT_EQ(ExchangeDefaultLogger(first), GetNullLogger());
  EXPECT_EQ(GetDefaultLogger(), first);
  EXPECT_EQ(LoadDefaultLoggerFromAnotherTranslationUnit(), first);
  EXPECT_EQ(ExchangeDefaultLogger(second), first);
  EXPECT_EQ(GetDefaultLogger(), second);
}

TEST_F(DefaultLoggerExchangeTest, DoesNotWaitAndStaleCallsFinishWithoutForwarding) {
  using namespace std::chrono_literals;

  producer::ProducerKernel& first_kernel = FirstKernel();
  producer::ProducerKernel& second_kernel = SecondKernel();
  const Logger first = first_kernel.GetLogger();
  const Logger second = second_kernel.GetLogger();
  CallGate gate;
  static_cast<void>(ExchangeDefaultLogger(first));

  std::size_t message_evaluations = 0;
  bool registration_failed = false;
  std::latch producer_ready{1};
  std::thread stale_producer{[&] {
    auto first_registration = first_kernel.TryRegisterProducer();
    auto second_registration = second_kernel.TryRegisterProducer();
    registration_failed = !first_registration || !second_registration;
    producer_ready.count_down();
    if (registration_failed) {
      return;
    }

    const Logger stale = GetDefaultLogger();
    const SourceLocation source = SourceLocation::Custom("test.cpp", "Producer", 1);
    stale.Log<Level::kInfo>(source, [&]() -> std::string_view {
      gate.entered.count_down();
      gate.release.wait();
      ++message_evaluations;
      return "stale call";
    });
    GetDefaultLogger().Log<Level::kInfo>(source, [&]() -> std::string_view {
      ++message_evaluations;
      return "new call";
    });
  }};
  producer_ready.wait();
  if (registration_failed) {
    stale_producer.join();
    FAIL() << "failed to register the deterministic Default Logger producers";
    return;
  }
  gate.entered.wait();

  std::promise<Logger> exchange_result;
  std::future<Logger> previous = exchange_result.get_future();
  std::latch exchange_started{1};
  std::thread exchanger{[&] {
    exchange_started.count_down();
    exchange_result.set_value(ExchangeDefaultLogger(second));
  }};
  exchange_started.wait();
  const bool exchange_completed_without_wait = previous.wait_for(1s) == std::future_status::ready;

  gate.release.count_down();
  stale_producer.join();
  exchanger.join();

  EXPECT_TRUE(exchange_completed_without_wait);
  EXPECT_EQ(previous.get(), first);
  EXPECT_EQ(message_evaluations, 2U);

  RecordObservation first_record{"stale call"};
  RecordObservation second_record{"new call"};
  EXPECT_EQ(first_kernel.TryConsume(&first_record, &ObserveRecord),
            producer::ConsumeStatus::kRecord);
  EXPECT_EQ(second_kernel.TryConsume(&second_record, &ObserveRecord),
            producer::ConsumeStatus::kRecord);
  EXPECT_EQ(first_record.matches, 1U);
  EXPECT_EQ(first_record.mismatches, 0U);
  EXPECT_EQ(second_record.matches, 1U);
  EXPECT_EQ(second_record.mismatches, 0U);
  EXPECT_EQ(first_kernel.TryConsume(&first_record, &ObserveRecord),
            producer::ConsumeStatus::kEmpty);
  EXPECT_EQ(second_kernel.TryConsume(&second_record, &ObserveRecord),
            producer::ConsumeStatus::kEmpty);
}

}  // namespace
}  // namespace ulog::test
