#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>

#include "control/admission_wait.hpp"
#include "control/thread_role.hpp"
#include "producer/producer_kernel.hpp"

namespace ulog::detail::producer {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr SourceLocation kSource = SourceLocation::Custom("block.cpp", "Block", 7);

BuildStatus CountBuild(void* context, RecordAppender&) {
  static_cast<std::atomic<std::size_t>*>(context)->fetch_add(1, std::memory_order_relaxed);
  return BuildStatus::kComplete;
}

/// A budget of 512 bytes with a 256-byte worst case: each small Record is charged 128 bytes, so
/// the fourth call no longer fits.
[[nodiscard]] KernelConfig BudgetConfig() {
  return KernelConfig{
      .threshold = Level::kTrace,
      .payload_capacity_bytes = 512,
      .maximum_record_bytes = 256,
      .producer_slots = 1,
      .ingress_cells = 8,
  };
}

/// A producer kernel whose blocking calls wait through an AdmissionWait, as a Runtime's do.
class WaitingKernel final {
 public:
  explicit WaitingKernel(KernelConfig config) : kernel_{WithWaiter(config, wait_)} {}
  WaitingKernel(const WaitingKernel&) = delete;
  WaitingKernel& operator=(const WaitingKernel&) = delete;
  ~WaitingKernel() {
    kernel_.CloseAdmission();
    wait_.CloseAndAwaitIdle();
  }

  [[nodiscard]] ProducerKernel& kernel() noexcept { return kernel_; }

 private:
  static KernelConfig WithWaiter(KernelConfig config, control::AdmissionWait& wait) {
    config.capacity_waiter = wait.Hooks();
    return config;
  }

  control::AdmissionWait wait_;
  ProducerKernel kernel_;
};

[[nodiscard]] PublishOutcome Publish(ProducerKernel& kernel,
                                     ProducerKernel::ProducerRegistration& producer,
                                     std::atomic<std::size_t>& evaluations) {
  return kernel
      .TryPublish(producer, Level::kInfo, kSource, BuildOperation{&evaluations, &CountBuild})
      .outcome;
}

[[nodiscard]] PublishOutcome PublishUntil(ProducerKernel& kernel,
                                          ProducerKernel::ProducerRegistration& producer,
                                          Clock::time_point deadline,
                                          std::atomic<std::size_t>& evaluations) {
  return kernel
      .TryPublishUntil(producer, Level::kInfo, deadline, kSource,
                       BuildOperation{&evaluations, &CountBuild})
      .outcome;
}

/// Publishes until a call is rejected; returns the rejection.
[[nodiscard]] PublishOutcome Fill(ProducerKernel& kernel,
                                  ProducerKernel::ProducerRegistration& producer,
                                  std::atomic<std::size_t>& evaluations) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    const PublishOutcome outcome = Publish(kernel, producer, evaluations);
    if (outcome != PublishOutcome::kAccepted) {
      return outcome;
    }
  }
  ADD_FAILURE() << "the kernel never filled";
  return PublishOutcome::kAccepted;
}

void ConsumeOne(ProducerKernel& kernel) {
  EXPECT_EQ(kernel.TryConsume(nullptr, [](void*, std::uint64_t, const RecordView&) noexcept {}),
            ConsumeStatus::kRecord);
}

TEST(BlockingAdmission, IsRefusedWithoutAWaiterAndOnEveryUlogOwnedThread) {
  ProducerKernel plain{BudgetConfig()};
  auto plain_producer = plain.TryRegisterProducer();
  ASSERT_TRUE(plain_producer);
  std::atomic<std::size_t> evaluations{0};
  EXPECT_EQ(PublishUntil(plain, plain_producer, Clock::now() + 1s, evaluations),
            PublishOutcome::kBlockingRefused);
  EXPECT_EQ(plain.GetSnapshot().rejected_blocking_refused, 1U);
  EXPECT_EQ(plain.GetSnapshot().attempted_records, 1U);

  WaitingKernel waiting{BudgetConfig()};
  ProducerKernel& kernel = waiting.kernel();
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  // Capacity is available, yet a Ulog-owned thread is refused before it claims or waits.
  for (const auto role : {control::UlogThreadRole::kWorker, control::UlogThreadRole::kIo,
                          control::UlogThreadRole::kCallback}) {
    const control::ScopedUlogThreadRole scoped{role};
    EXPECT_EQ(PublishUntil(kernel, producer, Clock::now() + 1s, evaluations),
              PublishOutcome::kBlockingRefused);
  }
  auto snapshot = kernel.GetSnapshot();
  EXPECT_EQ(snapshot.rejected_blocking_refused, 3U);
  EXPECT_EQ(snapshot.attempted_records, 3U);
  EXPECT_EQ(snapshot.accepted_records, 0U);
  EXPECT_EQ(evaluations.load(), 0U);

  EXPECT_EQ(PublishUntil(kernel, producer, Clock::now() + 1s, evaluations),
            PublishOutcome::kAccepted);
  EXPECT_EQ(evaluations.load(), 1U);
}

TEST(BlockingAdmission, AnExpiredDeadlineAdmitsExactlyLikeANonBlockingCall) {
  WaitingKernel waiting{BudgetConfig()};
  ProducerKernel& kernel = waiting.kernel();
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  std::atomic<std::size_t> evaluations{0};
  ASSERT_EQ(Fill(kernel, producer, evaluations), PublishOutcome::kBudgetExhausted);
  const std::size_t filled = evaluations.load();

  EXPECT_EQ(PublishUntil(kernel, producer, Clock::now() - 1ms, evaluations),
            PublishOutcome::kBudgetExhausted);
  EXPECT_EQ(kernel.GetSnapshot().rejected_budget, 2U);
  EXPECT_EQ(evaluations.load(), filled);

  // Under drop-oldest an expired call evicts at once, as TryPublish does.
  KernelConfig config = BudgetConfig();
  config.drop_oldest = true;
  WaitingKernel evicting{config};
  auto evicting_producer = evicting.kernel().TryRegisterProducer();
  ASSERT_TRUE(evicting_producer);
  for (int index = 0; index < 3; ++index) {
    ASSERT_EQ(Publish(evicting.kernel(), evicting_producer, evaluations),
              PublishOutcome::kAccepted);
  }
  EXPECT_EQ(PublishUntil(evicting.kernel(), evicting_producer, Clock::now() - 1ms, evaluations),
            PublishOutcome::kAccepted);
  EXPECT_EQ(evicting.kernel().GetSnapshot().evicted_records, 1U);
}

TEST(BlockingAdmission, AWaitingCallIsAdmittedOnceTheConsumerReturnsItsCredit) {
  WaitingKernel waiting{BudgetConfig()};
  ProducerKernel& kernel = waiting.kernel();
  std::atomic<std::size_t> evaluations{0};
  std::atomic<bool> registered{false};
  std::atomic<PublishOutcome> outcome{PublishOutcome::kInvalidRecord};
  std::thread producer_thread{[&] {
    auto producer = kernel.TryRegisterProducer();
    const bool filled =
        producer && Fill(kernel, producer, evaluations) == PublishOutcome::kBudgetExhausted;
    registered.store(true, std::memory_order_release);
    if (!filled) {
      ADD_FAILURE() << "the producer did not fill";
      return;
    }
    outcome.store(PublishUntil(kernel, producer, Clock::now() + 10s, evaluations));
  }};
  while (!registered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  const std::size_t filled = evaluations.load();
  std::this_thread::sleep_for(20ms);
  ConsumeOne(kernel);
  producer_thread.join();

  EXPECT_EQ(outcome.load(), PublishOutcome::kAccepted);
  EXPECT_EQ(evaluations.load(), filled + 1U);
  const auto snapshot = kernel.GetSnapshot();
  // Only the filling call was counted; the waiting attempts were not.
  EXPECT_EQ(snapshot.rejected_budget, 1U);
  EXPECT_EQ(snapshot.accepted_records, filled + 1U);
}

TEST(BlockingAdmission, AWaitingCallIsAdmittedOnceTheConsumerFreesItsLaneCell) {
  KernelConfig config = BudgetConfig();
  config.payload_capacity_bytes = 4'096;
  config.ingress_cells = 1;
  WaitingKernel waiting{config};
  ProducerKernel& kernel = waiting.kernel();
  std::atomic<std::size_t> evaluations{0};
  std::atomic<bool> registered{false};
  std::atomic<PublishOutcome> outcome{PublishOutcome::kInvalidRecord};
  std::thread producer_thread{[&] {
    auto producer = kernel.TryRegisterProducer();
    const bool filled =
        producer && Fill(kernel, producer, evaluations) == PublishOutcome::kLaneFull;
    registered.store(true, std::memory_order_release);
    if (!filled) {
      ADD_FAILURE() << "the producer did not fill";
      return;
    }
    // The farthest deadline still wakes on capacity.
    outcome.store(PublishUntil(kernel, producer, Clock::time_point::max(), evaluations));
  }};
  while (!registered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(20ms);
  ConsumeOne(kernel);
  producer_thread.join();

  EXPECT_EQ(outcome.load(), PublishOutcome::kAccepted);
  EXPECT_EQ(evaluations.load(), 2U);
  EXPECT_EQ(kernel.GetSnapshot().rejected_lane_full, 1U);
}

TEST(BlockingAdmission, AtTheDeadlineTheConfiguredPolicyDecidesOnce) {
  WaitingKernel waiting{BudgetConfig()};
  ProducerKernel& kernel = waiting.kernel();
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  std::atomic<std::size_t> evaluations{0};
  ASSERT_EQ(Fill(kernel, producer, evaluations), PublishOutcome::kBudgetExhausted);
  const std::size_t filled = evaluations.load();

  const auto started = Clock::now();
  EXPECT_EQ(PublishUntil(kernel, producer, started + 30ms, evaluations),
            PublishOutcome::kBudgetExhausted);
  EXPECT_GE(Clock::now() - started, 30ms);
  EXPECT_EQ(kernel.GetSnapshot().rejected_budget, 2U);
  EXPECT_EQ(evaluations.load(), filled);

  // Drop-oldest waits without evicting and evicts only once the deadline passed.
  KernelConfig config = BudgetConfig();
  config.drop_oldest = true;
  WaitingKernel evicting{config};
  auto evicting_producer = evicting.kernel().TryRegisterProducer();
  ASSERT_TRUE(evicting_producer);
  for (int index = 0; index < 3; ++index) {
    ASSERT_EQ(Publish(evicting.kernel(), evicting_producer, evaluations),
              PublishOutcome::kAccepted);
  }
  const auto evict_started = Clock::now();
  EXPECT_EQ(PublishUntil(evicting.kernel(), evicting_producer, evict_started + 30ms, evaluations),
            PublishOutcome::kAccepted);
  EXPECT_GE(Clock::now() - evict_started, 30ms);
  EXPECT_EQ(evicting.kernel().GetSnapshot().evicted_records, 1U);
}

TEST(BlockingAdmission, WaitingDefersAdaptiveThinning) {
  // Below the 960-byte watermark every call is kept; the seventh small Record takes the whole
  // shared budget, after which the keep ratio is zero.
  KernelConfig config = BudgetConfig();
  config.payload_capacity_bytes = 1'024;
  config.adaptive_thinning = true;
  config.thinning_low_watermark_bytes = 960;
  WaitingKernel waiting{config};
  ProducerKernel& kernel = waiting.kernel();
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  std::atomic<std::size_t> evaluations{0};
  ASSERT_EQ(Fill(kernel, producer, evaluations), PublishOutcome::kThinned);
  const std::size_t filled = evaluations.load();
  for (std::size_t index = 0; index < filled; ++index) {
    ConsumeOne(kernel);
  }
  // The consumed Records returned their credit to this producer, but the shared pool stays taken,
  // so a non-blocking call and an expired blocking call are still thinned.
  EXPECT_EQ(Publish(kernel, producer, evaluations), PublishOutcome::kThinned);
  EXPECT_EQ(PublishUntil(kernel, producer, Clock::now() - 1ms, evaluations),
            PublishOutcome::kThinned);
  EXPECT_EQ(kernel.GetSnapshot().rejected_thinned, 3U);
  // A waiting call skips thinning and reserves the returned credit at once.
  EXPECT_EQ(PublishUntil(kernel, producer, Clock::now() + 10s, evaluations),
            PublishOutcome::kAccepted);
  EXPECT_EQ(kernel.GetSnapshot().rejected_thinned, 3U);
  EXPECT_EQ(evaluations.load(), filled + 1U);
}

TEST(BlockingAdmission, ClosingAdmissionReleasesAWaitingCallAsRejected) {
  WaitingKernel waiting{BudgetConfig()};
  ProducerKernel& kernel = waiting.kernel();
  std::atomic<std::size_t> evaluations{0};
  std::atomic<bool> registered{false};
  std::atomic<PublishOutcome> outcome{PublishOutcome::kInvalidRecord};
  const auto started = Clock::now();
  std::thread producer_thread{[&] {
    auto producer = kernel.TryRegisterProducer();
    const bool filled =
        producer && Fill(kernel, producer, evaluations) == PublishOutcome::kBudgetExhausted;
    registered.store(true, std::memory_order_release);
    if (!filled) {
      ADD_FAILURE() << "the producer did not fill";
      return;
    }
    outcome.store(PublishUntil(kernel, producer, Clock::now() + 30s, evaluations));
  }};
  while (!registered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  const std::size_t filled = evaluations.load();
  std::this_thread::sleep_for(20ms);
  kernel.CloseAdmission();
  producer_thread.join();

  EXPECT_LT(Clock::now() - started, 10s);
  EXPECT_EQ(outcome.load(), PublishOutcome::kNoProducerSlot);
  EXPECT_EQ(evaluations.load(), filled);
  const auto snapshot = kernel.GetSnapshot();
  EXPECT_EQ(snapshot.rejected_budget, 1U);
  EXPECT_EQ(snapshot.rejected_no_producer, 1U);
}

TEST(BlockingAdmission, PartialWaiterHooksAreRejected) {
  KernelConfig config = BudgetConfig();
  config.capacity_waiter.may_block = [](void*) noexcept { return true; };
  EXPECT_THROW(ProducerKernel{config}, std::invalid_argument);
}

}  // namespace
}  // namespace ulog::detail::producer
