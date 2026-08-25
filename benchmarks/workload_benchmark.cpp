#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "prototypes/reservation/central_reservation_kernel.hpp"
#include "prototypes/reservation/producer_credit_reservation_kernel.hpp"
#include "support/benchmark_driver.hpp"
#include "support/workload_harness.hpp"

namespace {

using ulog::benchmark_support::Mode;
using ulog::benchmark_support::WorkloadCase;
using ulog::benchmark_support::WorkloadResult;
using ulog::benchmark_support::reservation::CentralReservationKernel;
using ulog::benchmark_support::reservation::ProducerCreditReservationKernel;

std::atomic<bool> deterministic_failure{false};
inline constexpr std::string_view kCandidateSchedule = "paired-alternating";

void PublishResult(benchmark::State& state, const WorkloadResult& result) {
  using ulog::benchmark_support::benchmark_driver::SetCounter;
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
void PublishCandidateResult(benchmark::State& state, const WorkloadResult& result, const Kernel&) {
  PublishResult(state, result);
}

template <typename Kernel>
bool HasDeterministicFailure(const WorkloadResult& result, const Kernel&) {
  return result.accounting_error_count != 0U || result.retained_bound_error_count != 0U ||
         result.allocation_failure_count != 0U;
}

template <typename Kernel>
void RegisterCandidateWorkload(const WorkloadCase& workload) {
  ulog::benchmark_support::benchmark_driver::RegisterSingleIterationWorkload<Kernel>(
      "UlogWorkload", workload, deterministic_failure, PublishCandidateResult<Kernel>,
      HasDeterministicFailure<Kernel>,
      "Deterministic workload checks failed; inspect accounting, allocation, and retained "
      "counters in the JSON result.");
}

void RegisterWorkloads(Mode mode) {
  const auto workloads = ulog::benchmark_support::MakeWorkloadMatrix(mode);
  for (const auto& workload : workloads) {
    if (workload.repetition % 2U == 0U) {
      RegisterCandidateWorkload<CentralReservationKernel>(workload);
      RegisterCandidateWorkload<ProducerCreditReservationKernel>(workload);
    } else {
      RegisterCandidateWorkload<ProducerCreditReservationKernel>(workload);
      RegisterCandidateWorkload<CentralReservationKernel>(workload);
    }
  }
  const std::size_t repetitions = workloads.empty() ? 0U : workloads.back().repetition + 1U;
  benchmark::AddCustomContext("ulog_result_protocol", "ulog-workload-results/3");
  benchmark::AddCustomContext("ulog_candidates", "central-reservation,producer-credit-reservation");
  benchmark::AddCustomContext("ulog_candidate_schedule", std::string{kCandidateSchedule});
  benchmark::AddCustomContext("ulog_mode", std::string{ulog::benchmark_support::ToString(mode)});
  benchmark::AddCustomContext("ulog_timing_policy", "advisory");
  benchmark::AddCustomContext("ulog_repetitions", std::to_string(repetitions));
}

}  // namespace

int main(int argument_count, char** arguments) {
  return ulog::benchmark_support::benchmark_driver::RunMain(
      argument_count, arguments, deterministic_failure, RegisterWorkloads);
}
