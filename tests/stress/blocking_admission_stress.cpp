// Concurrent Logger::LogUntil: producers wait for lane cells and payload credit while the worker
// frees them, then Shutdown releases producers that are still waiting. A lost wakeup shows up as a
// call that waits until its deadline and is dropped, or as a wait far longer than the worker needs.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <vector>

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr ulog::SourceLocation kSource = ulog::SourceLocation::Custom("stress.cpp", "Block", 1);
constexpr auto kDeadline = 20s;
/// Far above what the worker needs to free one cell, far below kDeadline.
constexpr auto kLongestExpectedWait = 5s;

bool Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "blocking admission stress failed: %s\n", what);
  }
  return condition;
}

[[nodiscard]] ulog::RuntimeCreateResult Create(ulog::RuntimeConfig config,
                                               const ulog::testing::InMemoryDestination& sink) {
  config.threshold = ulog::Level::kTrace;
  config.startup_timeout = 5s;
  config.destruction_timeout = 5s;
  return ulog::Runtime::Create(config, sink);
}

/// Producers that each make `calls` blocking calls while a consumer thread drains the destination.
/// Returns whether every call was admitted and none waited anywhere near its deadline.
bool RunSaturated(std::size_t producers, std::size_t calls, ulog::RuntimeConfig config,
                  const char* name) {
  ulog::testing::InMemoryDestination destination{
      {.capacity_records = 4, .maximum_record_bytes = 256}};
  auto created = Create(config, destination);
  if (!Check(static_cast<bool>(created), "the saturated Runtime was not created")) {
    return false;
  }
  ulog::Runtime& runtime = *created.runtime;

  std::atomic<bool> stop{false};
  std::atomic<std::size_t> taken{0};
  std::thread consumer{[&] {
    while (!stop.load(std::memory_order_acquire)) {
      if (destination.TryTake()) {
        taken.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }
    }
  }};

  std::atomic<std::int64_t> longest_wait_us{0};
  std::vector<std::thread> threads;
  threads.reserve(producers);
  for (std::size_t index = 0; index < producers; ++index) {
    threads.emplace_back([&] {
      const ulog::Logger logger = runtime.GetLogger();
      for (std::size_t call = 0; call < calls; ++call) {
        const auto started = Clock::now();
        logger.LogUntil<ulog::Level::kInfo>(started + kDeadline, kSource,
                                            [] { return std::string_view{"waited"}; });
        const auto waited =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - started).count();
        std::int64_t previous = longest_wait_us.load(std::memory_order_relaxed);
        while (waited > previous && !longest_wait_us.compare_exchange_weak(
                                        previous, waited, std::memory_order_relaxed)) {
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  auto shutdown = runtime.Shutdown();
  const bool stopped = shutdown && shutdown.operation.WaitUntil(Clock::now() + 30s).status ==
                                       ulog::OperationWaitStatus::kCompleted;
  stop.store(true, std::memory_order_release);
  consumer.join();
  while (destination.TryTake()) {
    taken.fetch_add(1, std::memory_order_relaxed);
  }

  const ulog::RuntimeSnapshot snapshot = runtime.GetSnapshot();
  const std::size_t attempts = producers * calls;
  std::printf("%s: %zu calls, %llu accepted, %llu dropped, longest wait %lld us\n", name, attempts,
              static_cast<unsigned long long>(snapshot.accepted_records),
              static_cast<unsigned long long>(snapshot.dropped_newest_records),
              static_cast<long long>(longest_wait_us.load()));
  return Check(stopped, "Shutdown did not complete") &&
         Check(snapshot.accepted_records == attempts, "a blocking call was not admitted") &&
         Check(snapshot.dropped_newest_records == 0U, "a blocking call was dropped") &&
         Check(taken.load() == attempts, "an admitted Record was not delivered") &&
         Check(std::chrono::microseconds{longest_wait_us.load()} < kLongestExpectedWait,
               "a blocking call waited far longer than the worker needed");
}

/// Producers fill their lanes against a paused destination and block; Shutdown must release
/// every waiting call promptly as rejected.
bool RunShutdownRelease(std::size_t producers) {
  // Room for every admitted Record, so Shutdown completes once the destination resumes.
  ulog::testing::InMemoryDestination destination{
      {.capacity_records = producers * 2U, .maximum_record_bytes = 256, .start_paused = true}};
  const ulog::RuntimeConfig config{
      .payload_capacity_bytes = std::size_t{64} * 1'024U,
      .maximum_record_bytes = 256,
      .producer_slots = producers,
      .ingress_cells = producers * 2U,
  };
  auto created = Create(config, destination);
  if (!Check(static_cast<bool>(created), "the shutdown Runtime was not created")) {
    return false;
  }
  ulog::Runtime& runtime = *created.runtime;

  constexpr std::size_t kCallsPerProducer = 4;
  std::atomic<std::int64_t> latest_return_us{0};
  std::vector<std::thread> threads;
  threads.reserve(producers);
  for (std::size_t index = 0; index < producers; ++index) {
    threads.emplace_back([&] {
      const ulog::Logger logger = runtime.GetLogger();
      for (std::size_t call = 0; call < kCallsPerProducer; ++call) {
        logger.LogUntil<ulog::Level::kInfo>(Clock::now() + kDeadline, kSource,
                                            [] { return std::string_view{"held"}; });
      }
      const auto now =
          std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
              .count();
      std::int64_t previous = latest_return_us.load(std::memory_order_relaxed);
      while (now > previous &&
             !latest_return_us.compare_exchange_weak(previous, now, std::memory_order_relaxed)) {
      }
    });
  }
  // Every lane holds two Records; the third call of each producer is waiting or about to.
  while (runtime.GetSnapshot().accepted_records < producers * 2U) {
    std::this_thread::sleep_for(1ms);
  }
  std::this_thread::sleep_for(50ms);
  const auto shutdown_at = Clock::now();
  auto shutdown = runtime.Shutdown();
  for (auto& thread : threads) {
    thread.join();
  }
  const auto released_after =
      std::chrono::microseconds{latest_return_us.load()} -
      std::chrono::duration_cast<std::chrono::microseconds>(shutdown_at.time_since_epoch());
  destination.Resume();
  const bool stopped = shutdown && shutdown.operation.WaitUntil(Clock::now() + 30s).status ==
                                       ulog::OperationWaitStatus::kCompleted;

  const ulog::RuntimeSnapshot snapshot = runtime.GetSnapshot();
  std::printf("shutdown: %zu producers released %lld us after Shutdown, %llu rejected\n", producers,
              static_cast<long long>(released_after.count()),
              static_cast<unsigned long long>(snapshot.rejected_no_producer));
  // The fourth call of each producer is filtered by the closed Runtime and counted nowhere.
  return Check(stopped, "Shutdown did not complete") &&
         Check(snapshot.accepted_records == producers * 2U, "unexpected admissions") &&
         Check(snapshot.rejected_no_producer == producers, "a waiting call was not released") &&
         Check(snapshot.dropped_newest_records == producers, "unexpected rejections") &&
         Check(released_after < kLongestExpectedWait, "Shutdown released waiting calls late");
}

bool Run() {
  // Lane-bound: the budget covers every lane, so producers wait only for their own cells and
  // share one wake epoch.
  const bool lanes = RunSaturated(4, 2'000,
                                  {.payload_capacity_bytes = std::size_t{64} * 1'024U,
                                   .maximum_record_bytes = 256,
                                   .producer_slots = 4,
                                   .ingress_cells = 8},
                                  "lane-bound");
  // Budget-bound: one producer whose credit returns only as the worker consumes its Records.
  const bool budget = RunSaturated(1, 4'000,
                                   {.payload_capacity_bytes = 512,
                                    .maximum_record_bytes = 256,
                                    .producer_slots = 1,
                                    .ingress_cells = 8},
                                   "budget-bound");
  const bool shutdown = RunShutdownRelease(4);
  return lanes && budget && shutdown;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "blocking admission stress failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("blocking admission stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
