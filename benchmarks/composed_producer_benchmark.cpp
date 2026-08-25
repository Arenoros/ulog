#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

#include "prototypes/composed/composed_producer_kernel.hpp"
#include "support/benchmark_driver.hpp"
#include "support/workload_harness.hpp"

namespace {

using ulog::benchmark_support::Mode;
using ulog::benchmark_support::WorkloadCase;
using ulog::benchmark_support::WorkloadResult;
using ulog::benchmark_support::composed::ComposedProducerKernel;
using ulog::benchmark_support::ingress::TopologySnapshot;

std::atomic<bool> deterministic_failure{false};

void PublishTopology(benchmark::State& state, const TopologySnapshot& topology) {
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  SetCounter(state, "topology_attempted_records", topology.attempted_records);
  SetCounter(state, "topology_enqueued_records", topology.enqueued_records);
  SetCounter(state, "topology_dequeued_records", topology.dequeued_records);
  SetCounter(state, "topology_rejected_records", topology.rejected_records);
  SetCounter(state, "topology_full_rejections", topology.full_rejections);
  SetCounter(state, "topology_contention_rejections", topology.contention_rejections);
  SetCounter(state, "topology_invalid_rejections", topology.invalid_rejections);
  SetCounter(state, "topology_retained_records", topology.retained_records);
  SetCounter(state, "topology_retained_serialized_bytes", topology.retained_serialized_bytes);
  SetCounter(state, "topology_retained_charge_bytes", topology.retained_charge_bytes);
}

void PublishResult(benchmark::State& state, const WorkloadResult& result,
                   const ComposedProducerKernel& kernel) {
  using ulog::benchmark_support::benchmark_driver::PublishWorkloadResult;
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  PublishWorkloadResult(state, result);
  SetCounter(state, "maximum_accepted_per_round", result.maximum_accepted_per_round);
  SetCounter(state, "message_callback_count", kernel.message_callback_count());
  SetCounter(state, "context_callback_count", kernel.context_callback_count());
  SetCounter(state, "fifo_error_count", kernel.fifo_error_count());
  SetCounter(state, "record_validation_error_count", kernel.record_validation_error_count());
  SetCounter(state, "publication_error_count", kernel.publication_error_count());
  SetCounter(state, "lifecycle_error_count", kernel.lifecycle_error_count());
  PublishTopology(state, kernel.MeasurementTopologySnapshot());
}

bool HasDeterministicFailure(const WorkloadResult& result, const ComposedProducerKernel& kernel) {
  const auto topology = kernel.MeasurementTopologySnapshot();
  const bool failed =
      result.allocation_count != 0U || result.allocation_failure_count != 0U ||
      result.accounting_error_count != 0U || result.retained_bound_error_count != 0U ||
      kernel.message_callback_count() != result.accepted_records ||
      kernel.context_callback_count() != result.accepted_records ||
      topology.enqueued_records != result.accepted_records ||
      topology.dequeued_records != result.accepted_records || topology.rejected_records != 0U ||
      topology.retained_records != 0U || topology.retained_serialized_bytes != 0U ||
      topology.retained_charge_bytes != 0U || kernel.fifo_error_count() != 0U ||
      kernel.record_validation_error_count() != 0U || kernel.publication_error_count() != 0U ||
      kernel.lifecycle_error_count() != 0U;
  if (failed) {
    std::cerr << "composed-producer deterministic failure: attempted=" << result.attempted_records
              << ", accepted=" << result.accepted_records
              << ", rejected=" << result.rejected_records
              << "; callbacks message=" << kernel.message_callback_count()
              << ", context=" << kernel.context_callback_count()
              << "; topology enqueued=" << topology.enqueued_records
              << ", dequeued=" << topology.dequeued_records
              << ", retained=" << topology.retained_records
              << "; errors fifo=" << kernel.fifo_error_count()
              << ", record=" << kernel.record_validation_error_count()
              << ", publication=" << kernel.publication_error_count()
              << ", lifecycle=" << kernel.lifecycle_error_count() << '\n';
  }
  return failed;
}

void RegisterWorkloads(Mode mode) {
  const auto workloads = ulog::benchmark_support::MakeWorkloadMatrix(mode);
  for (const WorkloadCase& workload : workloads) {
    ulog::benchmark_support::benchmark_driver::RegisterSingleIterationWorkload<
        ComposedProducerKernel>(
        "UlogComposedProducer", workload, deterministic_failure, PublishResult,
        HasDeterministicFailure,
        "Deterministic composed-producer checks failed; inspect callback, topology, FIFO, "
        "Record, accounting, and retained counters in the JSON result.");
  }
  const std::size_t repetitions = workloads.empty() ? 0U : workloads.back().repetition + 1U;
  benchmark::AddCustomContext("ulog_result_protocol", "ulog-composed-producer-results/1");
  benchmark::AddCustomContext("ulog_candidates", "composed-producer");
  benchmark::AddCustomContext("ulog_candidate_schedule", "single-candidate");
  benchmark::AddCustomContext("ulog_mode", std::string{ulog::benchmark_support::ToString(mode)});
  benchmark::AddCustomContext("ulog_timing_policy", "advisory");
  benchmark::AddCustomContext("ulog_repetitions", std::to_string(repetitions));
}

}  // namespace

int main(int argument_count, char** arguments) {
  return ulog::benchmark_support::benchmark_driver::RunMain(
      argument_count, arguments, deterministic_failure, RegisterWorkloads);
}
