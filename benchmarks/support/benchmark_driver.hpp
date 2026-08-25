#pragma once

#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "workload_harness.hpp"

namespace ulog::benchmark_support::benchmark_driver {

inline Mode ExtractModeArgument(int& argument_count, char** arguments) {
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

inline std::string WorkloadName(std::string_view benchmark_family, std::string_view candidate,
                                const WorkloadCase& workload) {
  return std::string{benchmark_family} + "/" + std::string{candidate} +
         "/producers:" + std::to_string(workload.producer_count) +
         "/record_bytes:" + std::to_string(workload.record_size_bytes) +
         "/occupancy:" + std::string{ToString(workload.occupancy)} +
         "/repetition:" + std::to_string(workload.repetition);
}

inline void SetCounter(benchmark::State& state, const char* name, std::uint64_t value) {
  state.counters[name] = static_cast<double>(value);
}

namespace detail {

inline void PublishLatency(benchmark::State& state, std::string_view prefix,
                           const LatencySummary& latency) {
  const std::string sample_count_name = std::string{prefix} + "_latency_sample_count";
  const std::string p50_name = std::string{prefix} + "_latency_p50_ns";
  const std::string p99_name = std::string{prefix} + "_latency_p99_ns";
  const std::string p999_name = std::string{prefix} + "_latency_p999_ns";
  SetCounter(state, sample_count_name.c_str(), latency.sample_count);
  SetCounter(state, p50_name.c_str(), latency.p50_nanoseconds);
  SetCounter(state, p99_name.c_str(), latency.p99_nanoseconds);
  SetCounter(state, p999_name.c_str(), latency.p999_nanoseconds);
}

inline void PublishFootprint(benchmark::State& state, const RecordFootprint& footprint) {
  SetCounter(state, "requested_message_bytes", footprint.requested_message_bytes);
  SetCounter(state, "stored_message_bytes", footprint.stored_message_bytes);
  SetCounter(state, "owned_payload_bytes", footprint.owned_payload_bytes);
  SetCounter(state, "metadata_bytes", footprint.metadata_bytes);
  SetCounter(state, "serialized_bytes", footprint.SerializedBytes());
  SetCounter(state, "fragmentation_bytes", footprint.fragmentation_bytes);
  SetCounter(state, "accounting_charge_bytes", footprint.accounting_charge_bytes);
  SetCounter(state, "minimum_accounting_charge_bytes", footprint.minimum_accounting_charge_bytes);
  SetCounter(state, "record_truncated", footprint.truncated ? 1U : 0U);
}

}  // namespace detail

inline void PublishWorkloadResult(benchmark::State& state, const WorkloadResult& result) {
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
  detail::PublishLatency(state, "accepted", result.accepted_latency);
  detail::PublishLatency(state, "rejected", result.rejected_latency);
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
  SetCounter(state, "truncated_records", result.truncated_records);
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
  detail::PublishFootprint(state, result.record_footprint);
}

template <typename Kernel, typename PublishResult, typename HasDeterministicFailure>
void RunSingleIterationWorkload(benchmark::State& state, WorkloadCase workload,
                                std::atomic<bool>& deterministic_failure,
                                const PublishResult& publish_result,
                                const HasDeterministicFailure& has_deterministic_failure,
                                const std::string& deterministic_failure_message) {
  std::unique_ptr<Kernel> kernel;
  std::optional<WorkloadResult> result;
  try {
    kernel = std::make_unique<Kernel>();
    for ([[maybe_unused]] const auto iteration : state) {
      result.emplace(RunWorkload(workload, *kernel));
      state.SetIterationTime(std::chrono::duration<double>{result->wall_time}.count());
    }
  } catch (const std::exception& error) {
    deterministic_failure.store(true, std::memory_order_relaxed);
    state.SkipWithError(error.what());
    return;
  }

  if (!result) {
    deterministic_failure.store(true, std::memory_order_relaxed);
    state.SkipWithError("The benchmark completed without running its single workload iteration.");
    return;
  }

  publish_result(state, *result, *kernel);
  if (has_deterministic_failure(*result, *kernel)) {
    deterministic_failure.store(true, std::memory_order_relaxed);
    state.SkipWithError(deterministic_failure_message.c_str());
  }
}

template <typename Kernel, typename PublishResult, typename HasDeterministicFailure>
void RegisterSingleIterationWorkload(std::string_view benchmark_family,
                                     const WorkloadCase& workload,
                                     std::atomic<bool>& deterministic_failure,
                                     PublishResult publish_result,
                                     HasDeterministicFailure has_deterministic_failure,
                                     std::string_view deterministic_failure_message) {
  const std::string benchmark_name = WorkloadName(benchmark_family, Kernel::Name(), workload);
  benchmark::RegisterBenchmark(
      benchmark_name.c_str(),
      [workload, &deterministic_failure, publish_result, has_deterministic_failure,
       deterministic_failure_message =
           std::string{deterministic_failure_message}](benchmark::State& state) {
        RunSingleIterationWorkload<Kernel>(state, workload, deterministic_failure, publish_result,
                                           has_deterministic_failure,
                                           deterministic_failure_message);
      })
      ->Iterations(1)
      ->UseManualTime();
}

template <typename RegisterWorkloads>
int RunMain(int argument_count, char** arguments, std::atomic<bool>& deterministic_failure,
            const RegisterWorkloads& register_workloads) {
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
  register_workloads(mode);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return deterministic_failure.load(std::memory_order_relaxed) ? 1 : 0;
}

}  // namespace ulog::benchmark_support::benchmark_driver
