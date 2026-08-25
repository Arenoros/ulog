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

using ulog::benchmark_support::Mode;
using ulog::benchmark_support::WorkloadCase;
using ulog::benchmark_support::WorkloadResult;
using ulog::benchmark_support::record_storage::ChunkedRecordStorageKernel;
using ulog::benchmark_support::record_storage::ContiguousRecordStorageKernel;
using ulog::benchmark_support::record_storage::HybridRecordStorageKernel;

std::atomic<bool> deterministic_failure{false};
inline constexpr std::string_view kCandidateSchedule = "six-permutation-cycle";

template <typename Kernel>
void PublishCandidateResult(benchmark::State& state, const WorkloadResult& result,
                            const Kernel& kernel) {
  using ulog::benchmark_support::benchmark_driver::PublishWorkloadResult;
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  PublishWorkloadResult(state, result);
  SetCounter(state, "record_validation_error_count", kernel.record_validation_error_count());
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
