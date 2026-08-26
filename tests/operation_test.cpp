#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <latch>
#include <semaphore>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/source_location.hpp>
#include <utility>

#include "control/control_reserve.hpp"
#include "control/thread_role.hpp"
#include "producer/producer_kernel.hpp"

namespace ulog::detail::control::test {
namespace {

constexpr auto kTestDeadline = std::chrono::seconds{1};

[[nodiscard]] bool WaitForIdle(ControlReserve& reserve) {
  const auto deadline = std::chrono::steady_clock::now() + kTestDeadline;
  while (reserve.GetSnapshot().in_use != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return reserve.GetSnapshot().in_use == 0;
}

struct ReentrantMoveCallback final {
  Operation* operation{nullptr};
  std::size_t* moves{nullptr};
  const std::atomic<std::size_t>* reenter_at_move{nullptr};
  OperationCallbackStatus* reentrant_status{nullptr};
  std::binary_semaphore* invoked{nullptr};

  ReentrantMoveCallback() = default;
  ReentrantMoveCallback(Operation& target_operation, std::size_t& move_count,
                        const std::atomic<std::size_t>& reentrant_move,
                        OperationCallbackStatus& observed_reentrant_status,
                        std::binary_semaphore& callback_invoked) noexcept
      : operation(&target_operation),
        moves(&move_count),
        reenter_at_move(&reentrant_move),
        reentrant_status(&observed_reentrant_status),
        invoked(&callback_invoked) {}
  ReentrantMoveCallback(const ReentrantMoveCallback&) = delete;
  ReentrantMoveCallback& operator=(const ReentrantMoveCallback&) = delete;
  ReentrantMoveCallback& operator=(ReentrantMoveCallback&&) = delete;
  ReentrantMoveCallback(ReentrantMoveCallback&& other) noexcept
      : operation(other.operation),
        moves(other.moves),
        reenter_at_move(other.reenter_at_move),
        reentrant_status(other.reentrant_status),
        invoked(other.invoked) {
    ++*moves;
    const auto move_to_reenter = reenter_at_move->load(std::memory_order_relaxed);
    if (move_to_reenter != 0 && *moves == move_to_reenter) {
      *reentrant_status = operation->OnComplete([](const OperationResult&) noexcept {}).status;
    }
  }

  void operator()(const OperationResult&) noexcept { invoked->release(); }
};

struct ReentrantReleasingMoveCallback final {
  Operation* operation{nullptr};
  OperationCompletion* completion{nullptr};
  std::size_t* moves{nullptr};
  std::atomic<std::size_t>* reenter_at_move{nullptr};
  bool* completion_won{nullptr};
  std::binary_semaphore* invoked{nullptr};

  ReentrantReleasingMoveCallback() = default;
  ReentrantReleasingMoveCallback(Operation& target_operation,
                                 OperationCompletion& target_completion, std::size_t& move_count,
                                 std::atomic<std::size_t>& reentrant_move,
                                 bool& observed_completion,
                                 std::binary_semaphore& callback_invoked) noexcept
      : operation(&target_operation),
        completion(&target_completion),
        moves(&move_count),
        reenter_at_move(&reentrant_move),
        completion_won(&observed_completion),
        invoked(&callback_invoked) {}
  ReentrantReleasingMoveCallback(const ReentrantReleasingMoveCallback&) = delete;
  ReentrantReleasingMoveCallback& operator=(const ReentrantReleasingMoveCallback&) = delete;
  ReentrantReleasingMoveCallback& operator=(ReentrantReleasingMoveCallback&&) = delete;
  ReentrantReleasingMoveCallback(ReentrantReleasingMoveCallback&& other) noexcept
      : operation(other.operation),
        completion(other.completion),
        moves(other.moves),
        reenter_at_move(other.reenter_at_move),
        completion_won(other.completion_won),
        invoked(other.invoked) {
    ++*moves;
    if (reenter_at_move->load(std::memory_order_relaxed) == *moves) {
      reenter_at_move->store(0, std::memory_order_relaxed);
      *completion_won = completion->TryComplete(OperationOutcome::kSucceeded);
      *operation = {};
    }
  }

  void operator()(const OperationResult&) noexcept { invoked->release(); }
};

struct ReentrantDestructorCallback final {
  Operation* operation{nullptr};
  OperationCallbackStatus* reentrant_status{nullptr};
  std::binary_semaphore* destroyed{nullptr};
  bool armed{false};

  ReentrantDestructorCallback(Operation& target_operation,
                              OperationCallbackStatus& observed_reentrant_status,
                              std::binary_semaphore& callback_destroyed) noexcept
      : operation(&target_operation),
        reentrant_status(&observed_reentrant_status),
        destroyed(&callback_destroyed) {}
  ReentrantDestructorCallback(const ReentrantDestructorCallback&) = delete;
  ReentrantDestructorCallback& operator=(const ReentrantDestructorCallback&) = delete;
  ReentrantDestructorCallback& operator=(ReentrantDestructorCallback&&) = delete;
  ReentrantDestructorCallback(ReentrantDestructorCallback&& other) noexcept
      : operation(other.operation),
        reentrant_status(other.reentrant_status),
        destroyed(other.destroyed),
        armed(std::exchange(other.armed, false)) {}
  ~ReentrantDestructorCallback() noexcept {
    if (armed) {
      *reentrant_status = operation->OnComplete([](const OperationResult&) noexcept {}).status;
      destroyed->release();
    }
  }

  void operator()(const OperationResult&) noexcept { armed = true; }
};

TEST(Operation, PendingOperationBecomesCompletedForPollingAndWaiting) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  Operation operation = std::move(started.operation);

  const auto pending = operation.Poll();
  EXPECT_EQ(pending.status, OperationPollStatus::kPending);
  EXPECT_FALSE(pending.completion);

  const auto timed_out = operation.WaitUntil(std::chrono::steady_clock::now());
  EXPECT_EQ(timed_out.status, OperationWaitStatus::kDeadlineExceeded);
  EXPECT_FALSE(timed_out.completion);
  EXPECT_FALSE(timed_out.Message().empty());
  EXPECT_NE(timed_out.HowToFix().find("still active"), std::string_view::npos);

  EXPECT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));

  const auto completed = operation.Poll();
  EXPECT_EQ(completed.status, OperationPollStatus::kCompleted);
  ASSERT_TRUE(completed.completion);
  EXPECT_EQ(completed.completion.value_or(OperationResult{}).Outcome(),
            OperationOutcome::kSucceeded);

  const auto waited = operation.WaitUntil(std::chrono::steady_clock::now());
  EXPECT_EQ(waited.status, OperationWaitStatus::kCompleted);
  ASSERT_TRUE(waited.completion);
  EXPECT_EQ(waited.completion.value_or(OperationResult{}).Outcome(), OperationOutcome::kSucceeded);
}

TEST(Operation, CompletionDispatchesTheSingleCallbackAwayFromCompletingThread) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);

  std::latch callback_entered{1};
  std::binary_semaphore release_callback{0};
  std::binary_semaphore completion_returned{0};
  std::thread::id completing_thread;
  std::thread::id callback_thread;
  OperationResult callback_result;
  std::size_t callback_calls = 0;

  const auto registered = started.operation.OnComplete([&](const OperationResult& result) noexcept {
    callback_thread = std::this_thread::get_id();
    callback_result = result;
    ++callback_calls;
    callback_entered.count_down();
    release_callback.acquire();
  });
  ASSERT_EQ(registered.status, OperationCallbackStatus::kRegistered);

  const auto duplicate = started.operation.OnComplete([](const OperationResult&) noexcept {});
  EXPECT_EQ(duplicate.status, OperationCallbackStatus::kAlreadyRegistered);

  std::thread completer{[&] {
    completing_thread = std::this_thread::get_id();
    EXPECT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));
    completion_returned.release();
  }};

  callback_entered.wait();
  const bool completion_was_offloaded =
      completion_returned.try_acquire_for(std::chrono::seconds{1});
  release_callback.release();
  completer.join();

  EXPECT_TRUE(completion_was_offloaded);
  EXPECT_NE(callback_thread, completing_thread);
  EXPECT_EQ(callback_calls, 1U);
  EXPECT_EQ(callback_result.Outcome(), OperationOutcome::kSucceeded);
}

TEST(Operation, CompletionOnlyQueuesWorkOnTheInjectedDispatcher) {
  ManualCallbackDispatcher dispatcher;
  ControlReserve reserve{1, dispatcher.GetDispatcher()};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  std::size_t callback_calls = 0;

  ASSERT_EQ(started.operation.OnComplete([&](const OperationResult&) noexcept { ++callback_calls; })
                .status,
            OperationCallbackStatus::kRegistered);
  ASSERT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));
  EXPECT_EQ(callback_calls, 0U);

  EXPECT_TRUE(dispatcher.RunOne());
  EXPECT_EQ(callback_calls, 1U);
  EXPECT_FALSE(dispatcher.RunOne());
}

TEST(Operation, CallbackRegisteredAfterCompletionIsStillDispatchedAsynchronously) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  ASSERT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));

  const std::thread::id registering_thread = std::this_thread::get_id();
  std::thread::id callback_thread;
  OperationOutcome callback_outcome = OperationOutcome::kFailed;
  std::binary_semaphore callback_finished{0};
  const auto registered = started.operation.OnComplete([&](const OperationResult& result) noexcept {
    callback_thread = std::this_thread::get_id();
    callback_outcome = result.Outcome();
    callback_finished.release();
  });

  ASSERT_EQ(registered.status, OperationCallbackStatus::kRegistered);
  ASSERT_TRUE(callback_finished.try_acquire_for(kTestDeadline));
  EXPECT_NE(callback_thread, registering_thread);
  EXPECT_EQ(callback_outcome, OperationOutcome::kSucceeded);
}

TEST(Operation, CallbackMoveMayReenterWithoutHoldingTheStateLock) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  std::size_t moves = 0;
  std::atomic<std::size_t> reenter_at_move{0};
  OperationCallbackStatus reentrant_status = OperationCallbackStatus::kInvalidOperation;
  std::binary_semaphore invoked{0};
  OperationCallback callback{
      ReentrantMoveCallback{started.operation, moves, reenter_at_move, reentrant_status, invoked}};
  reenter_at_move.store(moves + 2U, std::memory_order_relaxed);
  const auto registered = started.operation.OnComplete(std::move(callback));

  EXPECT_EQ(registered.status, OperationCallbackStatus::kRegistered);
  EXPECT_EQ(reentrant_status, OperationCallbackStatus::kAlreadyRegistered);
  ASSERT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));
  EXPECT_TRUE(invoked.try_acquire_for(kTestDeadline));
}

TEST(Operation, CallbackInstallationPinsStateAcrossReentrantCompletionAndRelease) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  std::size_t moves = 0;
  std::atomic<std::size_t> reenter_at_move{0};
  bool completion_won = false;
  std::binary_semaphore invoked{0};
  OperationCallback callback{ReentrantReleasingMoveCallback{
      started.operation, started.completion, moves, reenter_at_move, completion_won, invoked}};
  reenter_at_move.store(moves + 2U, std::memory_order_relaxed);

  const auto registered = started.operation.OnComplete(std::move(callback));

  EXPECT_EQ(registered.status, OperationCallbackStatus::kRegistered);
  EXPECT_TRUE(completion_won);
  EXPECT_FALSE(started.operation);
  EXPECT_FALSE(started.completion);
  EXPECT_TRUE(invoked.try_acquire_for(kTestDeadline));
  EXPECT_TRUE(WaitForIdle(reserve));
}

TEST(Operation, CallbackDestructorMayReenterWithoutHoldingTheStateLock) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  OperationCallbackStatus reentrant_status = OperationCallbackStatus::kInvalidOperation;
  std::binary_semaphore callback_destroyed{0};

  const auto registered = started.operation.OnComplete(
      ReentrantDestructorCallback{started.operation, reentrant_status, callback_destroyed});
  ASSERT_EQ(registered.status, OperationCallbackStatus::kRegistered);
  ASSERT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));

  ASSERT_TRUE(callback_destroyed.try_acquire_for(kTestDeadline));
  EXPECT_EQ(reentrant_status, OperationCallbackStatus::kAlreadyRegistered);
}

TEST(Operation, NullFunctionPointerIsRejectedWithoutConsumingTheCallbackSlot) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  using Callback = void (*)(const OperationResult&) noexcept;
  Callback callback = nullptr;

  const auto invalid = started.operation.OnComplete(callback);
  EXPECT_EQ(invalid.status, OperationCallbackStatus::kInvalidCallback);
  const auto registered = started.operation.OnComplete([](const OperationResult&) noexcept {});
  EXPECT_EQ(registered.status, OperationCallbackStatus::kRegistered);
}

TEST(Operation, WaitUntilRejectsUlogOwnedThreadsBeforeBlocking) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  OperationWaitResult wait_result;

  std::thread worker{[&] {
    const ScopedUlogThreadRole role{UlogThreadRole::kWorker};
    wait_result = started.operation.WaitUntil(std::chrono::steady_clock::now() + kTestDeadline);
  }};
  worker.join();

  EXPECT_EQ(wait_result.status, OperationWaitStatus::kForbiddenThread);
  EXPECT_FALSE(wait_result.Message().empty());
  EXPECT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));
}

TEST(Operation, WaitUntilObservesCompletionPublishedByAnotherThread) {
  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  ASSERT_TRUE(started);
  std::latch waiter_started{2};
  OperationWaitResult wait_result;

  std::thread waiter{[&] {
    waiter_started.arrive_and_wait();
    wait_result = started.operation.WaitUntil(std::chrono::steady_clock::now() + kTestDeadline);
  }};
  waiter_started.arrive_and_wait();
  ASSERT_TRUE(started.completion.TryComplete(OperationOutcome::kSucceeded));
  waiter.join();

  ASSERT_EQ(wait_result.status, OperationWaitStatus::kCompleted);
  ASSERT_TRUE(wait_result.completion);
  EXPECT_EQ(wait_result.completion.value_or(OperationResult{}).Outcome(),
            OperationOutcome::kSucceeded);
}

TEST(Operation, AbandonedCompletionSourcePublishesCancellation) {
  ControlReserve reserve{1};
  Operation operation;
  {
    auto started = reserve.TryStart();
    ASSERT_TRUE(started);
    operation = std::move(started.operation);
  }

  const auto cancelled = operation.Poll();
  ASSERT_EQ(cancelled.status, OperationPollStatus::kCompleted);
  ASSERT_TRUE(cancelled.completion);
  EXPECT_EQ(cancelled.completion.value_or(OperationResult{}).Outcome(),
            OperationOutcome::kCancelled);
}

TEST(Operation, StateOutlivesTheReserveOwner) {
  Operation operation;
  OperationCompletion completion;
  {
    ControlReserve reserve{1};
    auto started = reserve.TryStart();
    ASSERT_TRUE(started);
    operation = std::move(started.operation);
    completion = std::move(started.completion);
  }

  EXPECT_EQ(operation.Poll().status, OperationPollStatus::kPending);
  ASSERT_TRUE(completion.TryComplete(OperationOutcome::kSucceeded));
  const auto completed = operation.Poll();
  ASSERT_TRUE(completed.completion);
  EXPECT_EQ(completed.completion.value_or(OperationResult{}).Outcome(),
            OperationOutcome::kSucceeded);
}

TEST(Operation, CompletionAndCallbackRegistrationRaceInvokesExactlyOnce) {
  constexpr std::size_t kIterations = 128;
  ControlReserve reserve{1};

  for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
    auto started = reserve.TryStart();
    ASSERT_TRUE(started) << "iteration " << iteration;
    std::atomic<std::size_t> callback_calls{0};
    std::binary_semaphore callback_finished{0};
    std::latch start{3};
    OperationCallbackResult registration;
    bool completion_won = false;

    std::thread registrar{[&] {
      start.arrive_and_wait();
      registration = started.operation.OnComplete([&](const OperationResult& result) noexcept {
        if (result.Outcome() == OperationOutcome::kSucceeded) {
          callback_calls.fetch_add(1, std::memory_order_relaxed);
        }
        callback_finished.release();
      });
    }};
    std::thread completer{[&] {
      start.arrive_and_wait();
      completion_won = started.completion.TryComplete(OperationOutcome::kSucceeded);
    }};

    start.arrive_and_wait();
    registrar.join();
    completer.join();
    ASSERT_EQ(registration.status, OperationCallbackStatus::kRegistered);
    ASSERT_TRUE(completion_won);
    ASSERT_TRUE(callback_finished.try_acquire_for(kTestDeadline));
    EXPECT_EQ(callback_calls.load(std::memory_order_relaxed), 1U);
    const auto polled = started.operation.Poll();
    ASSERT_EQ(polled.status, OperationPollStatus::kCompleted);
    ASSERT_TRUE(polled.completion);
    EXPECT_EQ(polled.completion.value_or(OperationResult{}).Outcome(),
              OperationOutcome::kSucceeded);

    started.operation = {};
    ASSERT_TRUE(WaitForIdle(reserve)) << "iteration " << iteration;
    EXPECT_EQ(callback_calls.load(std::memory_order_relaxed), 1U);
  }
}

TEST(ControlReserve, ExhaustionIsActionableAndCompletedHandlesRetainTheirSlot) {
  ControlReserve reserve{1};
  auto first = reserve.TryStart();
  ASSERT_TRUE(first);

  const auto exhausted = reserve.TryStart();
  ASSERT_FALSE(exhausted);
  ASSERT_TRUE(exhausted.failure);
  const auto failure = exhausted.failure.value_or(OperationStartFailure{});
  EXPECT_EQ(failure.code, OperationStartErrorCode::kControlReserveExhausted);
  EXPECT_EQ(failure.control_capacity, 1U);
  EXPECT_EQ(failure.controls_in_use, 1U);
  EXPECT_FALSE(failure.Message().empty());
  EXPECT_NE(failure.HowToFix().find("control-operation capacity"), std::string_view::npos);

  ASSERT_TRUE(first.completion.TryComplete(OperationOutcome::kSucceeded));
  EXPECT_FALSE(reserve.TryStart());

  first.operation = {};
  ASSERT_TRUE(WaitForIdle(reserve));
  auto reused = reserve.TryStart();
  ASSERT_TRUE(reused);
  EXPECT_EQ(reserve.GetSnapshot().in_use, 1U);
}

TEST(ControlReserve, RemainsAvailableWhenTheProducerPayloadBudgetIsExhausted) {
  producer::ProducerKernel kernel{producer::KernelConfig{.threshold = Level::kTrace,
                                                         .payload_capacity_bytes = 256,
                                                         .maximum_record_bytes = 256,
                                                         .producer_slots = 1,
                                                         .ingress_cells = 1}};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const Logger logger = kernel.GetLogger();
  constexpr SourceLocation kSource = SourceLocation::Custom("operation.cpp", "Saturate", 1);
  logger.Log<Level::kInfo>(kSource, [] { return "occupy the complete payload reservation"; });

  const auto saturated = kernel.GetSnapshot();
  ASSERT_EQ(saturated.physical_retained_bytes, saturated.payload_capacity_bytes);
  std::size_t rejected_message_evaluations = 0;
  logger.Log<Level::kInfo>(kSource, [&]() -> std::string_view {
    ++rejected_message_evaluations;
    return "must not run";
  });
  EXPECT_EQ(rejected_message_evaluations, 0U);

  ControlReserve reserve{1};
  auto started = reserve.TryStart();
  EXPECT_TRUE(started);
  EXPECT_EQ(reserve.GetSnapshot().in_use, 1U);
}

TEST(ControlReserve, QueuedCallbackPinsTheSlotUntilDeliveryFinishes) {
  ManualCallbackDispatcher dispatcher;
  ControlReserve reserve{1, dispatcher.GetDispatcher()};
  auto first = reserve.TryStart();
  ASSERT_TRUE(first);
  ASSERT_EQ(first.operation.OnComplete([](const OperationResult&) noexcept {}).status,
            OperationCallbackStatus::kRegistered);
  ASSERT_TRUE(first.completion.TryComplete(OperationOutcome::kSucceeded));
  first.operation = {};

  EXPECT_FALSE(reserve.TryStart());
  ASSERT_TRUE(dispatcher.RunOne());
  ASSERT_TRUE(WaitForIdle(reserve));
  EXPECT_TRUE(reserve.TryStart());
}

}  // namespace
}  // namespace ulog::detail::control::test
