// Concurrent threshold changes, queries, and logging against one Runtime Logger. Run under TSan to
// check that the producer path observes level changes without a data race.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <latch>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kProducerCount = 3;
constexpr std::size_t kAttemptsPerProducer = 4'096;
constexpr std::size_t kLevelChanges = 4'096;
constexpr std::size_t kRecordBytes = 256;
constexpr std::array kThresholds{ulog::Level::kTrace, ulog::Level::kWarning, ulog::Level::kNone,
                                 ulog::Level::kError, ulog::Level::kInfo};

[[nodiscard]] bool Run() {
  ulog::testing::InMemoryDestination destination{
      {.capacity_records = 4, .maximum_record_bytes = kRecordBytes}};
  auto created = ulog::Runtime::Create(
      ulog::RuntimeConfig{
          .threshold = ulog::Level::kInfo,
          // One slot per producer thread plus the main thread's Logger registration.
          .payload_capacity_bytes = (kProducerCount + 1) * 2 * kRecordBytes,
          .maximum_record_bytes = kRecordBytes,
          .producer_slots = kProducerCount + 1,
          .ingress_cells = (kProducerCount + 1) * 2,
          .control_operations = 2,
          .worker_threads = 1,
          .startup_timeout = 2s,
          .destruction_timeout = 2s,
      },
      destination);
  if (!created) {
    std::fputs("runtime threshold stress: creation failed\n", stderr);
    return false;
  }
  ulog::Runtime& runtime = *created.runtime;
  const ulog::Logger logger = runtime.GetLogger();

  std::atomic<bool> producing{true};
  std::atomic<std::size_t> invalid_levels{0};
  std::atomic<std::size_t> evaluated_while_filtered{0};
  std::latch start{kProducerCount + 2};
  std::vector<std::thread> threads;

  for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
    threads.emplace_back([&] {
      const ulog::Logger registered = runtime.GetLogger();
      start.arrive_and_wait();
      for (std::size_t attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
        const auto level = static_cast<ulog::Level>(attempt % 6U);
        LOG_TO(registered, level, "attempt {}", attempt);
      }
    });
  }
  threads.emplace_back([&] {
    start.arrive_and_wait();
    for (std::size_t change = 0; change < kLevelChanges; ++change) {
      static_cast<void>(logger.SetLevel(kThresholds[change % kThresholds.size()]));
    }
    static_cast<void>(logger.SetLevel(ulog::Level::kNone));
  });
  threads.emplace_back([&] {
    start.arrive_and_wait();
    while (producing.load(std::memory_order_relaxed)) {
      const ulog::Level level = logger.GetLevel();
      if (ulog::ToString(level).empty()) {
        invalid_levels.fetch_add(1, std::memory_order_relaxed);
      }
      static_cast<void>(logger.ShouldLog(ulog::Level::kError));
    }
  });
  // Keep the destination draining so producers see both accepted and shed calls.
  std::thread consumer{[&] {
    while (producing.load(std::memory_order_relaxed)) {
      static_cast<void>(destination.TryTake());
      std::this_thread::yield();
    }
  }};

  for (std::size_t index = 0; index < kProducerCount + 1; ++index) {
    threads[index].join();
  }
  // Every change has finished and the final threshold is None: nothing is evaluated now.
  std::size_t evaluations = 0;
  LOG_CRITICAL_TO(logger, "after {}", ++evaluations);
  evaluated_while_filtered.fetch_add(evaluations, std::memory_order_relaxed);

  // Shutdown delivers every accepted Record, so keep taking them until it completes.
  auto shutdown = runtime.Shutdown();
  const bool stopped =
      shutdown && shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 5s).status ==
                      ulog::OperationWaitStatus::kCompleted;
  producing.store(false, std::memory_order_relaxed);
  threads.back().join();
  consumer.join();
  const auto snapshot = runtime.GetSnapshot();
  const bool valid = stopped && invalid_levels.load() == 0U &&
                     evaluated_while_filtered.load() == 0U &&
                     logger.GetLevel() == ulog::Level::kNone &&
                     snapshot.accepted_records <= kProducerCount * kAttemptsPerProducer;
  if (!valid) {
    std::fprintf(stderr,
                 "runtime threshold stress: stopped=%d invalid=%zu filtered_eval=%zu "
                 "accepted=%llu\n",
                 static_cast<int>(stopped), invalid_levels.load(), evaluated_while_filtered.load(),
                 static_cast<unsigned long long>(snapshot.accepted_records));
  }
  return valid;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "runtime threshold stress failed: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("runtime threshold stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
