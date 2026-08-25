#pragma once

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "support/process_cpu_clock.hpp"

namespace ulog::benchmark_support {

inline constexpr std::size_t kPayloadCapacityBytes = 1'048'576;

enum class Mode { kSmoke, kControlled };

enum class Occupancy { kEmpty, kPartial, kNearFull, kSaturated };

enum class AttemptStatus { kAccepted, kRejected, kAllocationFailure };

struct WorkloadCase final {
  std::size_t producer_count;
  std::size_t record_size_bytes;
  Occupancy occupancy;
  std::size_t capacity_bytes;
  std::size_t warmup_rounds;
  std::size_t measured_rounds;
  std::size_t repetition;
};

struct KernelSnapshot final {
  std::uint64_t attempted_records;
  std::uint64_t accepted_records;
  std::uint64_t rejected_records;
  std::uint64_t allocation_count;
  std::uint64_t allocation_failure_count;
  std::uint64_t logical_retained_initial_bytes;
  std::uint64_t logical_retained_high_water_bytes;
  std::uint64_t logical_retained_current_bytes;
  std::uint64_t logical_retained_limit_bytes;
  std::uint64_t physical_retained_initial_bytes;
  std::uint64_t physical_retained_high_water_bytes;
  std::uint64_t physical_retained_current_bytes;
  std::uint64_t physical_retained_limit_bytes;
};

struct RecordFootprint final {
  std::uint64_t requested_message_bytes;
  std::uint64_t stored_message_bytes;
  std::uint64_t owned_payload_bytes;
  std::uint64_t metadata_bytes;
  std::uint64_t fragmentation_bytes;
  std::uint64_t accounting_charge_bytes;
  std::uint64_t minimum_accounting_charge_bytes;
  bool truncated;

  [[nodiscard]] constexpr std::uint64_t SerializedBytes() const noexcept {
    return owned_payload_bytes + metadata_bytes;
  }
};

struct LatencySummary final {
  std::uint64_t sample_count;
  std::uint64_t p50_nanoseconds;
  std::uint64_t p99_nanoseconds;
  std::uint64_t p999_nanoseconds;
};

struct WorkloadResult final {
  WorkloadCase workload;
  LatencySummary latency;
  LatencySummary accepted_latency;
  LatencySummary rejected_latency;
  RecordFootprint record_footprint;
  std::chrono::nanoseconds wall_time;
  std::chrono::nanoseconds process_cpu_time;
  double cpu_utilization_percent;
  double attempts_per_second;
  double records_per_second;
  double bytes_per_second;
  std::uint64_t attempted_records;
  std::uint64_t accepted_records;
  std::uint64_t rejected_records;
  std::uint64_t accepted_bytes;
  std::uint64_t rejected_bytes;
  std::uint64_t allocation_count;
  std::uint64_t allocation_failure_count;
  std::uint64_t truncated_records;
  std::uint64_t logical_retained_initial_bytes;
  std::uint64_t logical_retained_high_water_bytes;
  std::uint64_t logical_retained_final_bytes;
  std::uint64_t logical_retained_limit_bytes;
  std::uint64_t physical_retained_initial_bytes;
  std::uint64_t physical_retained_high_water_bytes;
  std::uint64_t physical_retained_final_bytes;
  std::uint64_t physical_retained_limit_bytes;
  std::uint64_t accounting_error_count;
  std::uint64_t retained_bound_error_count;
};

[[nodiscard]] std::string_view ToString(Mode mode) noexcept;
[[nodiscard]] std::string_view ToString(Occupancy occupancy) noexcept;
[[nodiscard]] std::vector<WorkloadCase> MakeWorkloadMatrix(Mode mode);
[[nodiscard]] std::size_t InitialOccupancyBytes(const WorkloadCase& workload);
[[nodiscard]] std::size_t ExpectedAcceptedPerRound(const WorkloadCase& workload);
[[nodiscard]] std::size_t ExpectedAcceptedPerRound(const WorkloadCase& workload,
                                                   std::size_t accounting_charge_bytes);
[[nodiscard]] RecordFootprint MakePayloadOnlyRecordFootprint(std::size_t payload_bytes) noexcept;
[[nodiscard]] LatencySummary ComputeLatencySummary(
    std::span<const std::uint64_t> latency_nanoseconds);
void ValidateWorkloadCase(const WorkloadCase& workload);
void ValidateRecordFootprint(const WorkloadCase& workload, const RecordFootprint& footprint);

template <typename Kernel>
concept WorkloadKernel =
    requires(Kernel& kernel, const Kernel& const_kernel, const WorkloadCase& workload,
             std::span<const std::byte> payload, typename Kernel::Attempt& attempt,
             const typename Kernel::Attempt& const_attempt) {
      typename Kernel::Attempt;
      { Kernel::Name() } -> std::convertible_to<std::string_view>;
      { kernel.Prepare(workload) } -> std::same_as<void>;
      { kernel.BeginMeasurement() } noexcept -> std::same_as<void>;
      { kernel.ObserveRetainedHighWater() } noexcept -> std::same_as<void>;
      { kernel.EndMeasurement() } -> std::same_as<void>;
      { const_kernel.DescribeRecord(payload) } noexcept -> std::same_as<RecordFootprint>;
      {
        kernel.TryProduce(std::size_t{}, payload)
      } noexcept -> std::same_as<typename Kernel::Attempt>;
      { const_attempt.status() } noexcept -> std::same_as<AttemptStatus>;
      { kernel.Release(attempt) } noexcept -> std::same_as<void>;
      { const_kernel.Snapshot() } noexcept -> std::same_as<KernelSnapshot>;
    };

namespace impl {

struct LatencySample final {
  std::uint64_t nanoseconds{};
  AttemptStatus status{AttemptStatus::kRejected};
};

inline void CountErrorIf(bool has_error, std::uint64_t& error_count) noexcept {
  if (has_error) {
    ++error_count;
  }
}

struct StandardThreadLauncher final {
  template <typename Worker>
  void operator()(std::vector<std::thread>& threads, Worker&& worker) const {
    threads.emplace_back(std::forward<Worker>(worker));
  }
};

}  // namespace impl

template <WorkloadKernel Kernel, typename ThreadLauncher = impl::StandardThreadLauncher>
[[nodiscard]] WorkloadResult RunWorkload(const WorkloadCase& workload, Kernel& kernel,
                                         ThreadLauncher thread_launcher = {}) {
  ValidateWorkloadCase(workload);
  kernel.Prepare(workload);

  const std::size_t sample_count = workload.producer_count * workload.measured_rounds;
  std::vector<impl::LatencySample> samples(sample_count);
  std::vector<std::byte> payload(workload.record_size_bytes);
  for (std::size_t index = 0; index < payload.size(); ++index) {
    constexpr unsigned char kFirstPayloadCharacter = 'a';
    constexpr std::size_t kPayloadAlphabetSize = 26;
    payload[index] = std::byte{
        static_cast<unsigned char>(kFirstPayloadCharacter + index % kPayloadAlphabetSize)};
  }
  const std::span<const std::byte> payload_view{payload};
  const RecordFootprint record_footprint = std::as_const(kernel).DescribeRecord(payload_view);
  ValidateRecordFootprint(workload, record_footprint);
  const std::size_t expected_accepted_per_round = ExpectedAcceptedPerRound(
      workload, static_cast<std::size_t>(record_footprint.accounting_charge_bytes));

  const auto producer_count = static_cast<std::ptrdiff_t>(workload.producer_count);
  std::barrier initial_start{producer_count + 1};
  std::barrier warmup_done{producer_count + 1};
  std::barrier measurement_start{producer_count + 1};
  std::barrier measurement_done{producer_count + 1};
  std::barrier round_start{producer_count};
  std::size_t completed_round_count = 0;
  std::barrier round_attempted{producer_count, [&]() noexcept {
                                 if (completed_round_count == workload.warmup_rounds) {
                                   kernel.ObserveRetainedHighWater();
                                 }
                                 ++completed_round_count;
                               }};

  std::vector<std::thread> producer_threads;
  producer_threads.reserve(workload.producer_count);
  std::atomic<bool> launch_cancelled{false};
  try {
    for (std::size_t producer_index = 0; producer_index < workload.producer_count;
         ++producer_index) {
      thread_launcher(producer_threads, [&, producer_index] {
        initial_start.arrive_and_wait();
        if (launch_cancelled.load(std::memory_order_acquire)) {
          return;
        }
        for (std::size_t round = 0; round < workload.warmup_rounds; ++round) {
          round_start.arrive_and_wait();
          auto attempt = kernel.TryProduce(producer_index, payload_view);
          round_attempted.arrive_and_wait();
          kernel.Release(attempt);
        }

        warmup_done.arrive_and_wait();
        measurement_start.arrive_and_wait();
        for (std::size_t round = 0; round < workload.measured_rounds; ++round) {
          round_start.arrive_and_wait();
          const auto started_at = std::chrono::steady_clock::now();
          auto attempt = kernel.TryProduce(producer_index, payload_view);
          const auto finished_at = std::chrono::steady_clock::now();
          const auto elapsed =
              std::chrono::duration_cast<std::chrono::nanoseconds>(finished_at - started_at);
          const std::size_t sample_index = producer_index * workload.measured_rounds + round;
          samples[sample_index] = impl::LatencySample{
              .nanoseconds = static_cast<std::uint64_t>(std::max(elapsed.count(), std::int64_t{0})),
              .status = attempt.status(),
          };
          round_attempted.arrive_and_wait();
          kernel.Release(attempt);
        }
        measurement_done.arrive_and_wait();
      });
    }
  } catch (...) {
    launch_cancelled.store(true, std::memory_order_release);
    for (std::size_t missing_producer = producer_threads.size();
         missing_producer < workload.producer_count; ++missing_producer) {
      initial_start.arrive_and_drop();
    }
    initial_start.arrive_and_drop();
    for (auto& producer_thread : producer_threads) {
      producer_thread.join();
    }
    throw;
  }

  initial_start.arrive_and_wait();
  warmup_done.arrive_and_wait();
  kernel.BeginMeasurement();

  std::exception_ptr cpu_clock_error;
  std::chrono::nanoseconds cpu_started_at{};
  try {
    cpu_started_at = ReadProcessCpuTime();
  } catch (...) {
    cpu_clock_error = std::current_exception();
  }
  const auto wall_started_at = std::chrono::steady_clock::now();
  measurement_start.arrive_and_wait();
  measurement_done.arrive_and_wait();
  const auto wall_finished_at = std::chrono::steady_clock::now();

  std::chrono::nanoseconds cpu_finished_at{};
  try {
    cpu_finished_at = ReadProcessCpuTime();
  } catch (...) {
    if (!cpu_clock_error) {
      cpu_clock_error = std::current_exception();
    }
  }
  for (auto& producer_thread : producer_threads) {
    producer_thread.join();
  }
  const auto snapshot = std::as_const(kernel).Snapshot();
  kernel.EndMeasurement();
  if (cpu_clock_error) {
    std::rethrow_exception(cpu_clock_error);
  }

  const auto measured_wall_time = std::max(
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_finished_at - wall_started_at),
      std::chrono::nanoseconds{1});
  if (cpu_finished_at < cpu_started_at) {
    throw std::runtime_error(
        "Process CPU clock moved backwards during a workload; rerun on a supported monotonic "
        "process clock.");
  }
  const auto measured_cpu_time = cpu_finished_at - cpu_started_at;

  std::vector<std::uint64_t> latency_nanoseconds;
  std::vector<std::uint64_t> accepted_latency_nanoseconds;
  std::vector<std::uint64_t> rejected_latency_nanoseconds;
  latency_nanoseconds.reserve(samples.size());
  accepted_latency_nanoseconds.reserve(samples.size());
  rejected_latency_nanoseconds.reserve(samples.size());
  std::uint64_t accepted_records = 0;
  std::uint64_t rejected_records = 0;
  std::uint64_t observed_allocation_failures = 0;
  for (const auto& sample : samples) {
    latency_nanoseconds.push_back(sample.nanoseconds);
    if (sample.status == AttemptStatus::kAccepted) {
      ++accepted_records;
      accepted_latency_nanoseconds.push_back(sample.nanoseconds);
    } else {
      ++rejected_records;
      rejected_latency_nanoseconds.push_back(sample.nanoseconds);
      if (sample.status == AttemptStatus::kAllocationFailure) {
        ++observed_allocation_failures;
      }
    }
  }

  const auto attempted_records = static_cast<std::uint64_t>(samples.size());
  const auto record_size_bytes = static_cast<std::uint64_t>(workload.record_size_bytes);
  const auto accepted_bytes = accepted_records * record_size_bytes;
  const auto rejected_bytes = rejected_records * record_size_bytes;
  const auto expected_accepted_records =
      static_cast<std::uint64_t>(expected_accepted_per_round * workload.measured_rounds);

  std::uint64_t accounting_error_count = 0;
  impl::CountErrorIf(attempted_records != accepted_records + rejected_records,
                     accounting_error_count);
  impl::CountErrorIf(accepted_records != expected_accepted_records, accounting_error_count);
  impl::CountErrorIf(snapshot.attempted_records != attempted_records, accounting_error_count);
  impl::CountErrorIf(snapshot.accepted_records != accepted_records, accounting_error_count);
  impl::CountErrorIf(snapshot.rejected_records != rejected_records, accounting_error_count);
  impl::CountErrorIf(snapshot.allocation_failure_count != observed_allocation_failures,
                     accounting_error_count);

  const auto expected_initial = static_cast<std::uint64_t>(InitialOccupancyBytes(workload));
  const auto expected_logical_high_water =
      expected_initial +
      static_cast<std::uint64_t>(expected_accepted_per_round) * record_footprint.SerializedBytes();
  std::uint64_t retained_bound_error_count = 0;
  impl::CountErrorIf(snapshot.logical_retained_initial_bytes != expected_initial,
                     retained_bound_error_count);
  impl::CountErrorIf(snapshot.logical_retained_current_bytes != expected_initial,
                     retained_bound_error_count);
  impl::CountErrorIf(snapshot.logical_retained_high_water_bytes != expected_logical_high_water,
                     retained_bound_error_count);
  impl::CountErrorIf(snapshot.logical_retained_limit_bytes != workload.capacity_bytes,
                     retained_bound_error_count);
  impl::CountErrorIf(
      snapshot.logical_retained_high_water_bytes > snapshot.logical_retained_limit_bytes,
      retained_bound_error_count);
  impl::CountErrorIf(
      snapshot.physical_retained_current_bytes != snapshot.physical_retained_initial_bytes,
      retained_bound_error_count);
  impl::CountErrorIf(
      snapshot.physical_retained_high_water_bytes < snapshot.physical_retained_initial_bytes,
      retained_bound_error_count);
  impl::CountErrorIf(
      snapshot.physical_retained_high_water_bytes > snapshot.physical_retained_limit_bytes,
      retained_bound_error_count);

  const double wall_seconds = std::chrono::duration<double>{measured_wall_time}.count();
  const double attempted_as_double = static_cast<double>(attempted_records);
  const double accepted_as_double = static_cast<double>(accepted_records);
  const double cpu_seconds = std::chrono::duration<double>{measured_cpu_time}.count();

  return WorkloadResult{
      .workload = workload,
      .latency = ComputeLatencySummary(latency_nanoseconds),
      .accepted_latency = ComputeLatencySummary(accepted_latency_nanoseconds),
      .rejected_latency = ComputeLatencySummary(rejected_latency_nanoseconds),
      .record_footprint = record_footprint,
      .wall_time = measured_wall_time,
      .process_cpu_time = measured_cpu_time,
      .cpu_utilization_percent = cpu_seconds / wall_seconds * 100.0,
      .attempts_per_second = attempted_as_double / wall_seconds,
      .records_per_second = accepted_as_double / wall_seconds,
      .bytes_per_second =
          accepted_as_double * static_cast<double>(record_size_bytes) / wall_seconds,
      .attempted_records = attempted_records,
      .accepted_records = accepted_records,
      .rejected_records = rejected_records,
      .accepted_bytes = accepted_bytes,
      .rejected_bytes = rejected_bytes,
      .allocation_count = snapshot.allocation_count,
      .allocation_failure_count = snapshot.allocation_failure_count,
      .truncated_records = record_footprint.truncated ? accepted_records : 0U,
      .logical_retained_initial_bytes = snapshot.logical_retained_initial_bytes,
      .logical_retained_high_water_bytes = snapshot.logical_retained_high_water_bytes,
      .logical_retained_final_bytes = snapshot.logical_retained_current_bytes,
      .logical_retained_limit_bytes = snapshot.logical_retained_limit_bytes,
      .physical_retained_initial_bytes = snapshot.physical_retained_initial_bytes,
      .physical_retained_high_water_bytes = snapshot.physical_retained_high_water_bytes,
      .physical_retained_final_bytes = snapshot.physical_retained_current_bytes,
      .physical_retained_limit_bytes = snapshot.physical_retained_limit_bytes,
      .accounting_error_count = accounting_error_count,
      .retained_bound_error_count = retained_bound_error_count,
  };
}

}  // namespace ulog::benchmark_support
