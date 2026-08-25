#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "prototypes/reservation/central_reservation_kernel.hpp"
#include "prototypes/reservation/producer_credit_reservation_kernel.hpp"
#include "support/workload_harness.hpp"

namespace {

using ulog::benchmark_support::Mode;
using ulog::benchmark_support::WorkloadCase;
using ulog::benchmark_support::WorkloadResult;
using ulog::benchmark_support::reservation::CentralReservationKernel;
using ulog::benchmark_support::reservation::ProducerCreditReservationKernel;

std::atomic<bool> deterministic_failure{false};

Mode ExtractModeArgument(int& argument_count, char** arguments) {
  constexpr std::string_view kModePrefix = "--ulog_mode=";
  Mode mode = Mode::kSmoke;
  bool found_mode = false;
  int output_index = 1;
  for (int input_index = 1; input_index < argument_count; ++input_index) {
    const std::string_view argument{arguments[input_index]};
    if (!argument.starts_with(kModePrefix)) {
      arguments[output_index] = arguments[input_index];
      ++output_index;
      continue;
    }
    if (found_mode) {
      throw std::invalid_argument(
          "--ulog_mode may be specified only once; use --ulog_mode=smoke or "
          "--ulog_mode=controlled.");
    }
    found_mode = true;
    const std::string_view value = argument.substr(kModePrefix.size());
    if (value == "smoke") {
      mode = Mode::kSmoke;
    } else if (value == "controlled") {
      mode = Mode::kControlled;
    } else {
      throw std::invalid_argument(
          "Unknown --ulog_mode value; use --ulog_mode=smoke or --ulog_mode=controlled.");
    }
  }
  argument_count = output_index;
  return mode;
}

template <typename Kernel>
std::string WorkloadName(const WorkloadCase& workload) {
  return "UlogWorkload/" + std::string{Kernel::Name()} +
         "/producers:" + std::to_string(workload.producer_count) +
         "/record_bytes:" + std::to_string(workload.record_size_bytes) +
         "/occupancy:" + std::string{ulog::benchmark_support::ToString(workload.occupancy)} +
         "/repetition:" + std::to_string(workload.repetition);
}

void SetCounter(benchmark::State& state, const char* name, std::uint64_t value) {
  state.counters[name] = static_cast<double>(value);
}

void PublishResult(benchmark::State& state, const WorkloadResult& result) {
  SetCounter(state, "producer_count", result.workload.producer_count);
  SetCounter(state, "record_size_bytes", result.workload.record_size_bytes);
  SetCounter(state, "workload_repetition_index", result.workload.repetition);
  SetCounter(state, "warmup_rounds", result.workload.warmup_rounds);
  SetCounter(state, "measured_rounds", result.workload.measured_rounds);
  SetCounter(state, "sample_count", result.latency.sample_count);
  SetCounter(state, "wall_time_ns", static_cast<std::uint64_t>(result.wall_time.count()));
  SetCounter(state, "process_cpu_time_ns",
             static_cast<std::uint64_t>(result.process_cpu_time.count()));
  state.counters["cpu_utilization_percent"] = result.cpu_utilization_percent;
  SetCounter(state, "producer_latency_p50_ns", result.latency.p50_nanoseconds);
  SetCounter(state, "producer_latency_p99_ns", result.latency.p99_nanoseconds);
  SetCounter(state, "producer_latency_p999_ns", result.latency.p999_nanoseconds);
  state.counters["attempts_per_second"] = result.attempts_per_second;
  state.counters["records_per_second"] = result.records_per_second;
  state.counters["bytes_per_second"] = result.bytes_per_second;
  SetCounter(state, "attempted_records", result.attempted_records);
  SetCounter(state, "accepted_records", result.accepted_records);
  SetCounter(state, "rejected_records", result.rejected_records);
  SetCounter(state, "accepted_bytes", result.accepted_bytes);
  SetCounter(state, "rejected_bytes", result.rejected_bytes);
  SetCounter(state, "allocation_count", result.allocation_count);
  SetCounter(state, "allocation_failure_count", result.allocation_failure_count);
  SetCounter(state, "logical_retained_initial_bytes", result.logical_retained_initial_bytes);
  SetCounter(state, "logical_retained_high_water_bytes", result.logical_retained_high_water_bytes);
  SetCounter(state, "logical_retained_final_bytes", result.logical_retained_final_bytes);
  SetCounter(state, "logical_retained_limit_bytes", result.logical_retained_limit_bytes);
  SetCounter(state, "physical_retained_initial_bytes", result.physical_retained_initial_bytes);
  SetCounter(state, "physical_retained_high_water_bytes",
             result.physical_retained_high_water_bytes);
  SetCounter(state, "physical_retained_final_bytes", result.physical_retained_final_bytes);
  SetCounter(state, "physical_retained_limit_bytes", result.physical_retained_limit_bytes);
  SetCounter(state, "accounting_error_count", result.accounting_error_count);
  SetCounter(state, "retained_bound_error_count", result.retained_bound_error_count);
}

template <typename Kernel>
void RunCandidateWorkload(benchmark::State& state, WorkloadCase workload) {
  Kernel kernel;
  std::optional<WorkloadResult> result;
  try {
    for ([[maybe_unused]] const auto iteration : state) {
      result.emplace(ulog::benchmark_support::RunWorkload(workload, kernel));
      state.SetIterationTime(std::chrono::duration<double>{result->wall_time}.count());
    }
  } catch (const std::exception& error) {
    deterministic_failure.store(true, std::memory_order_relaxed);
    state.SkipWithError(error.what());
    return;
  }

  PublishResult(state, *result);
  if (result->accounting_error_count != 0U || result->retained_bound_error_count != 0U ||
      result->allocation_failure_count != 0U) {
    deterministic_failure.store(true, std::memory_order_relaxed);
    state.SkipWithError(
        "Deterministic workload checks failed; inspect accounting, allocation, and retained "
        "counters in the JSON result.");
  }
}

template <typename Kernel>
void RegisterCandidate(const std::vector<WorkloadCase>& workloads) {
  for (const auto& workload : workloads) {
    benchmark::RegisterBenchmark(WorkloadName<Kernel>(workload).c_str(),
                                 RunCandidateWorkload<Kernel>, workload)
        ->Iterations(1)
        ->UseManualTime();
  }
}

void RegisterWorkloads(Mode mode) {
  const auto workloads = ulog::benchmark_support::MakeWorkloadMatrix(mode);
  RegisterCandidate<CentralReservationKernel>(workloads);
  RegisterCandidate<ProducerCreditReservationKernel>(workloads);
  const std::size_t repetitions = workloads.empty() ? 0U : workloads.back().repetition + 1U;
  benchmark::AddCustomContext("ulog_result_protocol", "ulog-workload-results/2");
  benchmark::AddCustomContext("ulog_candidates", "central-reservation,producer-credit-reservation");
  benchmark::AddCustomContext("ulog_mode", std::string{ulog::benchmark_support::ToString(mode)});
  benchmark::AddCustomContext("ulog_timing_policy", "advisory");
  benchmark::AddCustomContext("ulog_repetitions", std::to_string(repetitions));
}

}  // namespace

int main(int argument_count, char** arguments) {
  Mode mode = Mode::kSmoke;
  try {
    mode = ExtractModeArgument(argument_count, arguments);
  } catch (const std::invalid_argument& error) {
    std::cerr << "error: " << error.what() << '\n';
    return 2;
  }

  benchmark::Initialize(&argument_count, arguments);
  if (benchmark::ReportUnrecognizedArguments(argument_count, arguments)) {
    return 2;
  }
  RegisterWorkloads(mode);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return deterministic_failure.load(std::memory_order_relaxed) ? 1 : 0;
}
