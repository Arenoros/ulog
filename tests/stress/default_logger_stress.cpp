#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

#include "logger_state.hpp"

namespace {

constexpr std::size_t kExchangeThreadCount = 8;
constexpr std::size_t kExchangesPerThread = 128;
constexpr std::size_t kConcurrentExchangeCount = kExchangeThreadCount * kExchangesPerThread;
constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kProducerRounds = 128;
constexpr std::size_t kCallsPerProducerRound = 16;
constexpr std::size_t kExchangesPerProducerRound = 64;
constexpr std::size_t kTotalCalls = kProducerCount * kProducerRounds * kCallsPerProducerRound;
constexpr std::size_t kFirstProducerTarget = kConcurrentExchangeCount;
constexpr std::size_t kSecondProducerTarget = kFirstProducerTarget + 1U;
constexpr std::size_t kTargetCount = kConcurrentExchangeCount + 2U;
constexpr auto kDeadline = std::chrono::seconds{5};

struct Observation final {
  void Reset() noexcept {
    evaluations.store(0, std::memory_order_relaxed);
    invalid_dispatches.store(0, std::memory_order_relaxed);
    for (auto& count : seen) {
      count.store(0, std::memory_order_relaxed);
    }
  }

  std::atomic<std::size_t> evaluations{0};
  std::atomic<std::size_t> invalid_dispatches{0};
  std::array<std::atomic<std::uint8_t>, kTotalCalls> seen{};
};

Observation& GetObservation() {
  static Observation observation;
  return observation;
}

class StableTarget final {
 public:
  StableTarget()
      : state_{std::atomic<std::uint8_t>{static_cast<std::uint8_t>(ulog::Level::kInfo)},
               &kOperations, this} {}

  [[nodiscard]] ulog::Logger GetLogger() const noexcept {
    return ulog::detail::LoggerAccess::FromState(&state_);
  }

  void Reset() noexcept { calls_.store(0, std::memory_order_relaxed); }

  [[nodiscard]] std::size_t CallCount() const noexcept {
    return calls_.load(std::memory_order_relaxed);
  }

 private:
  static void ConsumeText(void* context, std::string_view text) noexcept {
    auto& observation = *static_cast<Observation*>(context);
    if (text.size() != sizeof(std::uint64_t)) {
      observation.invalid_dispatches.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    std::uint64_t record_id = 0;
    std::memcpy(&record_id, text.data(), sizeof(record_id));
    if (record_id >= kTotalCalls ||
        observation.seen[record_id].fetch_add(1, std::memory_order_relaxed) != 0U) {
      observation.invalid_dispatches.fetch_add(1, std::memory_order_relaxed);
    }
  }

  static void LogMessage(void* context, ulog::Level, const ulog::SourceLocation&,
                         void* builder_context, ulog::detail::MessageBuilder build_message) {
    auto& target = *static_cast<StableTarget*>(context);
    Observation& observation = GetObservation();
    ulog::detail::MessageSink sink{&observation, &ConsumeText};
    build_message(builder_context, sink);
    target.calls_.fetch_add(1, std::memory_order_relaxed);
  }

  inline static constexpr ulog::detail::ProducerOperations kOperations{&LogMessage};

  std::atomic<std::size_t> calls_{0};
  ulog::detail::LoggerState state_;
};

std::array<StableTarget, kTargetCount>& GetTargets() {
  static std::array<StableTarget, kTargetCount> targets;
  return targets;
}

[[nodiscard]] bool CheckConcurrentExchangeChain(ulog::Logger initial) {
  auto& targets = GetTargets();
  std::array<ulog::Logger, kConcurrentExchangeCount> previous_targets;
  std::array<std::thread, kExchangeThreadCount> exchange_threads;
  std::barrier start{static_cast<std::ptrdiff_t>(kExchangeThreadCount)};

  for (std::size_t thread_index = 0; thread_index < kExchangeThreadCount; ++thread_index) {
    exchange_threads[thread_index] = std::thread{[&, thread_index] {
      start.arrive_and_wait();
      for (std::size_t exchange_index = 0; exchange_index < kExchangesPerThread; ++exchange_index) {
        const std::size_t target_index = thread_index * kExchangesPerThread + exchange_index;
        previous_targets[target_index] =
            ulog::ExchangeDefaultLogger(targets[target_index].GetLogger());
        if ((exchange_index & 0x0fU) == 0U) {
          std::this_thread::yield();
        }
      }
    }};
  }
  for (auto& thread : exchange_threads) {
    thread.join();
  }

  std::array<bool, kConcurrentExchangeCount> visited{};
  ulog::Logger current = ulog::GetDefaultLogger();
  for (std::size_t link = 0; link < kConcurrentExchangeCount; ++link) {
    std::size_t target_index = kConcurrentExchangeCount;
    for (std::size_t candidate = 0; candidate < kConcurrentExchangeCount; ++candidate) {
      if (targets[candidate].GetLogger() == current) {
        target_index = candidate;
        break;
      }
    }
    if (target_index == kConcurrentExchangeCount || visited[target_index]) {
      return false;
    }
    visited[target_index] = true;
    current = previous_targets[target_index];
  }
  return current == initial;
}

[[nodiscard]] bool CheckConcurrentDefaultCalls() {
  auto& targets = GetTargets();
  StableTarget& first = targets[kFirstProducerTarget];
  StableTarget& second = targets[kSecondProducerTarget];
  first.Reset();
  second.Reset();
  Observation& observation = GetObservation();
  observation.Reset();
  static_cast<void>(ulog::ExchangeDefaultLogger(first.GetLogger()));

  std::barrier phase{static_cast<std::ptrdiff_t>(kProducerCount + 1U)};
  std::array<std::thread, kProducerCount> producers;
  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    producers[producer_index] = std::thread{[&, producer_index] {
      for (std::size_t round = 0; round < kProducerRounds; ++round) {
        phase.arrive_and_wait();
        for (std::size_t call = 0; call < kCallsPerProducerRound; ++call) {
          const std::uint64_t record_id =
              (producer_index * kProducerRounds + round) * kCallsPerProducerRound + call;
          ulog::GetDefaultLogger().Log<ulog::Level::kInfo>(
              ulog::SourceLocation::Custom("default_logger_stress.cpp", "Producer", 1),
              [&]() -> std::string_view {
                observation.evaluations.fetch_add(1, std::memory_order_relaxed);
                return {reinterpret_cast<const char*>(&record_id), sizeof(record_id)};
              });
          if ((call & 0x07U) == 0U) {
            std::this_thread::yield();
          }
        }
        phase.arrive_and_wait();
      }
    }};
  }

  for (std::size_t round = 0; round < kProducerRounds; ++round) {
    phase.arrive_and_wait();
    for (std::size_t exchange = 0; exchange < kExchangesPerProducerRound; ++exchange) {
      StableTarget& target =
          ((round * kExchangesPerProducerRound + exchange) & 1U) == 0U ? second : first;
      static_cast<void>(ulog::ExchangeDefaultLogger(target.GetLogger()));
      if ((exchange & 0x07U) == 0U) {
        std::this_thread::yield();
      }
    }
    phase.arrive_and_wait();
  }
  for (auto& thread : producers) {
    thread.join();
  }

  bool each_call_seen_once = true;
  for (const auto& count : observation.seen) {
    each_call_seen_once = each_call_seen_once && count.load(std::memory_order_relaxed) == 1U;
  }
  return each_call_seen_once &&
         observation.evaluations.load(std::memory_order_relaxed) == kTotalCalls &&
         observation.invalid_dispatches.load(std::memory_order_relaxed) == 0U &&
         first.CallCount() + second.CallCount() == kTotalCalls;
}

[[nodiscard]] bool RunStress() {
  std::mutex watchdog_mutex;
  std::condition_variable watchdog_cv;
  bool watchdog_done = false;
  std::thread watchdog{[&] {
    std::unique_lock lock{watchdog_mutex};
    if (!watchdog_cv.wait_for(lock, kDeadline, [&] { return watchdog_done; })) {
      std::cerr << "default Logger stress exceeded its five-second watchdog\n";
      std::abort();
    }
  }};

  const ulog::Logger original = ulog::ExchangeDefaultLogger(ulog::GetNullLogger());
  const ulog::Logger initial = ulog::GetDefaultLogger();
  const bool exchange_chain_valid = CheckConcurrentExchangeChain(initial);
  const bool default_calls_valid = CheckConcurrentDefaultCalls();
  static_cast<void>(ulog::ExchangeDefaultLogger(original));

  {
    std::lock_guard lock{watchdog_mutex};
    watchdog_done = true;
  }
  watchdog_cv.notify_one();
  watchdog.join();

  if (!exchange_chain_valid || !default_calls_valid) {
    std::cerr << "default Logger stress failed: exchange-chain=" << exchange_chain_valid
              << " default-calls=" << default_calls_valid << '\n';
  }
  return exchange_chain_valid && default_calls_valid;
}

}  // namespace

int main() noexcept {
  try {
    return RunStress() ? 0 : 1;
  } catch (...) {
    std::cerr << "default Logger stress failed with an exception\n";
    return 1;
  }
}
