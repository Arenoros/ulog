#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <latch>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_destination.hpp>

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kAcceptedPerProducer = 2;
constexpr std::size_t kAttemptsPerProducer = 4'096;
constexpr std::size_t kAcceptedRecords = kProducerCount * kAcceptedPerProducer;
constexpr std::size_t kMaximumRecordBytes = 256;
constexpr auto kOperationDeadline = 2s;
constexpr auto kWatchdogDeadline = 5s;
constexpr auto kDestructionTestUpperBound = 1s;
constexpr ulog::SourceLocation kSource =
    ulog::SourceLocation::Custom("runtime_stress.cpp", "Producer", 1);
constexpr std::array<std::string_view, kProducerCount> kMessages{"producer-0", "producer-1",
                                                                 "producer-2", "producer-3"};

class Watchdog final {
 public:
  Watchdog()
      : thread_{[this] {
          std::unique_lock lock{mutex_};
          if (!condition_.wait_for(lock, kWatchdogDeadline, [this] { return done_; })) {
            std::fputs("runtime stress exceeded its five-second watchdog\n", stderr);
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

[[nodiscard]] bool WaitForOutcome(const ulog::Operation& operation,
                                  const ulog::OperationOutcome outcome) noexcept {
  const auto waited = operation.WaitUntil(std::chrono::steady_clock::now() + kOperationDeadline);
  return waited.status == ulog::OperationWaitStatus::kCompleted && waited.completion &&
         waited.completion->Outcome() == outcome;
}

[[nodiscard]] ulog::RuntimeConfig StressConfig() noexcept {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = kAcceptedRecords * kMaximumRecordBytes,
      .maximum_record_bytes = kMaximumRecordBytes,
      .producer_slots = kProducerCount,
      .ingress_cells = kAcceptedRecords,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 1s,
      .destruction_timeout = 250ms,
  };
}

[[nodiscard]] std::optional<std::size_t> MessageProducer(std::string_view message) noexcept {
  for (std::size_t index = 0; index < kMessages.size(); ++index) {
    if (message == kMessages[index]) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool RunConcurrentSaturationAndFifo() {
  ulog::testing::InMemoryDestination destination{{
      .capacity_records = kAcceptedRecords,
      .maximum_record_bytes = kMaximumRecordBytes,
      .start_paused = true,
  }};
  auto created = ulog::Runtime::Create(StressConfig(), destination);
  if (!created) {
    return false;
  }

  std::array<std::atomic<std::size_t>, kProducerCount> evaluations{};
  std::array<std::thread, kProducerCount> producers;
  std::latch start{kProducerCount + 1U};
  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    producers[producer_index] = std::thread{[&, producer_index] {
      const ulog::Logger logger = created.runtime->GetLogger();
      start.arrive_and_wait();
      for (std::size_t attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
        logger.Log<ulog::Level::kInfo>(kSource, [&]() noexcept {
          evaluations[producer_index].fetch_add(1, std::memory_order_relaxed);
          return kMessages[producer_index];
        });
      }
    }};
  }

  start.arrive_and_wait();
  for (auto& producer : producers) {
    producer.join();
  }

  const auto saturated = created.runtime->GetSnapshot();
  std::size_t evaluation_count = 0;
  for (const auto& count : evaluations) {
    evaluation_count += count.load(std::memory_order_relaxed);
  }
  const std::uint64_t expected_rejected =
      kProducerCount * (kAttemptsPerProducer - kAcceptedPerProducer);
  bool valid =
      evaluation_count == kAcceptedRecords && saturated.accepted_records == kAcceptedRecords &&
      saturated.delivered_records == 0U && saturated.rejected_lane_full == expected_rejected &&
      saturated.dropped_newest_records == expected_rejected &&
      saturated.retained_records == kAcceptedRecords;

  auto drain = created.runtime->Drain();
  valid = drain && drain.operation.Poll().status == ulog::OperationPollStatus::kPending && valid;
  destination.Resume();
  valid = WaitForOutcome(drain.operation, ulog::OperationOutcome::kSucceeded) && valid;

  std::array<std::size_t, kProducerCount> records_by_producer{};
  for (std::uint64_t sequence = 0; sequence < kAcceptedRecords; ++sequence) {
    std::optional<ulog::testing::ObservedRecord> record = destination.TryTake();
    if (!record || record->AdmissionSequence() != sequence) {
      valid = false;
      continue;
    }
    const auto producer_index = MessageProducer(record->Message());
    if (!producer_index) {
      valid = false;
      continue;
    }
    ++records_by_producer[*producer_index];
  }
  for (const std::size_t count : records_by_producer) {
    valid = count == kAcceptedPerProducer && valid;
  }

  const auto drained = created.runtime->GetSnapshot();
  valid = drained.delivered_records == kAcceptedRecords && drained.retained_records == 0U &&
          drained.admission_open && drained.worker_running && valid;
  auto shutdown = created.runtime->Shutdown();
  valid =
      shutdown && WaitForOutcome(shutdown.operation, ulog::OperationOutcome::kSucceeded) && valid;
  const auto stopped = created.runtime->GetSnapshot();
  valid = !stopped.admission_open && !stopped.worker_running && valid;

  if (!valid) {
    std::fprintf(stderr,
                 "runtime stress saturation failed: evaluations=%zu accepted=%llu "
                 "delivered=%llu lane_rejected=%llu retained=%llu\n",
                 evaluation_count, static_cast<unsigned long long>(saturated.accepted_records),
                 static_cast<unsigned long long>(drained.delivered_records),
                 static_cast<unsigned long long>(saturated.rejected_lane_full),
                 static_cast<unsigned long long>(drained.retained_records));
  }
  return valid;
}

[[nodiscard]] bool RunBoundedDestruction() {
  auto config = StressConfig();
  config.payload_capacity_bytes = kMaximumRecordBytes;
  config.producer_slots = 1;
  config.ingress_cells = 1;
  config.control_operations = 1;
  ulog::testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = kMaximumRecordBytes,
      .start_paused = true,
  }};
  auto created = ulog::Runtime::Create(config, destination);
  if (!created) {
    return false;
  }

  const ulog::Logger logger = created.runtime->GetLogger();
  std::size_t evaluations = 0;
  logger.Log<ulog::Level::kInfo>(kSource, [&]() noexcept {
    ++evaluations;
    return std::string_view{"pending"};
  });
  logger.Log<ulog::Level::kInfo>(kSource, [&]() noexcept {
    ++evaluations;
    return std::string_view{"rejected"};
  });
  auto drain = created.runtime->Drain();
  if (!drain || drain.operation.Poll().status != ulog::OperationPollStatus::kPending) {
    return false;
  }

  const auto destruction_started = std::chrono::steady_clock::now();
  created.runtime.reset();
  const auto destruction_elapsed = std::chrono::steady_clock::now() - destruction_started;
  const bool cancelled = WaitForOutcome(drain.operation, ulog::OperationOutcome::kCancelled);
  if (destruction_elapsed > kDestructionTestUpperBound) {
    std::fprintf(stderr, "runtime destruction exceeded one second\n");
  }
  return cancelled && destruction_elapsed <= kDestructionTestUpperBound && evaluations == 1U &&
         !destination.TryTake();
}

[[nodiscard]] bool RunStress() {
  Watchdog watchdog;
  const bool saturation_and_fifo = RunConcurrentSaturationAndFifo();
  const bool bounded_destruction = RunBoundedDestruction();
  if (!bounded_destruction) {
    std::fputs("runtime stress destruction scenario failed\n", stderr);
  }
  return saturation_and_fifo && bounded_destruction;
}

}  // namespace

int main() noexcept {
  try {
    return RunStress() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "runtime stress failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("runtime stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
