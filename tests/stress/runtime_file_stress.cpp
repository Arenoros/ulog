#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <latch>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>

#include "io/raw_file_sink.hpp"
#include "runtime_factory.hpp"
#include "support/temporary_directory.hpp"

namespace {

using namespace std::chrono_literals;

constexpr std::size_t kProducerCount = 4;
constexpr std::size_t kAttemptsPerProducer = 512;
constexpr std::size_t kDestructionRounds = 16;
constexpr auto kOperationDeadline = 2s;
constexpr auto kWatchdogDeadline = 8s;
constexpr ulog::SourceLocation kSource =
    ulog::SourceLocation::Custom("runtime_file_stress.cpp", "Producer", 1);
constexpr std::array<std::string_view, kProducerCount> kMessages{"producer-0", "producer-1",
                                                                 "producer-2", "producer-3"};

class Watchdog final {
 public:
  Watchdog()
      : thread_{[this] {
          std::unique_lock lock{mutex_};
          if (!condition_.wait_for(lock, kWatchdogDeadline, [this] { return done_; })) {
            std::fputs("file runtime stress exceeded its watchdog\n", stderr);
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

[[nodiscard]] ulog::RuntimeConfig StressConfig() noexcept {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 4U * 256U,
      .maximum_record_bytes = 256,
      .producer_slots = kProducerCount,
      .ingress_cells = kProducerCount,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 2s,
      .destruction_timeout = 1s,
  };
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

[[nodiscard]] std::array<std::uint64_t, kProducerCount> CountFrames(std::string_view bytes,
                                                                    bool& well_formed) {
  std::array<std::uint64_t, kProducerCount> counts{};
  well_formed = true;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t end = bytes.find('\n', offset);
    if (end == std::string_view::npos) {
      well_formed = false;
      break;
    }
    const std::string_view line = bytes.substr(offset, end - offset);
    bool matched = false;
    for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
      if (line == std::string{"tskv\ttext="} + std::string{kMessages[producer]}) {
        ++counts[producer];
        matched = true;
      }
    }
    well_formed = matched && well_formed;
    offset = end + 1U;
  }
  return counts;
}

/// Concurrent producers append through two buffers while every write is split into 5-byte parts.
[[nodiscard]] bool RunConcurrentPartialWrites() {
  const ulog::test_support::TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "concurrent.log";
  std::atomic<std::uint64_t> other_thread_operations{0};
  const ulog::detail::io::FileFaultPlan faults{.maximum_write_bytes = 5,
                                               .other_thread_operations = &other_thread_operations};
  auto created = ulog::detail::RuntimeFactoryAccess::CreateRawFileRuntime(
      StressConfig(), ulog::RawFileRouteConfig{.path = path, .write_buffers = 2}, faults);
  if (!created) {
    return false;
  }

  std::array<std::atomic<std::uint64_t>, kProducerCount> evaluations{};
  std::array<std::thread, kProducerCount> producers;
  std::latch start{kProducerCount + 1U};
  for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
    producers[producer] = std::thread{[&, producer] {
      const ulog::Logger logger = created.runtime->GetLogger();
      start.arrive_and_wait();
      for (std::size_t attempt = 0; attempt < kAttemptsPerProducer; ++attempt) {
        logger.Log<ulog::Level::kInfo>(kSource, [&]() noexcept {
          evaluations[producer].fetch_add(1, std::memory_order_relaxed);
          return kMessages[producer];
        });
      }
    }};
  }
  start.arrive_and_wait();
  for (auto& producer : producers) {
    producer.join();
  }

  auto shutdown = created.runtime->Shutdown();
  ulog::OperationResult stopped;
  const bool completed = WaitForReport(shutdown, stopped);
  const auto snapshot = created.runtime->GetSnapshot();
  created.runtime.reset();

  bool well_formed = false;
  const std::string bytes = ulog::test_support::ReadFileBytes(path);
  const auto counts = CountFrames(bytes, well_formed);
  bool valid = completed && stopped.Outcome() == ulog::OperationOutcome::kSucceeded &&
               well_formed && other_thread_operations.load() == 0U &&
               stopped.Report().delivered_records == snapshot.accepted_records &&
               stopped.Report().delivered_bytes == bytes.size() &&
               stopped.Report().unfinished_records == 0U;
  std::uint64_t evaluated = 0;
  for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
    const std::uint64_t producer_evaluations = evaluations[producer].load();
    evaluated += producer_evaluations;
    valid = counts[producer] == producer_evaluations && valid;
  }
  valid = evaluated == snapshot.accepted_records && valid;
  if (!valid) {
    std::fprintf(stderr,
                 "concurrent file stress failed: accepted=%llu delivered=%llu bytes=%zu "
                 "well_formed=%d off_loop=%llu\n",
                 static_cast<unsigned long long>(snapshot.accepted_records),
                 static_cast<unsigned long long>(stopped.Report().delivered_records), bytes.size(),
                 well_formed ? 1 : 0,
                 static_cast<unsigned long long>(other_thread_operations.load()));
  }
  return valid;
}

/// Destroys Runtimes while split appends are active; files must hold only whole frames.
[[nodiscard]] bool RunDestructionDuringActiveWrites() {
  const ulog::test_support::TemporaryDirectory directory;
  for (std::size_t round = 0; round < kDestructionRounds; ++round) {
    const std::filesystem::path path =
        directory.Path() / ("destroyed-" + std::to_string(round) + ".log");
    const ulog::detail::io::FileFaultPlan faults{.maximum_write_bytes = 3};
    auto config = StressConfig();
    config.producer_slots = 1;
    auto created = ulog::detail::RuntimeFactoryAccess::CreateRawFileRuntime(
        config, ulog::RawFileRouteConfig{.path = path, .write_buffers = 4}, faults);
    if (!created) {
      return false;
    }
    const ulog::Logger logger = created.runtime->GetLogger();
    for (std::size_t record = 0; record < 32U; ++record) {
      logger.Log<ulog::Level::kInfo>(kSource, []() noexcept { return kMessages[0]; });
    }
    auto drain = created.runtime->Drain();
    const auto started = std::chrono::steady_clock::now();
    created.runtime.reset();
    const bool bounded = std::chrono::steady_clock::now() - started < 1500ms;

    ulog::OperationResult result;
    const bool completed = WaitForReport(drain, result);
    const auto& report = result.Report();
    bool well_formed = false;
    const auto counts = CountFrames(ulog::test_support::ReadFileBytes(path), well_formed);
    const bool valid =
        bounded && completed && well_formed &&
        (result.Outcome() == ulog::OperationOutcome::kSucceeded ||
         result.Outcome() == ulog::OperationOutcome::kCancelled) &&
        report.delivered_records + report.failed_records + report.unfinished_records ==
            report.watermark_records &&
        report.failed_records == 0U && counts[0] >= report.delivered_records;
    if (!valid) {
      std::fprintf(stderr,
                   "file destruction stress failed in round %zu: bounded=%d outcome=%u "
                   "delivered=%llu unfinished=%llu frames=%llu well_formed=%d\n",
                   round, bounded ? 1 : 0, static_cast<unsigned>(result.Outcome()),
                   static_cast<unsigned long long>(report.delivered_records),
                   static_cast<unsigned long long>(report.unfinished_records),
                   static_cast<unsigned long long>(counts[0]), well_formed ? 1 : 0);
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool RunStress() {
  Watchdog watchdog;
  const bool concurrent = RunConcurrentPartialWrites();
  const bool destruction = RunDestructionDuringActiveWrites();
  return concurrent && destruction;
}

}  // namespace

int main() noexcept {
  try {
    return RunStress() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "file runtime stress failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("file runtime stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
