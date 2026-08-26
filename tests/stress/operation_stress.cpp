#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <latch>
#include <mutex>
#include <thread>
#include <ulog/operation.hpp>

#include "control/control_reserve.hpp"

namespace {

namespace control = ulog::detail::control;

constexpr std::size_t kOperationCount = 64;
constexpr std::size_t kBatches = 32;
constexpr auto kBatchDeadline = std::chrono::seconds{2};
constexpr auto kWatchdogDeadline = std::chrono::seconds{5};

class Watchdog final {
 public:
  Watchdog()
      : thread_{[this] {
          std::unique_lock lock{mutex_};
          if (!condition_.wait_for(lock, kWatchdogDeadline, [this] { return done_; })) {
            std::fputs("operation stress exceeded its five-second watchdog\n", stderr);
            std::abort();
          }
        }} {}

  Watchdog(const Watchdog&) = delete;
  Watchdog& operator=(const Watchdog&) = delete;

  ~Watchdog() {
    {
      std::lock_guard lock{mutex_};
      done_ = true;
    }
    condition_.notify_one();
    thread_.join();
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool done_{false};
  std::thread thread_;
};

[[nodiscard]] bool WaitForCallbacks(const std::atomic<std::size_t>& callbacks,
                                    const std::size_t expected) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kBatchDeadline;
  while (callbacks.load(std::memory_order_acquire) != expected &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return callbacks.load(std::memory_order_acquire) == expected;
}

[[nodiscard]] bool WaitForIdle(control::ControlReserve& reserve) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kBatchDeadline;
  while (reserve.GetSnapshot().in_use != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return reserve.GetSnapshot().in_use == 0;
}

[[nodiscard]] bool RunStress() {
  Watchdog watchdog;
  control::ControlReserve reserve{kOperationCount};
  std::array<control::ControlStartResult, kOperationCount> operations;
  std::array<std::atomic<std::size_t>, kOperationCount> callback_calls{};
  std::array<ulog::OperationCallbackStatus, kOperationCount> registrations{};
  std::array<bool, kOperationCount> completions{};
  bool success = true;

  for (std::size_t batch = 0; batch < kBatches && success; ++batch) {
    std::atomic<std::size_t> callbacks{0};
    for (std::size_t index = 0; index < kOperationCount; ++index) {
      callback_calls[index].store(0, std::memory_order_relaxed);
      operations[index] = reserve.TryStart();
      success = success && static_cast<bool>(operations[index]);
    }
    if (!success) {
      break;
    }

    std::latch start{3};
    std::thread registrar{[&] {
      start.arrive_and_wait();
      for (std::size_t index = 0; index < kOperationCount; ++index) {
        registrations[index] =
            operations[index]
                .operation
                .OnComplete([counter = &callback_calls[index],
                             total = &callbacks](const ulog::OperationResult& result) noexcept {
                  if (result.Outcome() == ulog::OperationOutcome::kSucceeded) {
                    counter->fetch_add(1, std::memory_order_relaxed);
                    total->fetch_add(1, std::memory_order_release);
                  }
                })
                .status;
      }
    }};
    std::thread completer{[&] {
      start.arrive_and_wait();
      for (std::size_t offset = 0; offset < kOperationCount; ++offset) {
        const std::size_t index = kOperationCount - offset - 1U;
        completions[index] =
            operations[index].completion.TryComplete(ulog::OperationOutcome::kSucceeded);
      }
    }};

    start.arrive_and_wait();
    registrar.join();
    completer.join();
    success = success && WaitForCallbacks(callbacks, kOperationCount);

    for (std::size_t index = 0; index < kOperationCount; ++index) {
      const auto polled = operations[index].operation.Poll();
      success = success && registrations[index] == ulog::OperationCallbackStatus::kRegistered &&
                completions[index] && callback_calls[index].load(std::memory_order_relaxed) == 1U &&
                polled.status == ulog::OperationPollStatus::kCompleted && polled.completion &&
                polled.completion->Outcome() == ulog::OperationOutcome::kSucceeded;
      operations[index].operation = {};
    }
    success = success && WaitForIdle(reserve);
  }

  const auto snapshot = reserve.GetSnapshot();
  success = success && snapshot.capacity == kOperationCount && snapshot.in_use == 0U &&
            snapshot.node_backing_bytes != 0U && snapshot.dispatcher_state_bytes != 0U;
  if (!success) {
    std::cerr << "operation stress failed: capacity=" << snapshot.capacity
              << " in_use=" << snapshot.in_use << '\n';
  }
  return success;
}

}  // namespace

int main() noexcept {
  try {
    return RunStress() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "operation stress failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("operation stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
