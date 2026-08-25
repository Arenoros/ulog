#include "support/workload_harness.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "support/reference_kernel.hpp"

namespace {

using ulog::benchmark_support::Mode;
using ulog::benchmark_support::Occupancy;
using ulog::benchmark_support::ReferenceLedgerKernel;
using ulog::benchmark_support::WorkloadCase;

inline constexpr std::string_view kInjectedLaunchFailure = "injected thread launch failure";

struct LaunchState final {
  std::atomic<std::size_t> started_workers{0};
  std::atomic<std::size_t> finished_workers{0};
};

class FailingThreadLauncher final {
 public:
  explicit FailingThreadLauncher(LaunchState& state) noexcept : state_(state) {}

  template <typename Worker>
  void operator()(std::vector<std::thread>& threads, Worker&& worker) {
    constexpr std::size_t kSuccessfulLaunches = 2;
    if (launch_count_ == kSuccessfulLaunches) {
      throw std::runtime_error(std::string{kInjectedLaunchFailure});
    }
    ++launch_count_;
    LaunchState& state = state_.get();
    threads.emplace_back([&state, worker = std::forward<Worker>(worker)]() mutable {
      state.started_workers.fetch_add(1, std::memory_order_relaxed);
      worker();
      state.finished_workers.fetch_add(1, std::memory_order_relaxed);
    });
  }

 private:
  std::reference_wrapper<LaunchState> state_;
  std::size_t launch_count_{0};
};

bool Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool TestMatrix() {
  const auto workloads = ulog::benchmark_support::MakeWorkloadMatrix(Mode::kSmoke);
  std::set<std::tuple<std::size_t, std::size_t, Occupancy>> cells;
  for (const auto& workload : workloads) {
    cells.emplace(workload.producer_count, workload.record_size_bytes, workload.occupancy);
  }

  if (!Check(!workloads.empty(), "smoke matrix must not be empty")) {
    return false;
  }
  bool success = true;
  success &= Check(workloads.size() == 120U, "smoke matrix must contain 120 workloads");
  success &= Check(cells.size() == 120U, "smoke matrix cells must be unique");
  success &= Check(workloads.front().producer_count == 1U, "matrix must start with one producer");
  success &= Check(workloads.back().producer_count == 32U, "matrix must end with 32 producers");

  const auto controlled = ulog::benchmark_support::MakeWorkloadMatrix(Mode::kControlled);
  success &= Check(controlled.size() == 840U,
                   "controlled matrix must contain seven repetitions of 120 workloads");
  for (const auto& workload : controlled) {
    success &= Check(workload.producer_count * workload.measured_rounds >= 100'000U,
                     "controlled workload must collect at least 100000 samples");
  }
  return success;
}

bool TestOccupancyAndExpectedAdmission() {
  constexpr std::array expected_occupancy{
      std::size_t{0},
      std::size_t{524'288},
      std::size_t{1'032'192},
      std::size_t{1'048'576},
  };
  constexpr std::array occupancies{
      Occupancy::kEmpty,
      Occupancy::kPartial,
      Occupancy::kNearFull,
      Occupancy::kSaturated,
  };

  bool success = true;
  for (std::size_t index = 0; index < occupancies.size(); ++index) {
    WorkloadCase workload{
        .producer_count = 32,
        .record_size_bytes = 16'384,
        .occupancy = occupancies[index],
        .capacity_bytes = 1'048'576,
        .warmup_rounds = 1,
        .measured_rounds = 1,
        .repetition = 0,
    };
    success &=
        Check(ulog::benchmark_support::InitialOccupancyBytes(workload) == expected_occupancy[index],
              "occupancy bytes must match the fixed matrix");
  }

  WorkloadCase near_full{
      .producer_count = 32,
      .record_size_bytes = 16'384,
      .occupancy = Occupancy::kNearFull,
      .capacity_bytes = 1'048'576,
      .warmup_rounds = 1,
      .measured_rounds = 1,
      .repetition = 0,
  };
  success &= Check(ulog::benchmark_support::ExpectedAcceptedPerRound(near_full) == 1U,
                   "near-full 16KiB workload must admit one producer per wave");
  success &= Check(ulog::benchmark_support::ExpectedAcceptedPerRound(near_full, 16'448U) == 0U,
                   "candidate-specific charge must control near-full admission");
  near_full.occupancy = Occupancy::kSaturated;
  success &= Check(ulog::benchmark_support::ExpectedAcceptedPerRound(near_full) == 0U,
                   "saturated workload must reject every producer");
  return success;
}

bool TestRecordFootprintValidation() {
  WorkloadCase workload{
      .producer_count = 1,
      .record_size_bytes = 64,
      .occupancy = Occupancy::kEmpty,
      .capacity_bytes = 1'048'576,
      .warmup_rounds = 1,
      .measured_rounds = 1,
      .repetition = 0,
  };
  const auto valid = ulog::benchmark_support::MakePayloadOnlyRecordFootprint(64);
  ulog::benchmark_support::ValidateRecordFootprint(workload, valid);

  auto invalid = valid;
  invalid.fragmentation_bytes = 64;
  bool mismatch_rejected = false;
  try {
    ulog::benchmark_support::ValidateRecordFootprint(workload, invalid);
  } catch (const std::invalid_argument&) {
    mismatch_rejected = true;
  }
  return Check(mismatch_rejected,
               "footprint validation must reject a charge that omits fragmentation");
}

bool TestNearestRankPercentiles() {
  const std::vector<std::uint64_t> samples{9, 1, 5, 3, 7};
  const auto summary = ulog::benchmark_support::ComputeLatencySummary(samples);
  return Check(summary.sample_count == 5U, "latency summary must retain the sample count") &&
         Check(summary.p50_nanoseconds == 5U, "p50 must use nearest rank") &&
         Check(summary.p99_nanoseconds == 9U, "p99 must use nearest rank") &&
         Check(summary.p999_nanoseconds == 9U, "p99.9 must use nearest rank");
}

bool TestBarrieredReferenceRun() {
  WorkloadCase workload{
      .producer_count = 4,
      .record_size_bytes = 16'384,
      .occupancy = Occupancy::kNearFull,
      .capacity_bytes = 1'048'576,
      .warmup_rounds = 2,
      .measured_rounds = 3,
      .repetition = 0,
  };
  ReferenceLedgerKernel kernel;
  const auto result = ulog::benchmark_support::RunWorkload(workload, kernel);

  bool success = true;
  success &= Check(result.attempted_records == 12U, "run must account for every attempt");
  success &=
      Check(result.accepted_records == 3U, "each near-full wave must admit exactly one record");
  success &= Check(result.rejected_records == 9U, "remaining near-full attempts must be rejected");
  success &= Check(result.accepted_bytes == 49'152U,
                   "accepted byte accounting must match accepted records");
  success &=
      Check(result.allocation_count == 0U, "reference ledger measured path must not allocate");
  success &= Check(result.allocation_failure_count == 0U,
                   "reference ledger must not report allocation failures");
  success &= Check(result.logical_retained_initial_bytes == 1'032'192U,
                   "reference run must start at near-full occupancy");
  success &= Check(result.logical_retained_high_water_bytes == 1'048'576U,
                   "reference run must reach but not exceed the payload limit");
  success &= Check(result.logical_retained_final_bytes == 1'032'192U,
                   "accepted tickets must be released after every wave");
  success &=
      Check(result.accounting_error_count == 0U, "reference run accounting checks must pass");
  success &= Check(result.retained_bound_error_count == 0U,
                   "reference run retained-bound checks must pass");
  success &= Check(result.latency.sample_count == result.attempted_records,
                   "latency sample count must equal producer attempts");
  success &= Check(result.accepted_latency.sample_count == result.accepted_records,
                   "accepted latency count must equal accepted attempts");
  success &= Check(result.rejected_latency.sample_count == result.rejected_records,
                   "rejected latency count must equal rejected attempts");
  success &= Check(result.record_footprint.accounting_charge_bytes == 16'384U,
                   "reference footprint charge must equal the payload size");
  success &= Check(result.truncated_records == 0U,
                   "the reference payload-only kernel must not report truncation");
  success &= Check(result.latency.p50_nanoseconds <= result.latency.p99_nanoseconds &&
                       result.latency.p99_nanoseconds <= result.latency.p999_nanoseconds,
                   "producer latency percentiles must be monotonic");
  success &= Check(result.wall_time > std::chrono::nanoseconds::zero(),
                   "measured wall time must be positive");
  return success;
}

bool TestPartialThreadLaunchFailure() {
  WorkloadCase workload{
      .producer_count = 4,
      .record_size_bytes = 64,
      .occupancy = Occupancy::kEmpty,
      .capacity_bytes = 1'048'576,
      .warmup_rounds = 1,
      .measured_rounds = 1,
      .repetition = 0,
  };
  ReferenceLedgerKernel kernel;
  LaunchState launch_state;

  bool original_error_rethrown = false;
  try {
    static_cast<void>(ulog::benchmark_support::RunWorkload(workload, kernel,
                                                           FailingThreadLauncher{launch_state}));
  } catch (const std::runtime_error& error) {
    original_error_rethrown = std::string_view{error.what()} == kInjectedLaunchFailure;
  }

  return Check(original_error_rethrown, "partial launch must rethrow the original error") &&
         Check(launch_state.started_workers.load(std::memory_order_relaxed) == 2U,
               "partial launch must start the workers created before the failure") &&
         Check(launch_state.finished_workers.load(std::memory_order_relaxed) == 2U,
               "partial launch must release and join every started worker");
}

}  // namespace

int main() {
  const bool success = TestMatrix() && TestOccupancyAndExpectedAdmission() &&
                       TestRecordFootprintValidation() && TestNearestRankPercentiles() &&
                       TestBarrieredReferenceRun() && TestPartialThreadLaunchFailure();
  return success ? 0 : 1;
}
