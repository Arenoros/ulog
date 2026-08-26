#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_destination.hpp>

#include "support/allocation_interposer.hpp"

namespace {

namespace allocation_tracking = ulog::benchmark_support::allocation_tracking;

using namespace std::chrono_literals;

constexpr std::uint64_t kCycles = 256;
constexpr std::string_view kMessage = "allocation-free-runtime";
constexpr ulog::SourceLocation kSource =
    ulog::SourceLocation::Custom("runtime_allocation.cpp", "Run", 1);

[[nodiscard]] bool WaitSucceeded(ulog::OperationStartResult& started) noexcept {
  if (!started) {
    if (started.failure) {
      std::fprintf(stderr,
                   "runtime allocation test: control start failed: capacity=%zu "
                   "in_use=%zu\n",
                   started.failure->control_capacity, started.failure->controls_in_use);
    }
    return false;
  }
  const auto waited = started.operation.WaitUntil(std::chrono::steady_clock::now() + 2s);
  const bool succeeded = waited.status == ulog::OperationWaitStatus::kCompleted &&
                         waited.completion &&
                         waited.completion->Outcome() == ulog::OperationOutcome::kSucceeded;
  if (!succeeded) {
    std::fprintf(stderr, "runtime allocation test: wait failed: status=%u\n",
                 static_cast<unsigned>(waited.status));
  }
  return succeeded;
}

[[nodiscard]] ulog::RuntimeConfig AllocationConfig() noexcept {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 512,
      .maximum_record_bytes = 512,
      .producer_slots = 1,
      .ingress_cells = 1,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 2s,
      .destruction_timeout = 1s,
  };
}

[[nodiscard]] bool RunCycle(ulog::Runtime& runtime, const ulog::Logger& logger,
                            ulog::testing::InMemoryDestination& destination,
                            const std::uint64_t expected_sequence) noexcept {
  logger.Log<ulog::Level::kInfo>(kSource, []() noexcept { return kMessage; });
  auto drain = runtime.Drain();
  if (!WaitSucceeded(drain)) {
    return false;
  }

  std::optional<ulog::testing::ObservedRecord> record = destination.TryTake();
  return record && record->AdmissionSequence() == expected_sequence &&
         record->Message() == kMessage;
}

[[nodiscard]] bool Run() {
  ulog::testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(AllocationConfig(), destination);
  if (!created) {
    const std::string_view message =
        created.failure ? created.failure->Message() : "missing Runtime";
    std::fprintf(stderr, "runtime allocation test: creation failed: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    return false;
  }

  ulog::Runtime& runtime = *created.runtime;
  const ulog::Logger logger = runtime.GetLogger();
  if (!RunCycle(runtime, logger, destination, 0)) {
    std::fputs("runtime allocation test: warm-up failed\n", stderr);
    return false;
  }

  const std::uint64_t allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  bool valid = true;
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    if (!RunCycle(runtime, logger, destination, cycle + 1U)) {
      valid = false;
      break;
    }
  }

  logger.Log<ulog::Level::kInfo>(kSource, []() noexcept { return kMessage; });
  auto shutdown = runtime.Shutdown();
  valid = WaitSucceeded(shutdown) && valid;
  std::optional<ulog::testing::ObservedRecord> final_record = destination.TryTake();
  valid = final_record && final_record->AdmissionSequence() == kCycles + 1U &&
          final_record->Message() == kMessage && valid;

  const std::uint64_t allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const auto snapshot = runtime.GetSnapshot();
  const std::uint64_t expected_records = kCycles + 2U;
  valid = allocations_after == allocations_before &&
          snapshot.accepted_records == expected_records &&
          snapshot.delivered_records == expected_records && snapshot.dropped_newest_records == 0U &&
          snapshot.retained_records == 0U && !snapshot.admission_open && !snapshot.worker_running &&
          valid;
  if (!valid) {
    std::fprintf(stderr,
                 "runtime allocation test: allocations=%llu accepted=%llu delivered=%llu "
                 "no_producer=%llu lane_full=%llu budget=%llu dropped=%llu retained=%llu\n",
                 static_cast<unsigned long long>(allocations_after - allocations_before),
                 static_cast<unsigned long long>(snapshot.accepted_records),
                 static_cast<unsigned long long>(snapshot.delivered_records),
                 static_cast<unsigned long long>(snapshot.rejected_no_producer),
                 static_cast<unsigned long long>(snapshot.rejected_lane_full),
                 static_cast<unsigned long long>(snapshot.rejected_budget),
                 static_cast<unsigned long long>(snapshot.dropped_newest_records),
                 static_cast<unsigned long long>(snapshot.retained_records));
  }
  return valid;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "runtime allocation test failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("runtime allocation test failed with an unknown exception\n", stderr);
    return 1;
  }
}
