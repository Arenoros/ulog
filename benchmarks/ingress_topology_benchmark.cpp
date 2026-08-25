#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#include "prototypes/ingress/ingress_kernel.hpp"
#include "support/benchmark_driver.hpp"
#include "support/workload_harness.hpp"

namespace {

using ulog::benchmark_support::Mode;
using ulog::benchmark_support::WorkloadCase;
using ulog::benchmark_support::WorkloadResult;
using ulog::benchmark_support::ingress::BoundedRingIngressKernel;
using ulog::benchmark_support::ingress::ChunkedMpscIngressKernel;
using ulog::benchmark_support::ingress::PerProducerLanesIngressKernel;
using ulog::benchmark_support::ingress::TopologySnapshot;

inline constexpr std::string_view kCandidateSchedule = "six-permutation-cycle";

static_assert(ulog::benchmark_support::WorkloadKernel<BoundedRingIngressKernel>);
static_assert(ulog::benchmark_support::WorkloadKernel<ChunkedMpscIngressKernel>);
static_assert(ulog::benchmark_support::WorkloadKernel<PerProducerLanesIngressKernel>);

std::atomic<bool> deterministic_failure{false};

void PublishTopologySnapshot(benchmark::State& state, const TopologySnapshot& snapshot) {
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  SetCounter(state, "topology_attempted_records", snapshot.attempted_records);
  SetCounter(state, "topology_enqueued_records", snapshot.enqueued_records);
  SetCounter(state, "topology_dequeued_records", snapshot.dequeued_records);
  SetCounter(state, "topology_rejected_records", snapshot.rejected_records);
  SetCounter(state, "topology_full_rejections", snapshot.full_rejections);
  SetCounter(state, "topology_contention_rejections", snapshot.contention_rejections);
  SetCounter(state, "topology_invalid_rejections", snapshot.invalid_rejections);
  SetCounter(state, "topology_retained_records", snapshot.retained_records);
  SetCounter(state, "topology_retained_serialized_bytes", snapshot.retained_serialized_bytes);
  SetCounter(state, "topology_retained_charge_bytes", snapshot.retained_charge_bytes);
}

template <typename Kernel>
void PublishCandidateResult(benchmark::State& state, const WorkloadResult& result,
                            const Kernel& kernel) {
  using ulog::benchmark_support::benchmark_driver::PublishWorkloadResult;
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  PublishWorkloadResult(state, result);
  SetCounter(state, "maximum_accepted_per_round", result.maximum_accepted_per_round);
  PublishTopologySnapshot(state, kernel.MeasurementTopologySnapshot());
  SetCounter(state, "fifo_error_count", kernel.fifo_error_count());
  SetCounter(state, "sequence_error_count", kernel.sequence_error_count());
  SetCounter(state, "record_validation_error_count", kernel.record_validation_error_count());
  SetCounter(state, "maximum_publication_actions_observed",
             static_cast<std::uint64_t>(kernel.maximum_publication_actions_observed()));
  SetCounter(state, "publication_action_limit",
             static_cast<std::uint64_t>(Kernel::publication_action_limit()));
}

bool HasTopologyAccountingFailure(const WorkloadResult& result,
                                  const TopologySnapshot& snapshot) noexcept {
  return snapshot.attempted_records != snapshot.enqueued_records + snapshot.rejected_records ||
         snapshot.rejected_records != snapshot.full_rejections + snapshot.contention_rejections +
                                          snapshot.invalid_rejections ||
         snapshot.enqueued_records != snapshot.dequeued_records + snapshot.retained_records ||
         snapshot.enqueued_records != result.accepted_records ||
         snapshot.attempted_records > result.attempted_records ||
         snapshot.rejected_records > result.rejected_records || snapshot.invalid_rejections != 0U ||
         snapshot.retained_records != 0U || snapshot.retained_serialized_bytes != 0U ||
         snapshot.retained_charge_bytes != 0U;
}

template <typename Kernel>
bool HasDeterministicFailure(const WorkloadResult& result, const Kernel& kernel) {
  const TopologySnapshot topology = kernel.MeasurementTopologySnapshot();
  const std::size_t maximum_actions = kernel.maximum_publication_actions_observed();
  const std::size_t action_limit = Kernel::publication_action_limit();
  const bool failed =
      result.allocation_count != 0U || result.allocation_failure_count != 0U ||
      result.accounting_error_count != 0U || result.retained_bound_error_count != 0U ||
      HasTopologyAccountingFailure(result, topology) || kernel.fifo_error_count() != 0U ||
      kernel.sequence_error_count() != 0U || kernel.record_validation_error_count() != 0U ||
      action_limit == 0U || maximum_actions > action_limit ||
      (topology.attempted_records != 0U && maximum_actions == 0U);
  if (failed) {
    std::cerr << Kernel::Name()
              << " deterministic failure: workload accepted=" << result.accepted_records
              << ", rejected=" << result.rejected_records
              << "; topology attempted=" << topology.attempted_records
              << ", enqueued=" << topology.enqueued_records
              << ", dequeued=" << topology.dequeued_records
              << ", rejected=" << topology.rejected_records
              << ", retained=" << topology.retained_records
              << "; errors fifo=" << kernel.fifo_error_count()
              << ", sequence=" << kernel.sequence_error_count()
              << ", record=" << kernel.record_validation_error_count()
              << "; actions=" << maximum_actions << '/' << action_limit << '\n';
  }
  return failed;
}

template <typename Kernel>
void RegisterCandidateWorkload(const WorkloadCase& workload) {
  ulog::benchmark_support::benchmark_driver::RegisterSingleIterationWorkload<Kernel>(
      "UlogIngressTopology", workload, deterministic_failure, PublishCandidateResult<Kernel>,
      HasDeterministicFailure<Kernel>,
      "Deterministic ingress checks failed; inspect topology accounting, retained bytes, FIFO, "
      "sequence, Record validation, and publication-action counters in the JSON result.");
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
      RegisterCandidatePermutation<BoundedRingIngressKernel, ChunkedMpscIngressKernel,
                                   PerProducerLanesIngressKernel>(workload);
      return;
    case 1:
      RegisterCandidatePermutation<BoundedRingIngressKernel, PerProducerLanesIngressKernel,
                                   ChunkedMpscIngressKernel>(workload);
      return;
    case 2:
      RegisterCandidatePermutation<ChunkedMpscIngressKernel, BoundedRingIngressKernel,
                                   PerProducerLanesIngressKernel>(workload);
      return;
    case 3:
      RegisterCandidatePermutation<ChunkedMpscIngressKernel, PerProducerLanesIngressKernel,
                                   BoundedRingIngressKernel>(workload);
      return;
    case 4:
      RegisterCandidatePermutation<PerProducerLanesIngressKernel, BoundedRingIngressKernel,
                                   ChunkedMpscIngressKernel>(workload);
      return;
    case 5:
      RegisterCandidatePermutation<PerProducerLanesIngressKernel, ChunkedMpscIngressKernel,
                                   BoundedRingIngressKernel>(workload);
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
  benchmark::AddCustomContext("ulog_result_protocol", "ulog-ingress-results/1");
  benchmark::AddCustomContext("ulog_candidates",
                              "bounded-mpsc-ring,chunked-mpsc,per-producer-lanes");
  benchmark::AddCustomContext("ulog_candidate_schedule", std::string{kCandidateSchedule});
  benchmark::AddCustomContext("ulog_mode", std::string{ulog::benchmark_support::ToString(mode)});
  benchmark::AddCustomContext("ulog_timing_policy", "advisory");
  benchmark::AddCustomContext("ulog_repetitions", std::to_string(repetitions));
  benchmark::AddCustomContext("ulog_publication_action_unit",
                              "bounded topology actions per TryPublish call");
}

}  // namespace

int main(int argument_count, char** arguments) {
  return ulog::benchmark_support::benchmark_driver::RunMain(
      argument_count, arguments, deterministic_failure, RegisterWorkloads);
}
