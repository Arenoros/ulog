#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

#include "prototypes/record_storage/record_storage_kernel.hpp"
#include "support/benchmark_driver.hpp"
#include "support/workload_harness.hpp"

namespace {

using ulog::benchmark_support::LatencySummary;
using ulog::benchmark_support::Mode;
using ulog::benchmark_support::RecordFootprint;
using ulog::benchmark_support::WorkloadCase;
using ulog::benchmark_support::WorkloadResult;
using ulog::benchmark_support::record_storage::ChunkedRecordStorageKernel;
using ulog::benchmark_support::record_storage::ContiguousRecordStorageKernel;
using ulog::benchmark_support::record_storage::HybridRecordStorageKernel;

std::atomic<bool> deterministic_failure{false};
inline constexpr std::string_view kCandidateSchedule = "six-permutation-cycle";

void PublishLatency(benchmark::State& state, std::string_view prefix,
                    const LatencySummary& latency) {
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  const std::string sample_count_name = std::string{prefix} + "_latency_sample_count";
  const std::string p50_name = std::string{prefix} + "_latency_p50_ns";
  const std::string p99_name = std::string{prefix} + "_latency_p99_ns";
  const std::string p999_name = std::string{prefix} + "_latency_p999_ns";
  SetCounter(state, sample_count_name.c_str(), latency.sample_count);
  SetCounter(state, p50_name.c_str(), latency.p50_nanoseconds);
  SetCounter(state, p99_name.c_str(), latency.p99_nanoseconds);
  SetCounter(state, p999_name.c_str(), latency.p999_nanoseconds);
}

void PublishFootprint(benchmark::State& state, const RecordFootprint& footprint) {
  using ulog::benchmark_support::benchmark_driver::SetCounter;
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

void PublishResult(benchmark::State& state, const WorkloadResult& result,
                   std::uint64_t record_validation_error_count) {
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
  PublishLatency(state, "accepted", result.accepted_latency);
  PublishLatency(state, "rejected", result.rejected_latency);
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
  SetCounter(state, "record_validation_error_count", record_validation_error_count);
  PublishFootprint(state, result.record_footprint);
}

template <typename Kernel>
void PublishCandidateResult(benchmark::State& state, const WorkloadResult& result,
                            const Kernel& kernel) {
  PublishResult(state, result, kernel.record_validation_error_count());
}

template <typename Kernel>
bool HasDeterministicFailure(const WorkloadResult& result, const Kernel& kernel) {
  const std::uint64_t record_validation_error_count = kernel.record_validation_error_count();
  return result.allocation_count != 0U || result.allocation_failure_count != 0U ||
         result.accounting_error_count != 0U || result.retained_bound_error_count != 0U ||
         record_validation_error_count != 0U;
}

template <typename Kernel>
void RegisterCandidateWorkload(const WorkloadCase& workload) {
  ulog::benchmark_support::benchmark_driver::RegisterSingleIterationWorkload<Kernel>(
      "UlogRecordStorage", workload, deterministic_failure, PublishCandidateResult<Kernel>,
      HasDeterministicFailure<Kernel>,
      "Deterministic Record-storage checks failed; inspect allocation, accounting, and "
      "retained counters in the JSON result.");
}

template <typename First, typename Second, typename Third>
void RegisterCandidatePermutation(const WorkloadCase& workload) {
  RegisterCandidateWorkload<First>(workload);
  RegisterCandidateWorkload<Second>(workload);
  RegisterCandidateWorkload<Third>(workload);
}

void RegisterWorkload(const WorkloadCase& workload) {
  switch (workload.repetition % 6U) {
    case 0:
      RegisterCandidatePermutation<ContiguousRecordStorageKernel, ChunkedRecordStorageKernel,
                                   HybridRecordStorageKernel>(workload);
      return;
    case 1:
      RegisterCandidatePermutation<ContiguousRecordStorageKernel, HybridRecordStorageKernel,
                                   ChunkedRecordStorageKernel>(workload);
      return;
    case 2:
      RegisterCandidatePermutation<ChunkedRecordStorageKernel, ContiguousRecordStorageKernel,
                                   HybridRecordStorageKernel>(workload);
      return;
    case 3:
      RegisterCandidatePermutation<ChunkedRecordStorageKernel, HybridRecordStorageKernel,
                                   ContiguousRecordStorageKernel>(workload);
      return;
    case 4:
      RegisterCandidatePermutation<HybridRecordStorageKernel, ContiguousRecordStorageKernel,
                                   ChunkedRecordStorageKernel>(workload);
      return;
    case 5:
      RegisterCandidatePermutation<HybridRecordStorageKernel, ChunkedRecordStorageKernel,
                                   ContiguousRecordStorageKernel>(workload);
      return;
    default:
      std::terminate();
  }
}

void RegisterWorkloads(Mode mode) {
  const auto workloads = ulog::benchmark_support::MakeWorkloadMatrix(mode);
  for (const auto& workload : workloads) {
    RegisterWorkload(workload);
  }
  const std::size_t repetitions = workloads.empty() ? 0U : workloads.back().repetition + 1U;
  benchmark::AddCustomContext("ulog_result_protocol", "ulog-record-storage-results/2");
  benchmark::AddCustomContext("ulog_candidates", "contiguous-record,chunked-record,hybrid-record");
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
