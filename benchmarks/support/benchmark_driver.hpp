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
