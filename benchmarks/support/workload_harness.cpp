#include "support/workload_harness.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace ulog::benchmark_support {
namespace {

inline constexpr std::array<std::size_t, 6> kProducerCounts{1, 2, 4, 8, 16, 32};
inline constexpr std::array<std::size_t, 5> kRecordSizes{64, 256, 1'024, 4'096, 16'384};
inline constexpr std::array<Occupancy, 4> kOccupancies{
    Occupancy::kEmpty,
    Occupancy::kPartial,
    Occupancy::kNearFull,
    Occupancy::kSaturated,
};
inline constexpr std::size_t kSmokeWarmupRounds = 1;
inline constexpr std::size_t kSmokeMeasuredRounds = 1;
inline constexpr std::size_t kControlledWarmupRounds = 64;
inline constexpr std::size_t kControlledMinimumMeasuredRounds = 64;
inline constexpr std::size_t kControlledMinimumSamplesPerCell = 100'000;
inline constexpr std::size_t kControlledRepetitions = 7;

template <typename Value, std::size_t Size>
bool Contains(const std::array<Value, Size>& values, const Value& candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

std::uint64_t NearestRank(const std::vector<std::uint64_t>& sorted_samples, std::size_t numerator) {
  constexpr std::size_t kDenominator = 1'000;
  const std::size_t rank = (sorted_samples.size() * numerator + kDenominator - 1U) / kDenominator;
  return sorted_samples[rank - 1U];
}

}  // namespace

std::string_view ToString(Mode mode) noexcept {
  switch (mode) {
    case Mode::kSmoke:
      return "smoke";
    case Mode::kControlled:
      return "controlled";
  }
  return "unknown";
}

std::string_view ToString(Occupancy occupancy) noexcept {
  switch (occupancy) {
    case Occupancy::kEmpty:
      return "empty";
    case Occupancy::kPartial:
      return "partial";
    case Occupancy::kNearFull:
      return "near-full";
    case Occupancy::kSaturated:
      return "saturated";
  }
  return "unknown";
}

std::vector<WorkloadCase> MakeWorkloadMatrix(Mode mode) {
  const std::size_t repetitions = mode == Mode::kSmoke ? 1U : kControlledRepetitions;
  std::vector<WorkloadCase> workloads;
  workloads.reserve(repetitions * kProducerCounts.size() * kRecordSizes.size() *
                    kOccupancies.size());

  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    for (const std::size_t producer_count : kProducerCounts) {
      const std::size_t measured_rounds =
          mode == Mode::kSmoke
              ? kSmokeMeasuredRounds
              : std::max(kControlledMinimumMeasuredRounds,
                         (kControlledMinimumSamplesPerCell + producer_count - 1U) / producer_count);
      const std::size_t warmup_rounds =
          mode == Mode::kSmoke ? kSmokeWarmupRounds : kControlledWarmupRounds;
      for (const std::size_t record_size_bytes : kRecordSizes) {
        for (const Occupancy occupancy : kOccupancies) {
          workloads.push_back(WorkloadCase{
              .producer_count = producer_count,
              .record_size_bytes = record_size_bytes,
              .occupancy = occupancy,
              .capacity_bytes = kPayloadCapacityBytes,
              .warmup_rounds = warmup_rounds,
              .measured_rounds = measured_rounds,
              .repetition = repetition,
          });
        }
      }
    }
  }
  return workloads;
}

std::size_t InitialOccupancyBytes(const WorkloadCase& workload) {
  switch (workload.occupancy) {
    case Occupancy::kEmpty:
      return 0;
    case Occupancy::kPartial:
      return workload.capacity_bytes / 2U;
    case Occupancy::kNearFull:
      return workload.capacity_bytes / 64U * 63U;
    case Occupancy::kSaturated:
      return workload.capacity_bytes;
  }
  throw std::invalid_argument("Unknown workload occupancy value.");
}

std::size_t ExpectedAcceptedPerRound(const WorkloadCase& workload) {
  return ExpectedAcceptedPerRound(workload, workload.record_size_bytes);
}

std::size_t ExpectedAcceptedPerRound(const WorkloadCase& workload,
                                     std::size_t accounting_charge_bytes) {
  ValidateWorkloadCase(workload);
  if (accounting_charge_bytes == 0U) {
    throw std::invalid_argument(
        "Workload accounting_charge_bytes must be positive. Report the candidate's minimum "
        "physical Record charge and retry.");
  }
  const std::size_t initial_occupancy = InitialOccupancyBytes(workload);
  const std::size_t free_bytes = workload.capacity_bytes - initial_occupancy;
  return std::min(workload.producer_count, free_bytes / accounting_charge_bytes);
}

RecordFootprint MakePayloadOnlyRecordFootprint(std::size_t payload_bytes) noexcept {
  const auto bytes = static_cast<std::uint64_t>(payload_bytes);
  return RecordFootprint{
      .requested_message_bytes = bytes,
      .stored_message_bytes = bytes,
      .owned_payload_bytes = bytes,
      .metadata_bytes = 0,
      .fragmentation_bytes = 0,
      .accounting_charge_bytes = bytes,
      .minimum_accounting_charge_bytes = 1,
      .truncated = false,
  };
}

LatencySummary ComputeLatencySummary(std::span<const std::uint64_t> latency_nanoseconds) {
  if (latency_nanoseconds.empty()) {
    return LatencySummary{};
  }
  std::vector<std::uint64_t> sorted_samples(latency_nanoseconds.begin(), latency_nanoseconds.end());
  std::sort(sorted_samples.begin(), sorted_samples.end());
  return LatencySummary{
      .sample_count = static_cast<std::uint64_t>(sorted_samples.size()),
      .p50_nanoseconds = NearestRank(sorted_samples, 500),
      .p99_nanoseconds = NearestRank(sorted_samples, 990),
      .p999_nanoseconds = NearestRank(sorted_samples, 999),
  };
}

void ValidateWorkloadCase(const WorkloadCase& workload) {
  if (!Contains(kProducerCounts, workload.producer_count)) {
    throw std::invalid_argument("Workload producer_count must be one of 1, 2, 4, 8, 16, or 32.");
  }
  if (!Contains(kRecordSizes, workload.record_size_bytes)) {
    throw std::invalid_argument(
        "Workload record_size_bytes must be one of 64, 256, 1024, 4096, or 16384.");
  }
  if (!Contains(kOccupancies, workload.occupancy)) {
    throw std::invalid_argument(
        "Workload occupancy must be empty, partial, near-full, or saturated.");
  }
  if (workload.capacity_bytes != kPayloadCapacityBytes) {
    throw std::invalid_argument(
        "Workload capacity_bytes must be 1048576 for comparable prototype results.");
  }
  if (workload.warmup_rounds == 0U || workload.measured_rounds == 0U) {
    throw std::invalid_argument(
        "Workload warmup_rounds and measured_rounds must both be positive.");
  }
  const std::size_t initial_occupancy = InitialOccupancyBytes(workload);
  if (initial_occupancy > workload.capacity_bytes ||
      initial_occupancy % workload.record_size_bytes != 0U) {
    throw std::invalid_argument(
        "Workload occupancy must fit the capacity and align to the selected Record size.");
  }
}

void ValidateRecordFootprint(const WorkloadCase& workload, const RecordFootprint& footprint) {
  ValidateWorkloadCase(workload);
  const auto invalid = [](const char* reason) {
    throw std::invalid_argument(
        std::string{"Kernel Record footprint is invalid: "} + reason +
        " Fix DescribeRecord so payload, metadata, fragmentation, and accounting charge are "
        "exact and retry.");
  };

  if (footprint.requested_message_bytes != workload.record_size_bytes) {
    invalid("requested_message_bytes does not match the workload Record size.");
  }
  if (footprint.stored_message_bytes > footprint.requested_message_bytes) {
    invalid("stored_message_bytes exceeds requested_message_bytes.");
  }
  if (footprint.owned_payload_bytes < footprint.stored_message_bytes) {
    invalid("owned_payload_bytes does not cover the stored message.");
  }
  if (footprint.truncated != (footprint.stored_message_bytes < footprint.requested_message_bytes)) {
    invalid("the truncated flag disagrees with the stored message size.");
  }
  if (footprint.owned_payload_bytes >
      std::numeric_limits<std::uint64_t>::max() - footprint.metadata_bytes) {
    invalid("payload plus metadata overflows uint64_t.");
  }
  const std::uint64_t serialized_bytes = footprint.SerializedBytes();
  if (serialized_bytes >
      std::numeric_limits<std::uint64_t>::max() - footprint.fragmentation_bytes) {
    invalid("serialized bytes plus fragmentation overflows uint64_t.");
  }
  if (serialized_bytes + footprint.fragmentation_bytes != footprint.accounting_charge_bytes) {
    invalid("payload + metadata + fragmentation does not equal accounting_charge_bytes.");
  }
  if (footprint.minimum_accounting_charge_bytes == 0U ||
      footprint.minimum_accounting_charge_bytes > footprint.accounting_charge_bytes) {
    invalid("minimum_accounting_charge_bytes is zero or exceeds this Record's charge.");
  }
  if (footprint.accounting_charge_bytes > std::numeric_limits<std::size_t>::max()) {
    invalid("accounting_charge_bytes cannot be represented by size_t on this platform.");
  }
  if (footprint.accounting_charge_bytes > workload.capacity_bytes) {
    invalid("accounting_charge_bytes exceeds the common workload capacity.");
  }
}

}  // namespace ulog::benchmark_support
