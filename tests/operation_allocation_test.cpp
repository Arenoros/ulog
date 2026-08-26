#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <thread>
#include <ulog/operation.hpp>

#include "control/control_reserve.hpp"
#include "support/allocation_interposer.hpp"

namespace {

namespace allocation_tracking = ulog::benchmark_support::allocation_tracking;
namespace control = ulog::detail::control;

constexpr std::uint64_t kCycles = 512;
constexpr auto kDeadline = std::chrono::seconds{2};

[[nodiscard]] bool WaitForValue(const std::atomic<std::uint64_t>& value,
                                const std::uint64_t expected) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kDeadline;
  while (value.load(std::memory_order_acquire) != expected &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return value.load(std::memory_order_acquire) == expected;
}

[[nodiscard]] bool WaitForIdle(control::ControlReserve& reserve) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kDeadline;
  while (reserve.GetSnapshot().in_use != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return reserve.GetSnapshot().in_use == 0;
}

[[nodiscard]] bool Run() {
  control::ControlReserve reserve{1};
  std::atomic<std::uint64_t> callbacks{0};

  auto warm = reserve.TryStart();
  if (!warm ||
      warm.operation
              .OnComplete([&](const ulog::OperationResult&) noexcept {
                callbacks.fetch_add(1, std::memory_order_release);
              })
              .status != ulog::OperationCallbackStatus::kRegistered ||
      !warm.completion.TryComplete(ulog::OperationOutcome::kSucceeded) ||
      !WaitForValue(callbacks, 1)) {
    std::fputs("operation allocation test: warm-up failed\n", stderr);
    return false;
  }
  warm.operation = {};
  if (!WaitForIdle(reserve)) {
    std::fputs("operation allocation test: warm-up slot was not recycled\n", stderr);
    return false;
  }

  const std::uint64_t allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  bool valid = true;
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    auto started = reserve.TryStart();
    valid = valid && static_cast<bool>(started);
    if (!started) {
      break;
    }
    valid = valid && started.operation
                             .OnComplete([&](const ulog::OperationResult& result) noexcept {
                               if (result.Outcome() == ulog::OperationOutcome::kSucceeded) {
                                 callbacks.fetch_add(1, std::memory_order_release);
                               }
                             })
                             .status == ulog::OperationCallbackStatus::kRegistered;
    valid = valid && started.completion.TryComplete(ulog::OperationOutcome::kSucceeded);
    valid = valid && WaitForValue(callbacks, cycle + 2U);
    const auto polled = started.operation.Poll();
    valid = valid && polled.status == ulog::OperationPollStatus::kCompleted && polled.completion &&
            polled.completion->Outcome() == ulog::OperationOutcome::kSucceeded;
    started.operation = {};
    valid = valid && WaitForIdle(reserve);
  }
  const std::uint64_t allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);

  const auto snapshot = reserve.GetSnapshot();
  valid = valid && allocations_after == allocations_before && snapshot.capacity == 1U &&
          snapshot.in_use == 0U && snapshot.node_backing_bytes != 0U &&
          snapshot.dispatcher_state_bytes != 0U;
  if (!valid) {
    std::fprintf(stderr, "operation allocation test: allocations=%llu callbacks=%llu in_use=%zu\n",
                 static_cast<unsigned long long>(allocations_after - allocations_before),
                 static_cast<unsigned long long>(callbacks.load(std::memory_order_relaxed)),
                 snapshot.in_use);
  }
  return valid;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "operation allocation test failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("operation allocation test failed with an unknown exception\n", stderr);
    return 1;
  }
}
