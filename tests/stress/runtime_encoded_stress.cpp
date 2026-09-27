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
#include <vector>

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

constexpr std::size_t kDeferredProducers = 2;
constexpr std::size_t kDeferredAttemptsPerProducer = 512;
constexpr std::size_t kDeferredSlots = 4;
constexpr std::string_view kDeferredMessage = "deferred";
constexpr std::string_view kDeferredFrame = "tskv\ttext=deferred\n";
constexpr std::size_t kLateCompletionRounds = 32;

[[nodiscard]] bool FailsDelivery(std::uint64_t admission_sequence) noexcept {
  return admission_sequence % 3U == 0U;
}

[[nodiscard]] bool WaitForReport(ulog::OperationStartResult& started,
                                 ulog::OperationResult& result) noexcept {
  if (!started) {
    return false;
  }
  const auto waited =
      started.operation.WaitUntil(std::chrono::steady_clock::now() + kOperationDeadline);
  if (waited.status != ulog::OperationWaitStatus::kCompleted || !waited.completion) {
    return false;
  }
  result = *waited.completion;
  return true;
}

[[nodiscard]] bool IsBalanced(const ulog::OperationReport& report) noexcept {
  return report.delivered_records + report.failed_records + report.unfinished_records ==
             report.watermark_records &&
         report.delivered_bytes + report.failed_bytes + report.unfinished_bytes ==
             report.processed_bytes;
}

/// Completes held deliveries in pairs, newest first, and fails every third admission sequence.
[[nodiscard]] bool RunDeferredCompletionRaces() {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = kDeferredSlots,
      .maximum_record_bytes = kMaximumRecordBytes,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto config = StressConfig();
  config.producer_slots = kDeferredProducers;
  config.ingress_cells = kDeferredSlots;
  config.payload_capacity_bytes = kDeferredSlots * kMaximumRecordBytes;
  auto created = ulog::Runtime::Create(config, destination);
  if (!created) {
    return false;
  }

  std::atomic<bool> stop_completer{false};
  std::atomic<bool> completer_valid{true};
  std::uint64_t completed_deliveries = 0;
  std::uint64_t failed_deliveries = 0;
  std::thread completer{[&] {
    auto finish = [&](ulog::testing::PendingEncodedDelivery& delivery) noexcept {
      const bool fail = FailsDelivery(delivery.AdmissionSequence());
      if (delivery.Bytes() != kDeferredFrame) {
        completer_valid.store(false, std::memory_order_relaxed);
      }
      const auto status = fail ? delivery.Fail() : delivery.Complete();
      if (status != ulog::testing::EncodedDeliveryCompletionStatus::kCompleted) {
        completer_valid.store(false, std::memory_order_relaxed);
      }
      ++(fail ? failed_deliveries : completed_deliveries);
    };
    while (!stop_completer.load(std::memory_order_acquire)) {
      auto older = destination.TryTakePendingDelivery();
      if (!older) {
        std::this_thread::yield();
        continue;
      }
      auto newer = destination.TryTakePendingDelivery();
      if (newer) {
        finish(*newer);
      }
      finish(*older);
    }
  }};

  std::atomic<bool> stop_observer{false};
  std::vector<std::uint8_t> observed(kDeferredProducers * kDeferredAttemptsPerProducer, 0U);
  std::atomic<bool> observer_valid{true};
  std::atomic<std::uint64_t> observed_frames{0};
  std::thread observer{[&] {
    while (!stop_observer.load(std::memory_order_acquire)) {
      auto frame = destination.TryTake();
      if (!frame) {
        std::this_thread::yield();
        continue;
      }
      const std::uint64_t sequence = frame->AdmissionSequence();
      if (sequence >= observed.size() || observed[sequence] != 0U || FailsDelivery(sequence) ||
          frame->Bytes() != kDeferredFrame) {
        observer_valid.store(false, std::memory_order_relaxed);
        continue;
      }
      observed[sequence] = 1U;
      observed_frames.fetch_add(1, std::memory_order_release);
    }
  }};

  std::array<std::thread, kDeferredProducers> producers;
  std::latch start{kDeferredProducers + 1U};
  for (auto& producer : producers) {
    producer = std::thread{[&] {
      const ulog::Logger logger = created.runtime->GetLogger();
      start.arrive_and_wait();
      for (std::size_t attempt = 0; attempt < kDeferredAttemptsPerProducer; ++attempt) {
        logger.Log<ulog::Level::kInfo>(kSource, []() noexcept { return kDeferredMessage; });
      }
    }};
  }
  start.arrive_and_wait();
  for (auto& producer : producers) {
    producer.join();
  }

  const std::uint64_t accepted = created.runtime->GetSnapshot().accepted_records;
  auto drain = created.runtime->Drain();
  ulog::OperationResult drained;
  const bool drain_completed = WaitForReport(drain, drained);
  stop_completer.store(true, std::memory_order_release);
  completer.join();

  std::uint64_t expected_failed = 0;
  for (std::uint64_t sequence = 0; sequence < accepted; ++sequence) {
    expected_failed += FailsDelivery(sequence) ? 1U : 0U;
  }
  const ulog::OperationReport& report = drained.Report();
  const std::uint64_t frame_bytes = kDeferredFrame.size();
  bool valid = drain_completed && drained.Outcome() == ulog::OperationOutcome::kSucceeded &&
               completer_valid.load(std::memory_order_relaxed) && accepted != 0U &&
               report.watermark_records == accepted && report.processed_records == accepted &&
               report.failed_records == expected_failed &&
               report.delivered_records == accepted - expected_failed &&
               report.unfinished_records == 0U &&
               report.processed_bytes == accepted * frame_bytes &&
               report.delivered_bytes == report.delivered_records * frame_bytes &&
               report.failed_bytes == expected_failed * frame_bytes && IsBalanced(report) &&
               completed_deliveries == report.delivered_records &&
               failed_deliveries == report.failed_records;

  const auto observe_deadline = std::chrono::steady_clock::now() + kOperationDeadline;
  while (observed_frames.load(std::memory_order_acquire) != report.delivered_records &&
         std::chrono::steady_clock::now() < observe_deadline) {
    std::this_thread::sleep_for(kPollInterval);
  }
  stop_observer.store(true, std::memory_order_release);
  observer.join();
  const std::uint64_t observed_count = observed_frames.load(std::memory_order_acquire);
  valid = observed_count == report.delivered_records &&
          observer_valid.load(std::memory_order_relaxed) && valid;

  auto shutdown = created.runtime->Shutdown();
  ulog::OperationResult stopped;
  valid = WaitForReport(shutdown, stopped) &&
          stopped.Outcome() == ulog::OperationOutcome::kSucceeded && stopped.Report() == report &&
          valid;
  if (!valid) {
    std::fprintf(stderr,
                 "deferred completion stress failed: accepted=%llu processed=%llu delivered=%llu "
                 "failed=%llu unfinished=%llu completed_by_test=%llu failed_by_test=%llu "
                 "observed=%llu\n",
                 static_cast<unsigned long long>(accepted),
                 static_cast<unsigned long long>(report.processed_records),
                 static_cast<unsigned long long>(report.delivered_records),
                 static_cast<unsigned long long>(report.failed_records),
                 static_cast<unsigned long long>(report.unfinished_records),
                 static_cast<unsigned long long>(completed_deliveries),
                 static_cast<unsigned long long>(failed_deliveries),
                 static_cast<unsigned long long>(observed_count));
  }
  return valid;
}

/// Races held completions against Runtime destruction; every outcome must be counted once.
[[nodiscard]] bool RunLateCompletionDuringDestruction() {
  for (std::size_t round = 0; round < kLateCompletionRounds; ++round) {
    auto config = StressConfig();
    config.producer_slots = 1;
    config.ingress_cells = 2;
    config.payload_capacity_bytes = 2U * kMaximumRecordBytes;
    ulog::testing::InMemoryEncodedDestination destination{{
        .capacity_records = 2,
        .maximum_record_bytes = kMaximumRecordBytes,
    }};
    destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
    auto created = ulog::Runtime::Create(config, destination);
    if (!created) {
      return false;
    }
    const ulog::Logger logger = created.runtime->GetLogger();
    logger.Log<ulog::Level::kInfo>(kSource, []() noexcept { return std::string_view{"late"}; });
    logger.Log<ulog::Level::kInfo>(kSource, []() noexcept { return std::string_view{"late"}; });

    std::optional<ulog::testing::PendingEncodedDelivery> first;
    std::optional<ulog::testing::PendingEncodedDelivery> second;
    const auto take_deadline = std::chrono::steady_clock::now() + kOperationDeadline;
    while ((!first || !second) && std::chrono::steady_clock::now() < take_deadline) {
      auto delivery = destination.TryTakePendingDelivery();
      if (!delivery) {
        std::this_thread::yield();
      } else if (!first) {
        first = std::move(delivery);
      } else {
        second = std::move(delivery);
      }
    }
    if (!first || !second) {
      return false;
    }
    auto drain = created.runtime->Drain();

    ulog::testing::EncodedDeliveryCompletionStatus completed_status{};
    ulog::testing::EncodedDeliveryCompletionStatus failed_status{};
    std::latch race{2};
    std::thread completer{[&] {
      race.arrive_and_wait();
      completed_status = first->Complete();
      failed_status = second->Fail();
    }};
    race.arrive_and_wait();
    created.runtime.reset();
    completer.join();

    ulog::OperationResult result;
    if (!WaitForReport(drain, result)) {
      return false;
    }
    const auto& report = result.Report();
    const auto accepted_status =
        [](ulog::testing::EncodedDeliveryCompletionStatus status) noexcept {
          return status == ulog::testing::EncodedDeliveryCompletionStatus::kCompleted ||
                 status == ulog::testing::EncodedDeliveryCompletionStatus::kCancelled;
        };
    const std::uint64_t delivered =
        completed_status == ulog::testing::EncodedDeliveryCompletionStatus::kCompleted ? 1U : 0U;
    const std::uint64_t failed =
        failed_status == ulog::testing::EncodedDeliveryCompletionStatus::kCompleted ? 1U : 0U;
    const bool outcome_valid =
        result.Outcome() == ulog::OperationOutcome::kCancelled ||
        (result.Outcome() == ulog::OperationOutcome::kSucceeded && report.unfinished_records == 0U);
    const bool valid =
        accepted_status(completed_status) && accepted_status(failed_status) && outcome_valid &&
        report.watermark_records == 2U && report.processed_records == 2U &&
        report.delivered_records == delivered && report.failed_records == failed &&
        IsBalanced(report) &&
        first->Complete() == ulog::testing::EncodedDeliveryCompletionStatus::kInvalidHandle;
    if (!valid) {
      std::fprintf(stderr,
                   "late completion stress failed in round %zu: outcome=%u delivered=%llu "
                   "failed=%llu unfinished=%llu statuses=%u/%u\n",
                   round, static_cast<unsigned>(result.Outcome()),
                   static_cast<unsigned long long>(report.delivered_records),
                   static_cast<unsigned long long>(report.failed_records),
                   static_cast<unsigned long long>(report.unfinished_records),
                   static_cast<unsigned>(completed_status), static_cast<unsigned>(failed_status));
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool RunStress() {
  Watchdog watchdog;
  const bool backpressure_and_fifo = RunConcurrentBackpressureAndFifo();
  const bool bounded_destruction = RunBoundedDestruction();
  if (!bounded_destruction) {
    std::fputs("encoded runtime stress destruction scenario failed\n", stderr);
  }
  const bool deferred_completion = RunDeferredCompletionRaces();
  const bool late_completion = RunLateCompletionDuringDestruction();
  return backpressure_and_fifo && bounded_destruction && deferred_completion && late_completion;
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
