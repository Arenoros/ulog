#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
#include <ulog/testing/in_memory_encoded_destination.hpp>

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kAcceptedPerProducer = 2;
constexpr std::size_t kAttemptsPerProducer = 4'096;
constexpr std::size_t kAcceptedRecords = kProducerCount * kAcceptedPerProducer;
constexpr std::size_t kDestinationCapacity = 2;
constexpr std::size_t kMaximumRecordBytes = 256;
constexpr auto kOperationDeadline = 1'500ms;
constexpr auto kBackpressureDeadline = 1s;
constexpr auto kWatchdogDeadline = 5s;
constexpr auto kPollInterval = 1ms;
constexpr ulog::SourceLocation kSource =
    ulog::SourceLocation::Custom("runtime_encoded_stress.cpp", "Producer", 1);

struct ProducerCase final {
  std::string_view message;
  std::string_view encoded_frame;
};

constexpr std::array<ProducerCase, kProducerCount> kProducerCases{{
    {.message = "producer-0", .encoded_frame = "tskv\ttext=producer-0\n"},
    {.message = "producer-1", .encoded_frame = "tskv\ttext=producer-1\n"},
    {.message = "producer-2", .encoded_frame = "tskv\ttext=producer-2\n"},
    {.message = "producer-3", .encoded_frame = "tskv\ttext=producer-3\n"},
}};

class Watchdog final {
 public:
  Watchdog()
      : thread_{[this] {
          std::unique_lock lock{mutex_};
          if (!condition_.wait_for(lock, kWatchdogDeadline, [this] { return done_; })) {
            std::fputs("encoded runtime stress exceeded its five-second watchdog\n", stderr);
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

[[nodiscard]] bool WaitForDelivered(const ulog::Runtime& runtime,
                                    std::uint64_t delivered_records) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kBackpressureDeadline;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runtime.GetSnapshot().delivered_records == delivered_records) {
      return true;
    }
    std::this_thread::sleep_for(kPollInterval);
  }
  return runtime.GetSnapshot().delivered_records == delivered_records;
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

[[nodiscard]] std::optional<std::size_t> FrameProducer(std::string_view frame) noexcept {
  for (std::size_t index = 0; index < kProducerCases.size(); ++index) {
    if (frame == kProducerCases[index].encoded_frame) {
      return index;
    }
  }
  return std::nullopt;
}

[[nodiscard]] bool RunConcurrentBackpressureAndFifo() {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = kDestinationCapacity,
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
          return kProducerCases[producer_index].message;
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
  bool valid = evaluation_count == kAcceptedRecords &&
               saturated.accepted_records == kAcceptedRecords &&
               saturated.completed_records == 0U && saturated.delivered_records == 0U &&
               saturated.encoding_failed_records == 0U &&
               saturated.rejected_lane_full == expected_rejected &&
               saturated.dropped_newest_records == expected_rejected &&
               saturated.retained_records == kAcceptedRecords;

  auto drain = created.runtime->Drain();
  valid = drain && drain.operation.Poll().status == ulog::OperationPollStatus::kPending && valid;
  destination.Resume();
  const bool destination_filled = WaitForDelivered(*created.runtime, kDestinationCapacity);
  const auto backpressured = created.runtime->GetSnapshot();
  valid = destination_filled && backpressured.completed_records == kDestinationCapacity &&
          backpressured.delivered_records == kDestinationCapacity &&
          backpressured.retained_records == kAcceptedRecords - kDestinationCapacity &&
          drain.operation.Poll().status == ulog::OperationPollStatus::kPending && valid;

  std::array<std::size_t, kProducerCount> records_by_producer{};
  std::uint64_t observed_records = 0;
  std::uint64_t observed_bytes = 0;
  bool consumer_valid = true;
  std::atomic<bool> stop_consumer{false};
  const auto consumer_deadline = std::chrono::steady_clock::now() + kOperationDeadline;
  std::thread consumer{[&] {
    while (observed_records < kAcceptedRecords && !stop_consumer.load(std::memory_order_relaxed) &&
           std::chrono::steady_clock::now() < consumer_deadline) {
      auto frame = destination.TryTake();
      if (!frame) {
        std::this_thread::sleep_for(kPollInterval);
        continue;
      }
      consumer_valid = frame->AdmissionSequence() == observed_records && consumer_valid;
      const auto producer_index = FrameProducer(frame->Bytes());
      if (!producer_index) {
        consumer_valid = false;
      } else {
        ++records_by_producer[*producer_index];
      }
      observed_bytes += frame->Bytes().size();
      ++observed_records;
    }
    consumer_valid = observed_records == kAcceptedRecords && consumer_valid;
  }};

  const bool drain_succeeded = WaitForOutcome(drain.operation, ulog::OperationOutcome::kSucceeded);
  if (!drain_succeeded) {
    stop_consumer.store(true, std::memory_order_relaxed);
  }
  consumer.join();
  valid = drain_succeeded && consumer_valid && valid;
  for (const std::size_t count : records_by_producer) {
    valid = count == kAcceptedPerProducer && valid;
  }

  const auto drained = created.runtime->GetSnapshot();
  valid = drained.completed_records == kAcceptedRecords &&
          drained.delivered_records == kAcceptedRecords &&
          drained.delivered_bytes == observed_bytes && drained.encoding_failed_records == 0U &&
          drained.retained_records == 0U && drained.admission_open && drained.worker_running &&
          !destination.TryTake() && valid;

  auto shutdown = created.runtime->Shutdown();
  valid =
      shutdown && WaitForOutcome(shutdown.operation, ulog::OperationOutcome::kSucceeded) && valid;
  const auto stopped = created.runtime->GetSnapshot();
  valid = !stopped.admission_open && !stopped.worker_running && valid;

  if (!valid) {
    std::fprintf(stderr,
                 "encoded runtime stress failed: evaluations=%zu accepted=%llu completed=%llu "
                 "delivered=%llu bytes=%llu encoding_failed=%llu lane_rejected=%llu retained=%llu "
                 "observed=%llu\n",
                 evaluation_count, static_cast<unsigned long long>(saturated.accepted_records),
                 static_cast<unsigned long long>(drained.completed_records),
                 static_cast<unsigned long long>(drained.delivered_records),
                 static_cast<unsigned long long>(drained.delivered_bytes),
                 static_cast<unsigned long long>(drained.encoding_failed_records),
                 static_cast<unsigned long long>(saturated.rejected_lane_full),
                 static_cast<unsigned long long>(drained.retained_records),
                 static_cast<unsigned long long>(observed_records));
  }
  return valid;
}

[[nodiscard]] bool RunBoundedDestruction() {
  auto config = StressConfig();
  config.payload_capacity_bytes = kMaximumRecordBytes;
  config.producer_slots = 1;
  config.ingress_cells = 1;
  config.control_operations = 1;
  ulog::testing::InMemoryEncodedDestination destination{{
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

  const auto before_destruction = created.runtime->GetSnapshot();
  created.runtime.reset();
  const bool cancelled = WaitForOutcome(drain.operation, ulog::OperationOutcome::kCancelled);
  return cancelled && evaluations == 1U && before_destruction.accepted_records == 1U &&
         before_destruction.completed_records == 0U && before_destruction.delivered_records == 0U &&
         before_destruction.retained_records == 1U && !destination.TryTake();
}

[[nodiscard]] bool RunStress() {
  Watchdog watchdog;
  const bool backpressure_and_fifo = RunConcurrentBackpressureAndFifo();
  const bool bounded_destruction = RunBoundedDestruction();
  if (!bounded_destruction) {
    std::fputs("encoded runtime stress destruction scenario failed\n", stderr);
  }
  return backpressure_and_fifo && bounded_destruction;
}

}  // namespace

int main() noexcept {
  try {
    return RunStress() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "encoded runtime stress failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("encoded runtime stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
