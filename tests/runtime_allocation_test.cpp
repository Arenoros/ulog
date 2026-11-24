#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <typeinfo>
#include <ulog/fields.hpp>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <ulog/timestamp.hpp>
#include <utility>
#include <vector>

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
      .payload_capacity_bytes = 1024,
      .maximum_record_bytes = 1024,
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
  // MSVC source functions are long; the Record bound leaves room for them and every field.
  LOG_INFO_TO(logger,
              ulog::Fields{{"string", kMessage},
                           {"signed", -1},
                           {"unsigned", expected_sequence},
                           {"double", 0.5},
                           {"flag", true},
                           {"none", ulog::kNull},
                           {"replaced", 1},
                           {"replaced", 2}},
              "{}", kMessage);
  auto drain = runtime.Drain();
  if (!WaitSucceeded(drain)) {
    return false;
  }

  std::optional<ulog::testing::ObservedRecord> record = destination.TryTake();
  if (!record) {
    return false;
  }
  const std::optional<ulog::testing::ObservedField> replaced = record->Field(6);
  // The warm-up cycle loads the platform time-zone state used by local formatting.
  return record->AdmissionSequence() == expected_sequence && record->Message() == kMessage &&
         record->FieldCount() == 7U && replaced && replaced->value.Int64() == 2 &&
         ulog::FormatUtcTimestamp(record->EventTimestamp()).size == ulog::kTimestampTextBytes &&
         ulog::FormatLocalTimestamp(record->EventTimestamp()).size == ulog::kTimestampTextBytes;
}

struct StreamFailure final : std::exception {
  [[nodiscard]] const char* what() const noexcept override { return "stream failure"; }
};

/// Writes one Record through every native stream form: continued fmt text, characters, strings,
/// integers, floating point, booleans, an exception, and inserted fields.
[[nodiscard]] bool RunStreamCycle(ulog::Runtime& runtime, const ulog::Logger& logger,
                                  ulog::testing::InMemoryDestination& destination,
                                  const std::uint64_t expected_sequence,
                                  const std::exception& exception,
                                  std::string_view expected_message) noexcept {
  LOG_INFO_TO(logger, "{}:", kMessage)
      << ' ' << -1 << ' ' << std::numeric_limits<std::uint64_t>::max() << ' ' << 0.5 << ' ' << true
      << ' ' << 'x' << ' ' << std::string_view{"view"} << ' ' << exception
      << ulog::Fields{{"sequence", expected_sequence}};
  auto drain = runtime.Drain();
  if (!WaitSucceeded(drain)) {
    return false;
  }
  std::optional<ulog::testing::ObservedRecord> record = destination.TryTake();
  return record && record->AdmissionSequence() == expected_sequence &&
         record->Message() == expected_message && record->FieldCount() == 1U;
}

/// Values for the advanced native forms, built before the measured cycles.
struct AdvancedValues final {
  std::atomic<int> counter{3};
  std::optional<int> empty;
  std::vector<int> numbers{1, 2};
  std::map<std::string, int> map{{"k", 1}};
  std::tuple<int, char> tuple{4, 't'};
  std::chrono::system_clock::time_point time =
      std::chrono::sys_days{std::chrono::year{2000} / std::chrono::January / 2} +
      std::chrono::seconds{1};
};

/// Writes one Record through the allocation-free advanced forms.
[[nodiscard]] bool RunAdvancedCycle(ulog::Runtime& runtime, const ulog::Logger& logger,
                                    ulog::testing::InMemoryDestination& destination,
                                    const std::uint64_t expected_sequence,
                                    const AdvancedValues& values) noexcept {
  constexpr std::string_view kAdvanced =
      "(null) 0x000000000000002A 2A \"q\\\"\" 3 (none) [1, 2] [\"k\": 1] (4, t) (5, x) 7ms "
      "2000-01-02T00:00:01+0000 lazy";
  const auto lazy = [](ulog::LogWriter& writer) { writer << "lazy"; };
  LOG_INFO_TO(logger) << static_cast<const int*>(nullptr) << ' ' << ulog::Hex{42U} << ' '
                      << ulog::HexShort{42U} << ' ' << ulog::Quoted{"q\""} << ' ' << values.counter
                      << ' ' << values.empty << ' ' << values.numbers << ' ' << values.map << ' '
                      << values.tuple << ' ' << std::pair<int, char>{5, 'x'} << ' '
                      << std::chrono::milliseconds{7} << ' ' << values.time << ' ' << lazy;
  auto drain = runtime.Drain();
  if (!WaitSucceeded(drain)) {
    return false;
  }
  std::optional<ulog::testing::ObservedRecord> record = destination.TryTake();
  return record && record->AdmissionSequence() == expected_sequence &&
         record->Message() == kAdvanced;
}

[[nodiscard]] bool Run() {
  ulog::testing::InMemoryDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 1024,
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
  const StreamFailure exception;
  // An exception writes its message and dynamic type, whose spelling depends on the compiler.
  const std::string streamed =
      "allocation-free-runtime: -1 18446744073709551615 0.5 true x view stream failure (" +
      std::string{ulog::detail::TypeName(typeid(StreamFailure))} + ")";
  const AdvancedValues advanced;
  if (!RunCycle(runtime, logger, destination, 0) ||
      !RunStreamCycle(runtime, logger, destination, 1, exception, streamed) ||
      !RunAdvancedCycle(runtime, logger, destination, 2, advanced)) {
    std::fputs("runtime allocation test: warm-up failed\n", stderr);
    return false;
  }

  const std::uint64_t allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  bool valid = true;
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    const std::uint64_t sequence = 3U + 3U * cycle;
    if (!RunCycle(runtime, logger, destination, sequence) ||
        !RunStreamCycle(runtime, logger, destination, sequence + 1U, exception, streamed) ||
        !RunAdvancedCycle(runtime, logger, destination, sequence + 2U, advanced)) {
      valid = false;
      break;
    }
  }

  logger.Log<ulog::Level::kInfo>(kSource, []() noexcept { return kMessage; });
  auto shutdown = runtime.Shutdown();
  valid = WaitSucceeded(shutdown) && valid;
  std::optional<ulog::testing::ObservedRecord> final_record = destination.TryTake();
  valid = final_record && final_record->AdmissionSequence() == 3U * kCycles + 3U &&
          final_record->Message() == kMessage && valid;

  const std::uint64_t allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const auto snapshot = runtime.GetSnapshot();
  const std::uint64_t expected_records = 3U * kCycles + 4U;
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
