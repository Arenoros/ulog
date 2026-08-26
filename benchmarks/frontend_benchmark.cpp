#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ulog/log.hpp>
#include <vector>

#include "producer/producer_kernel.hpp"
#include "support/allocation_interposer.hpp"
#include "support/benchmark_driver.hpp"
#include "support/process_cpu_clock.hpp"
#include "support/workload_harness.hpp"

namespace {

std::uint64_t default_logger_load_count = 0;

}  // namespace

namespace ulog {

Logger CountedGetDefaultLogger() noexcept {
  ++default_logger_load_count;
  return GetDefaultLogger();
}

}  // namespace ulog

// Macro rescan redirects only this translation unit's expanded default-target calls.
#define GetDefaultLogger() CountedGetDefaultLogger()

namespace {

namespace allocation_tracking = ulog::benchmark_support::allocation_tracking;
namespace producer = ulog::detail::producer;

using ulog::benchmark_support::LatencySummary;
using ulog::benchmark_support::Mode;

constexpr std::string_view kCandidate = "producer-frontend";
constexpr std::string_view kMessage = "frontend";
constexpr std::size_t kMaximumRecordBytes = producer::kMaximumRecordBytes;
constexpr std::size_t kTwoRecordCapacityBytes = 2 * kMaximumRecordBytes;
constexpr std::uint64_t kSmokeWarmupAttempts = 1;
constexpr std::uint64_t kSmokeMeasuredAttempts = 1'024;
constexpr std::uint64_t kControlledWarmupAttempts = 1'024;
constexpr std::uint64_t kControlledMeasuredAttempts = 100'000;
constexpr std::size_t kSmokeRepetitions = 1;
constexpr std::size_t kControlledRepetitions = 7;

enum class FrontendPath : std::uint8_t {
  kCompileErased,
  kRuntimeFiltered,
  kNullLogger,
  kAdmissionRejected,
  kOrdinaryAccepted,
};

constexpr FrontendPath kPaths[]{
    FrontendPath::kCompileErased,     FrontendPath::kRuntimeFiltered,  FrontendPath::kNullLogger,
    FrontendPath::kAdmissionRejected, FrontendPath::kOrdinaryAccepted,
};

std::atomic<bool> deterministic_failure{false};

[[nodiscard]] constexpr std::string_view ToString(FrontendPath path) noexcept {
  switch (path) {
    case FrontendPath::kCompileErased:
      return "compile-erased";
    case FrontendPath::kRuntimeFiltered:
      return "runtime-filtered";
    case FrontendPath::kNullLogger:
      return "null-logger";
    case FrontendPath::kAdmissionRejected:
      return "admission-rejected";
    case FrontendPath::kOrdinaryAccepted:
      return "ordinary-accepted";
  }
  return "invalid";
}

class KernelFixture final {
 public:
  KernelFixture(std::size_t payload_capacity_bytes, std::size_t ingress_cells)
      : kernel(producer::KernelConfig{
            .threshold = ulog::Level::kTrace,
            .payload_capacity_bytes = payload_capacity_bytes,
            .maximum_record_bytes = kMaximumRecordBytes,
            .producer_slots = 1,
            .ingress_cells = ingress_cells,
        }),
        registration(kernel.TryRegisterProducer()) {
    if (!registration) {
      throw std::runtime_error(
          "frontend benchmark could not register its application-lifetime producer; "
          "reduce fixture producer usage and retry");
    }
  }

  void ReconcileAndRegister() {
    registration = producer::ProducerKernel::ProducerRegistration{};
    registration = kernel.TryRegisterProducer();
    if (!registration) {
      throw std::runtime_error(
          "frontend benchmark could not restore its application-lifetime producer after "
          "draining; ensure the fixture is empty and retry");
    }
  }

  producer::ProducerKernel kernel;
  producer::ProducerKernel::ProducerRegistration registration;
};

KernelFixture& ActiveFixture() {
  static KernelFixture fixture{kTwoRecordCapacityBytes, 2};
  return fixture;
}

KernelFixture& RejectedFixture() {
  static KernelFixture fixture{kTwoRecordCapacityBytes, 1};
  return fixture;
}

class ScopedDefaultLogger final {
 public:
  explicit ScopedDefaultLogger(ulog::Logger logger) noexcept
      : previous_(ulog::ExchangeDefaultLogger(logger)) {}

  ScopedDefaultLogger(const ScopedDefaultLogger&) = delete;
  ScopedDefaultLogger& operator=(const ScopedDefaultLogger&) = delete;

  ~ScopedDefaultLogger() { static_cast<void>(ulog::ExchangeDefaultLogger(previous_)); }

 private:
  ulog::Logger previous_;
};

struct Observation final {
  std::uint64_t records{0};
  std::uint64_t invalid_records{0};
};

void ObserveRecord(void* context, std::uint64_t, const producer::RecordView& record) noexcept {
  auto& observation = *static_cast<Observation*>(context);
  ++observation.records;
  if (record.message() != kMessage) {
    ++observation.invalid_records;
  }
}

void DiscardRecord(void*, std::uint64_t, const producer::RecordView&) noexcept {}

std::string_view CountMessage(std::uint64_t& evaluations) noexcept {
  ++evaluations;
  return kMessage;
}

void Attempt(FrontendPath path, std::uint64_t& evaluations) {
  switch (path) {
    case FrontendPath::kCompileErased:
      LOG_INFO("{}", CountMessage(evaluations));
      return;
    case FrontendPath::kRuntimeFiltered:
      LOG(ulog::Level::kInfo, "{}", CountMessage(evaluations));
      return;
    case FrontendPath::kNullLogger:
    case FrontendPath::kAdmissionRejected:
    case FrontendPath::kOrdinaryAccepted:
      LOG_CRITICAL("{}", CountMessage(evaluations));
      return;
  }
}

struct FrontendResult final {
  FrontendPath path;
  std::size_t repetition;
  std::uint64_t warmup_attempts;
  std::uint64_t measured_attempts;
  LatencySummary latency;
  LatencySummary accepted_latency;
  LatencySummary rejected_latency;
  std::chrono::nanoseconds wall_time;
  std::chrono::nanoseconds process_cpu_time;
  double cpu_utilization_percent;
  double attempts_per_second;
  double records_per_second;
  double bytes_per_second;
  std::uint64_t accepted_records;
  std::uint64_t rejected_records;
  std::uint64_t compile_erased_records;
  std::uint64_t runtime_filtered_records;
  std::uint64_t null_logger_records;
  std::uint64_t admission_rejected_records;
  std::uint64_t message_evaluation_count;
  std::uint64_t default_logger_load_count;
  std::uint64_t accepted_bytes;
  std::uint64_t allocation_count;
  std::uint64_t allocation_failure_count;
  std::uint64_t logical_retained_final_bytes;
  std::uint64_t physical_retained_final_bytes;
  std::uint64_t accounting_error_count;
  std::uint64_t retained_bound_error_count;
};

struct KernelOutcomeDelta final {
  std::uint64_t attempted_records{0};
  std::uint64_t accepted_records{0};
  std::uint64_t consumed_records{0};
  std::uint64_t rejected_no_producer{0};
  std::uint64_t rejected_lane_full{0};
  std::uint64_t rejected_budget{0};
  std::uint64_t abandoned_builds{0};
  std::uint64_t invalid_records{0};
  std::uint64_t truncated_records{0};

  friend constexpr bool operator==(const KernelOutcomeDelta&,
                                   const KernelOutcomeDelta&) noexcept = default;
};

[[nodiscard]] std::uint64_t CheckedCounterDelta(std::uint64_t after, std::uint64_t before,
                                                std::uint64_t& errors) noexcept {
  if (after < before) {
    ++errors;
    return 0;
  }
  return after - before;
}

[[nodiscard]] KernelOutcomeDelta ComputeOutcomeDelta(const producer::KernelSnapshot& after,
                                                     const producer::KernelSnapshot& before,
                                                     std::uint64_t& errors) noexcept {
  return {
      .attempted_records =
          CheckedCounterDelta(after.attempted_records, before.attempted_records, errors),
      .accepted_records =
          CheckedCounterDelta(after.accepted_records, before.accepted_records, errors),
      .consumed_records =
          CheckedCounterDelta(after.consumed_records, before.consumed_records, errors),
      .rejected_no_producer =
          CheckedCounterDelta(after.rejected_no_producer, before.rejected_no_producer, errors),
      .rejected_lane_full =
          CheckedCounterDelta(after.rejected_lane_full, before.rejected_lane_full, errors),
      .rejected_budget = CheckedCounterDelta(after.rejected_budget, before.rejected_budget, errors),
      .abandoned_builds =
          CheckedCounterDelta(after.abandoned_builds, before.abandoned_builds, errors),
      .invalid_records = CheckedCounterDelta(after.invalid_records, before.invalid_records, errors),
      .truncated_records =
          CheckedCounterDelta(after.truncated_records, before.truncated_records, errors),
  };
}

[[nodiscard]] KernelOutcomeDelta ExpectedOutcomeDelta(FrontendPath path,
                                                      std::uint64_t attempts) noexcept {
  KernelOutcomeDelta expected;
  if (path == FrontendPath::kOrdinaryAccepted) {
    expected.attempted_records = attempts;
    expected.accepted_records = attempts;
    expected.consumed_records = attempts;
  } else if (path == FrontendPath::kAdmissionRejected) {
    expected.attempted_records = attempts;
    expected.rejected_lane_full = attempts;
  }
  return expected;
}

void PrintOutcomeMismatch(FrontendPath path, const KernelOutcomeDelta& actual,
                          const KernelOutcomeDelta& expected) {
  std::cerr << "frontend outcome mismatch: path=" << ToString(path)
            << "; actual{attempted=" << actual.attempted_records
            << ",accepted=" << actual.accepted_records << ",consumed=" << actual.consumed_records
            << ",no_producer=" << actual.rejected_no_producer
            << ",lane=" << actual.rejected_lane_full << ",budget=" << actual.rejected_budget
            << ",abandoned=" << actual.abandoned_builds << ",invalid=" << actual.invalid_records
            << ",truncated=" << actual.truncated_records
            << "}; expected{attempted=" << expected.attempted_records
            << ",accepted=" << expected.accepted_records
            << ",consumed=" << expected.consumed_records
            << ",no_producer=" << expected.rejected_no_producer
            << ",lane=" << expected.rejected_lane_full << ",budget=" << expected.rejected_budget
            << ",abandoned=" << expected.abandoned_builds << ",invalid=" << expected.invalid_records
            << ",truncated=" << expected.truncated_records << "}\n";
}

[[nodiscard]] FrontendResult RunPath(FrontendPath path, std::size_t repetition, Mode mode) {
  auto& active = ActiveFixture();
  auto& rejected = RejectedFixture();
  producer::ProducerKernel& observed_kernel =
      path == FrontendPath::kAdmissionRejected ? rejected.kernel : active.kernel;
  active.kernel.SetLevel(path == FrontendPath::kRuntimeFiltered ? ulog::Level::kWarning
                                                                : ulog::Level::kTrace);
  const ulog::Logger target =
      path == FrontendPath::kNullLogger ? ulog::GetNullLogger() : observed_kernel.GetLogger();
  const ScopedDefaultLogger default_logger{target};
  const std::uint64_t warmup_attempts =
      mode == Mode::kSmoke ? kSmokeWarmupAttempts : kControlledWarmupAttempts;
  const std::uint64_t measured_attempts =
      mode == Mode::kSmoke ? kSmokeMeasuredAttempts : kControlledMeasuredAttempts;

  if (path == FrontendPath::kAdmissionRejected) {
    LOG_CRITICAL(kMessage);
  }
  std::uint64_t warmup_evaluations = 0;
  Observation warmup_observation;
  for (std::uint64_t attempt = 0; attempt < warmup_attempts; ++attempt) {
    Attempt(path, warmup_evaluations);
    if (path == FrontendPath::kOrdinaryAccepted) {
      if (active.kernel.TryConsume(&warmup_observation, &ObserveRecord) !=
          producer::ConsumeStatus::kRecord) {
        ++warmup_observation.invalid_records;
      }
    }
  }

  std::vector<std::uint64_t> latency_nanoseconds(measured_attempts);
  std::uint64_t message_evaluations = 0;
  Observation observation;
  const auto snapshot_before = observed_kernel.GetSnapshot();
  const std::uint64_t default_loads_before = default_logger_load_count;
  const std::uint64_t allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const std::uint64_t allocation_failures_before =
      allocation_tracking::allocation_failure_count.load(std::memory_order_relaxed);
  const auto cpu_started = ulog::benchmark_support::ReadProcessCpuTime();
  const auto wall_started = std::chrono::steady_clock::now();
  for (std::uint64_t attempt = 0; attempt < measured_attempts; ++attempt) {
    const auto producer_started = std::chrono::steady_clock::now();
    Attempt(path, message_evaluations);
    const auto producer_finished = std::chrono::steady_clock::now();
    const auto latency =
        std::chrono::duration_cast<std::chrono::nanoseconds>(producer_finished - producer_started);
    latency_nanoseconds[attempt] =
        static_cast<std::uint64_t>(std::max<std::chrono::nanoseconds::rep>(latency.count(), 0));
    if (path == FrontendPath::kOrdinaryAccepted &&
        active.kernel.TryConsume(&observation, &ObserveRecord) !=
            producer::ConsumeStatus::kRecord) {
      ++observation.invalid_records;
    }
  }
  const auto wall_finished = std::chrono::steady_clock::now();
  const auto cpu_finished = ulog::benchmark_support::ReadProcessCpuTime();
  const std::uint64_t allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const std::uint64_t allocation_failures_after =
      allocation_tracking::allocation_failure_count.load(std::memory_order_relaxed);
  const auto snapshot_after = observed_kernel.GetSnapshot();
  if (path == FrontendPath::kAdmissionRejected &&
      rejected.kernel.TryConsume(nullptr, &DiscardRecord) != producer::ConsumeStatus::kRecord) {
    ++observation.invalid_records;
  }
  KernelFixture& observed_fixture = path == FrontendPath::kAdmissionRejected ? rejected : active;
  observed_fixture.ReconcileAndRegister();
  const auto final_snapshot = observed_kernel.GetSnapshot();

  std::uint64_t accounting_errors =
      warmup_observation.invalid_records + observation.invalid_records;
  const KernelOutcomeDelta outcome_delta =
      ComputeOutcomeDelta(snapshot_after, snapshot_before, accounting_errors);
  const KernelOutcomeDelta expected_outcome = ExpectedOutcomeDelta(path, measured_attempts);
  const std::uint64_t default_loads =
      CheckedCounterDelta(default_logger_load_count, default_loads_before, accounting_errors);
  const std::uint64_t allocations =
      CheckedCounterDelta(allocations_after, allocations_before, accounting_errors);
  const std::uint64_t allocation_failures =
      CheckedCounterDelta(allocation_failures_after, allocation_failures_before, accounting_errors);
  const std::uint64_t expected_accepted =
      path == FrontendPath::kOrdinaryAccepted ? measured_attempts : 0;
  const std::uint64_t expected_evaluations = expected_accepted;
  const std::uint64_t expected_default_loads =
      path == FrontendPath::kCompileErased ? 0 : measured_attempts;
  const std::uint64_t expected_warmup_evaluations =
      path == FrontendPath::kOrdinaryAccepted ? warmup_attempts : 0;
  if (outcome_delta != expected_outcome) {
    PrintOutcomeMismatch(path, outcome_delta, expected_outcome);
  }
  accounting_errors += outcome_delta != expected_outcome ? 1U : 0U;
  accounting_errors += message_evaluations != expected_evaluations ? 1U : 0U;
  accounting_errors += warmup_evaluations != expected_warmup_evaluations ? 1U : 0U;
  accounting_errors += default_loads != expected_default_loads ? 1U : 0U;
  accounting_errors += observation.records != expected_accepted ? 1U : 0U;
  accounting_errors += warmup_observation.records != expected_warmup_evaluations ? 1U : 0U;

  const auto wall_time =
      std::chrono::duration_cast<std::chrono::nanoseconds>(wall_finished - wall_started);
  const auto process_cpu_time = cpu_finished - cpu_started;
  if (wall_time.count() <= 0 || process_cpu_time.count() < 0) {
    ++accounting_errors;
  }
  const auto safe_wall_nanoseconds =
      static_cast<std::uint64_t>(std::max<std::chrono::nanoseconds::rep>(wall_time.count(), 1));
  const double rate_scale = 1'000'000'000.0 / static_cast<double>(safe_wall_nanoseconds);
  const LatencySummary latency =
      ulog::benchmark_support::ComputeLatencySummary(latency_nanoseconds);
  const LatencySummary accepted_latency =
      path == FrontendPath::kOrdinaryAccepted ? latency : LatencySummary{};
  const LatencySummary rejected_latency =
      path == FrontendPath::kOrdinaryAccepted ? LatencySummary{} : latency;
  const std::uint64_t retained_bound_errors =
      final_snapshot.logical_retained_bytes != 0U || final_snapshot.physical_retained_bytes != 0U ||
              final_snapshot.retained_records != 0U || !final_snapshot.accounting_sample_consistent
          ? 1U
          : 0U;
  const std::uint64_t rejected_records = measured_attempts - expected_accepted;
  return {
      .path = path,
      .repetition = repetition,
      .warmup_attempts = warmup_attempts,
      .measured_attempts = measured_attempts,
      .latency = latency,
      .accepted_latency = accepted_latency,
      .rejected_latency = rejected_latency,
      .wall_time = std::chrono::nanoseconds{safe_wall_nanoseconds},
      .process_cpu_time = process_cpu_time,
      .cpu_utilization_percent = static_cast<double>(process_cpu_time.count()) * 100.0 /
                                 static_cast<double>(safe_wall_nanoseconds),
      .attempts_per_second = static_cast<double>(measured_attempts) * rate_scale,
      .records_per_second = static_cast<double>(expected_accepted) * rate_scale,
      .bytes_per_second = static_cast<double>(expected_accepted * kMessage.size()) * rate_scale,
      .accepted_records = outcome_delta.accepted_records,
      .rejected_records = rejected_records,
      .compile_erased_records = path == FrontendPath::kCompileErased ? measured_attempts : 0,
      .runtime_filtered_records = path == FrontendPath::kRuntimeFiltered ? measured_attempts : 0,
      .null_logger_records = path == FrontendPath::kNullLogger ? measured_attempts : 0,
      .admission_rejected_records =
          path == FrontendPath::kAdmissionRejected ? measured_attempts : 0,
      .message_evaluation_count = message_evaluations,
      .default_logger_load_count = default_loads,
      .accepted_bytes = outcome_delta.accepted_records * kMessage.size(),
      .allocation_count = allocations,
      .allocation_failure_count = allocation_failures,
      .logical_retained_final_bytes = final_snapshot.logical_retained_bytes,
      .physical_retained_final_bytes = final_snapshot.physical_retained_bytes,
      .accounting_error_count = accounting_errors,
      .retained_bound_error_count = retained_bound_errors,
  };
}

void PublishLatency(benchmark::State& state, std::string_view prefix,
                    const LatencySummary& latency) {
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  const std::string sample_count = std::string{prefix} + "_latency_sample_count";
  const std::string p50 = std::string{prefix} + "_latency_p50_ns";
  const std::string p99 = std::string{prefix} + "_latency_p99_ns";
  const std::string p999 = std::string{prefix} + "_latency_p999_ns";
  SetCounter(state, sample_count.c_str(), latency.sample_count);
  SetCounter(state, p50.c_str(), latency.p50_nanoseconds);
  SetCounter(state, p99.c_str(), latency.p99_nanoseconds);
  SetCounter(state, p999.c_str(), latency.p999_nanoseconds);
}

void PublishResult(benchmark::State& state, const FrontendResult& result) {
  using ulog::benchmark_support::benchmark_driver::SetCounter;
  SetCounter(state, "workload_repetition_index", result.repetition);
  SetCounter(state, "warmup_attempts", result.warmup_attempts);
  SetCounter(state, "measured_attempts", result.measured_attempts);
  SetCounter(state, "sample_count", result.latency.sample_count);
  SetCounter(state, "producer_latency_p50_ns", result.latency.p50_nanoseconds);
  SetCounter(state, "producer_latency_p99_ns", result.latency.p99_nanoseconds);
  SetCounter(state, "producer_latency_p999_ns", result.latency.p999_nanoseconds);
  PublishLatency(state, "accepted", result.accepted_latency);
  PublishLatency(state, "rejected", result.rejected_latency);
  SetCounter(state, "attempted_records", result.measured_attempts);
  SetCounter(state, "accepted_records", result.accepted_records);
  SetCounter(state, "rejected_records", result.rejected_records);
  SetCounter(state, "compile_erased_records", result.compile_erased_records);
  SetCounter(state, "runtime_filtered_records", result.runtime_filtered_records);
  SetCounter(state, "null_logger_records", result.null_logger_records);
  SetCounter(state, "admission_rejected_records", result.admission_rejected_records);
  SetCounter(state, "message_evaluation_count", result.message_evaluation_count);
  SetCounter(state, "default_logger_load_count", result.default_logger_load_count);
  SetCounter(state, "accepted_bytes", result.accepted_bytes);
  SetCounter(state, "allocation_count", result.allocation_count);
  SetCounter(state, "allocation_failure_count", result.allocation_failure_count);
  SetCounter(state, "logical_retained_final_bytes", result.logical_retained_final_bytes);
  SetCounter(state, "physical_retained_final_bytes", result.physical_retained_final_bytes);
  SetCounter(state, "accounting_error_count", result.accounting_error_count);
  SetCounter(state, "retained_bound_error_count", result.retained_bound_error_count);
  SetCounter(state, "wall_time_ns", static_cast<std::uint64_t>(result.wall_time.count()));
  SetCounter(state, "process_cpu_time_ns",
             static_cast<std::uint64_t>(result.process_cpu_time.count()));
  state.counters["cpu_utilization_percent"] = result.cpu_utilization_percent;
  state.counters["attempts_per_second"] = result.attempts_per_second;
  state.counters["records_per_second"] = result.records_per_second;
  state.counters["bytes_per_second"] = result.bytes_per_second;
}

[[nodiscard]] bool HasDeterministicFailure(const FrontendResult& result) noexcept {
  return result.allocation_count != 0U || result.allocation_failure_count != 0U ||
         result.accounting_error_count != 0U || result.retained_bound_error_count != 0U;
}

void RunBenchmark(benchmark::State& state, FrontendPath path, std::size_t repetition, Mode mode) {
  FrontendResult result{};
  try {
    for ([[maybe_unused]] const auto iteration : state) {
      result = RunPath(path, repetition, mode);
      state.SetIterationTime(std::chrono::duration<double>{result.wall_time}.count());
    }
  } catch (const std::exception& error) {
    deterministic_failure.store(true, std::memory_order_relaxed);
    state.SkipWithError(error.what());
    return;
  }
  PublishResult(state, result);
  if (HasDeterministicFailure(result)) {
    deterministic_failure.store(true, std::memory_order_relaxed);
    std::cerr << "frontend deterministic failure: path=" << ToString(path)
              << ", allocations=" << result.allocation_count
              << ", allocation_failures=" << result.allocation_failure_count
              << ", evaluations=" << result.message_evaluation_count
              << ", default_loads=" << result.default_logger_load_count
              << ", accepted=" << result.accepted_records
              << ", rejected=" << result.rejected_records
              << ", logical_retained=" << result.logical_retained_final_bytes
              << ", physical_retained=" << result.physical_retained_final_bytes
              << ", accounting_errors=" << result.accounting_error_count
              << ", retained_errors=" << result.retained_bound_error_count << '\n';
    state.SkipWithError(
        "Deterministic frontend checks failed; inspect the preceding stderr diagnostic for "
        "allocation, evaluation, default-load, admission, and retained counters.");
  }
}

void RegisterWorkloads(Mode mode) {
  const std::size_t repetitions = mode == Mode::kSmoke ? kSmokeRepetitions : kControlledRepetitions;
  for (std::size_t repetition = 0; repetition < repetitions; ++repetition) {
    for (const FrontendPath path : kPaths) {
      const std::string name = "UlogFrontend/" + std::string{kCandidate} +
                               "/path:" + std::string{ToString(path)} +
                               "/repetition:" + std::to_string(repetition);
      benchmark::RegisterBenchmark(name.c_str(),
                                   [path, repetition, mode](benchmark::State& state) {
                                     RunBenchmark(state, path, repetition, mode);
                                   })
          ->Iterations(1)
          ->UseManualTime();
    }
  }
  benchmark::AddCustomContext("ulog_result_protocol", "ulog-frontend-results/1");
  benchmark::AddCustomContext("ulog_candidates", std::string{kCandidate});
  benchmark::AddCustomContext("ulog_candidate_schedule", "repetition-major");
  benchmark::AddCustomContext(
      "ulog_frontend_paths",
      "compile-erased,runtime-filtered,null-logger,admission-rejected,ordinary-accepted");
  benchmark::AddCustomContext("ulog_mode", std::string{ulog::benchmark_support::ToString(mode)});
  benchmark::AddCustomContext("ulog_timing_policy", "advisory");
  benchmark::AddCustomContext("ulog_repetitions", std::to_string(repetitions));
}

}  // namespace

int main(int argument_count, char** arguments) {
  static_assert(ULOG_COMPILE_TIME_MIN_LEVEL == 5);
  return ulog::benchmark_support::benchmark_driver::RunMain(
      argument_count, arguments, deterministic_failure, RegisterWorkloads);
}
