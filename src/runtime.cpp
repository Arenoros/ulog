#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>
#include <utility>
#include <variant>

#include "control/control_reserve.hpp"
#include "control/thread_role.hpp"
#include "io/raw_file_sink.hpp"
#include "producer/producer_kernel.hpp"
#include "route/delivery_accounting.hpp"
#include "runtime_factory.hpp"
#include "testing/in_memory_destination_access.hpp"
#include "testing/in_memory_encoded_destination_access.hpp"

namespace ulog {
namespace {

using namespace std::chrono_literals;
using detail::control::ControlReserve;
using detail::control::OperationCompletion;
using detail::io::RawFileSink;
using detail::producer::ConsumeStatus;
using detail::producer::KernelConfig;
using detail::producer::KernelSnapshot;
using detail::producer::ProducerKernel;
using detail::route::DeliveryOutcome;
using detail::route::DeliveryRetirement;
using detail::route::InFlightSummary;
using detail::route::RouteWake;
using detail::route::StoreResult;
using detail::testing::DestinationWriteClaim;
using detail::testing::EncodedDestinationWriteClaim;
using detail::testing::InMemoryDestinationAccess;
using detail::testing::InMemoryEncodedDestinationAccess;

constexpr std::size_t kMinimumRecordBytes = 128;
constexpr std::size_t kMaximumControlOperations = 64;
constexpr auto kWorkerRecheckInterval = 10ms;
constexpr auto kMaximumLifecycleTimeout = std::chrono::hours{24};

constexpr std::size_t kMaximumWriteBuffers = 64;

[[nodiscard]] std::optional<RuntimeCreateFailure> ValidateRuntimeConfig(
    const RuntimeConfig& config) noexcept {
  if (static_cast<std::uint8_t>(config.threshold) > static_cast<std::uint8_t>(Level::kNone)) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidThreshold};
  }
  if (config.maximum_record_bytes < kMinimumRecordBytes ||
      config.maximum_record_bytes > detail::producer::kMaximumRecordBytes ||
      config.maximum_record_bytes % detail::producer::kAccountingQuantumBytes != 0U) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidMaximumRecordBytes};
  }
  if (config.payload_capacity_bytes < config.maximum_record_bytes ||
      config.payload_capacity_bytes % detail::producer::kAccountingQuantumBytes != 0U) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidPayloadCapacity};
  }
  if (config.producer_slots == 0U ||
      config.producer_slots > detail::producer::kMaximumProducerSlots) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidProducerSlots};
  }
  if (config.ingress_cells < config.producer_slots ||
      config.ingress_cells > detail::producer::kMaximumIngressCells) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidIngressCells};
  }
  if (config.control_operations == 0U || config.control_operations > kMaximumControlOperations) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidControlCapacity};
  }
  if (config.worker_threads != 1U) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidWorkerCount};
  }
  if (config.startup_timeout <= std::chrono::milliseconds::zero() ||
      config.startup_timeout > kMaximumLifecycleTimeout) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidStartupTimeout};
  }
  if (config.destruction_timeout <= std::chrono::milliseconds::zero() ||
      config.destruction_timeout > kMaximumLifecycleTimeout) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidDestructionTimeout};
  }
  return std::nullopt;
}

template <typename Destination>
[[nodiscard]] std::optional<RuntimeCreateFailure> ValidateConfig(
    const RuntimeConfig& config, const Destination& destination) noexcept {
  if (auto failure = ValidateRuntimeConfig(config)) {
    return failure;
  }
  if (destination.Capacity() == 0U ||
      destination.MaximumRecordBytes() < config.maximum_record_bytes) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidDestination};
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::string_view FailureMessage(RuntimeCreateErrorCode code) noexcept {
  switch (code) {
    case RuntimeCreateErrorCode::kInvalidThreshold:
      return "Runtime threshold is not a valid ulog::Level value.";
    case RuntimeCreateErrorCode::kInvalidMaximumRecordBytes:
      return "Runtime maximum_record_bytes is outside the supported aligned range.";
    case RuntimeCreateErrorCode::kInvalidPayloadCapacity:
      return "Runtime payload_capacity_bytes cannot reserve one maximum-sized Record.";
    case RuntimeCreateErrorCode::kInvalidProducerSlots:
      return "Runtime producer_slots is outside the supported bounded range.";
    case RuntimeCreateErrorCode::kInvalidIngressCells:
      return "Runtime ingress_cells cannot provide a bounded lane for every producer slot.";
    case RuntimeCreateErrorCode::kInvalidControlCapacity:
      return "Runtime control_operations is outside the supported bounded range.";
    case RuntimeCreateErrorCode::kInvalidWorkerCount:
      return "The current Runtime supports exactly one worker thread.";
    case RuntimeCreateErrorCode::kInvalidStartupTimeout:
      return "Runtime startup_timeout must be positive and no greater than 24 hours.";
    case RuntimeCreateErrorCode::kInvalidDestructionTimeout:
      return "Runtime destruction_timeout must be positive and no greater than 24 hours.";
    case RuntimeCreateErrorCode::kInvalidDestination:
      return "The in-memory destination is empty, too small, already attached, or stopped.";
    case RuntimeCreateErrorCode::kAllocationFailed:
      return "Runtime could not reserve its fixed bounded state.";
    case RuntimeCreateErrorCode::kWorkerStartFailed:
      return "Runtime could not start its configured worker thread.";
    case RuntimeCreateErrorCode::kWorkerStartupTimedOut:
      return "Runtime worker did not report readiness before startup_timeout.";
    case RuntimeCreateErrorCode::kInvalidFilePath:
      return "The file route path is empty, contains NUL, is not valid UTF-8, or names no file.";
    case RuntimeCreateErrorCode::kInvalidWriteBuffers:
      return "The file route write_buffers is outside the supported bounded range.";
    case RuntimeCreateErrorCode::kIoLoopStartFailed:
      return "Runtime could not start its private I/O loop thread before startup_timeout.";
    case RuntimeCreateErrorCode::kFileOpenFailed:
      return "Runtime could not create or append-open the file route path.";
  }
  return "Runtime creation failed.";
}

[[nodiscard]] constexpr std::string_view FailureFix(RuntimeCreateErrorCode code) noexcept {
  switch (code) {
    case RuntimeCreateErrorCode::kInvalidThreshold:
      return "Set threshold to Trace through None.";
    case RuntimeCreateErrorCode::kInvalidMaximumRecordBytes:
      return "Set maximum_record_bytes to a 64-byte multiple from 128 through 16384.";
    case RuntimeCreateErrorCode::kInvalidPayloadCapacity:
      return "Use a 64-byte-aligned payload_capacity_bytes at least maximum_record_bytes.";
    case RuntimeCreateErrorCode::kInvalidProducerSlots:
      return "Set producer_slots from 1 through 32.";
    case RuntimeCreateErrorCode::kInvalidIngressCells:
      return "Set ingress_cells from producer_slots through 64.";
    case RuntimeCreateErrorCode::kInvalidControlCapacity:
      return "Set control_operations from 1 through 64.";
    case RuntimeCreateErrorCode::kInvalidWorkerCount:
      return "Set worker_threads to 1; the current Runtime has one worker.";
    case RuntimeCreateErrorCode::kInvalidStartupTimeout:
      return "Set startup_timeout to a positive duration no greater than 24 hours.";
    case RuntimeCreateErrorCode::kInvalidDestructionTimeout:
      return "Set destruction_timeout to a positive duration no greater than 24 hours.";
    case RuntimeCreateErrorCode::kInvalidDestination:
      return "Use a fresh, non-empty destination whose maximum_record_bytes is at least the "
             "Runtime value; attach each destination state to exactly one Runtime.";
    case RuntimeCreateErrorCode::kAllocationFailed:
      return "Reduce configured pool capacities or make enough memory available, then retry.";
    case RuntimeCreateErrorCode::kWorkerStartFailed:
      return "Make an OS thread available or reduce process thread usage, then retry.";
    case RuntimeCreateErrorCode::kWorkerStartupTimedOut:
      return "Inspect host scheduling pressure; retry only after the worker-start stall is "
             "understood.";
    case RuntimeCreateErrorCode::kInvalidFilePath:
      return "Set RawFileRouteConfig::path to a UTF-8 file path such as logs/app.log, not an "
             "empty path or a directory.";
    case RuntimeCreateErrorCode::kInvalidWriteBuffers:
      return "Set RawFileRouteConfig::write_buffers from 1 through 64.";
    case RuntimeCreateErrorCode::kIoLoopStartFailed:
      return "Check IoErrorName(io_error); make an OS thread and event handle available or "
             "raise startup_timeout, then retry.";
    case RuntimeCreateErrorCode::kFileOpenFailed:
      return "Check IoErrorName(io_error): create the parent directory, grant write permission, "
             "or choose a path that is not a directory, then retry.";
  }
  return "Correct the Runtime configuration and retry.";
}

/// Returns the libuv path bytes: UTF-8 on Windows and native bytes elsewhere.
[[nodiscard]] std::optional<std::string> FilePathBytes(const std::filesystem::path& path) {
#if defined(_WIN32)
  const std::u8string utf8 = path.u8string();
  std::string bytes{reinterpret_cast<const char*>(utf8.data()), utf8.size()};
#else
  std::string bytes = path.native();
#endif
  if (bytes.empty() || bytes.find('\0') != std::string::npos || !path.has_filename()) {
    return std::nullopt;
  }
  return bytes;
}

[[nodiscard]] std::optional<RuntimeCreateFailure> ValidateFileRoute(
    const RawFileRouteConfig& route) noexcept {
  if (route.path.empty()) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidFilePath};
  }
  if (route.write_buffers == 0U || route.write_buffers > kMaximumWriteBuffers) {
    return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidWriteBuffers};
  }
  return std::nullopt;
}

[[nodiscard]] RuntimeCreateFailure MapFileSinkStart(
    detail::io::FileSinkStartResult started) noexcept {
  switch (started.status) {
    case detail::io::FileSinkStartStatus::kStarted:
      break;
    case detail::io::FileSinkStartStatus::kOpenFailed:
      return {.code = RuntimeCreateErrorCode::kFileOpenFailed, .io_error = started.io_error};
    case detail::io::FileSinkStartStatus::kThreadFailed:
    case detail::io::FileSinkStartStatus::kLoopFailed:
    case detail::io::FileSinkStartStatus::kTimedOut:
      return {.code = RuntimeCreateErrorCode::kIoLoopStartFailed, .io_error = started.io_error};
  }
  return {.code = RuntimeCreateErrorCode::kIoLoopStartFailed, .io_error = started.io_error};
}

enum class ControlActionKind : std::uint8_t { kDrain, kShutdown };

struct ControlAction final {
  ControlActionKind kind{ControlActionKind::kDrain};
  std::uint64_t watermark{0};
  OperationCompletion completion{};
  bool active{false};
};

struct RouteDeliveryResult final {
  ConsumeStatus consumed{ConsumeStatus::kEmpty};
  std::uint64_t admission_sequence{0};
  std::size_t encoded_bytes{0};
  bool submitted{false};
  bool retired{false};
};

/// Exact route accounting owned by the worker and read under the Runtime state lock.
struct RouteLedger final {
  std::uint64_t processed_records{0};
  std::uint64_t processed_bytes{0};
  std::uint64_t retired_records{0};
  std::uint64_t delivered_records{0};
  std::uint64_t delivered_bytes{0};
  std::uint64_t failed_records{0};
  std::uint64_t failed_bytes{0};
  std::uint64_t encoding_failed_records{0};
  std::optional<std::uint64_t> encoding_failed_sequence{};
  bool settled{false};
};

class RuntimeRoute final {
 public:
  explicit RuntimeRoute(testing::InMemoryDestination destination) noexcept
      : destination_(std::move(destination)) {}
  explicit RuntimeRoute(testing::InMemoryEncodedDestination destination) noexcept
      : destination_(std::move(destination)) {}
  explicit RuntimeRoute(std::unique_ptr<RawFileSink> sink) noexcept
      : destination_(std::move(sink)) {}

  RuntimeRoute(RuntimeRoute&& other) noexcept
      : destination_(std::move(other.destination_)),
        attached_(std::exchange(other.attached_, false)) {}
  RuntimeRoute& operator=(RuntimeRoute&&) = delete;
  RuntimeRoute(const RuntimeRoute&) = delete;
  RuntimeRoute& operator=(const RuntimeRoute&) = delete;

  ~RuntimeRoute() { Detach(); }

  /// Attaches an in-memory destination, or starts the file sink's loop and opens its file.
  [[nodiscard]] std::optional<RuntimeCreateFailure> Attach(
      RouteWake wake, std::chrono::steady_clock::time_point deadline) noexcept {
    if (auto* structured_destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      attached_ = InMemoryDestinationAccess::TryAttachRuntime(*structured_destination);
    } else if (auto* encoded_destination =
                   std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      attached_ = InMemoryEncodedDestinationAccess::TryAttachRuntime(*encoded_destination, wake);
    } else if (auto* sink = FileSink()) {
      const auto started = sink->Start(wake, deadline);
      attached_ = started.status == detail::io::FileSinkStartStatus::kStarted;
      if (!attached_) {
        return MapFileSinkStart(started);
      }
    }
    if (!attached_) {
      return RuntimeCreateFailure{RuntimeCreateErrorCode::kInvalidDestination};
    }
    return std::nullopt;
  }

  void Detach() noexcept {
    if (!attached_) {
      return;
    }
    if (auto* structured_destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      InMemoryDestinationAccess::DetachRuntime(*structured_destination);
    } else if (auto* encoded_destination =
                   std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      InMemoryEncodedDestinationAccess::DetachRuntime(*encoded_destination);
    } else if (auto* sink = FileSink()) {
      sink->DetachWake();
    }
    attached_ = false;
  }

  /// Consumes at most one Record. A structured Record retires immediately; an encoded Record
  /// becomes one serialized delivery that retires after its destination or file completion.
  [[nodiscard]] RouteDeliveryResult Deliver(
      ProducerKernel& producer, std::chrono::steady_clock::duration recheck_interval) noexcept {
    if (auto* destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      DestinationWriteClaim claim =
          InMemoryDestinationAccess::WaitForWrite(*destination, recheck_interval);
      if (!claim) {
        return {};
      }
      const ConsumeStatus consumed = producer.TryConsume(&claim, &StoreStructuredRecord);
      const bool stored = consumed == ConsumeStatus::kRecord;
      return {.consumed = consumed, .submitted = stored, .retired = stored};
    }
    if (auto* destination = std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      return DeliverRaw(producer, InMemoryEncodedDestinationAccess::TryClaimWrite(*destination));
    }
    if (auto* sink = FileSink()) {
      return DeliverRaw(producer, sink->TryClaimWrite());
    }
    return {};
  }

  [[nodiscard]] std::optional<DeliveryRetirement> TryRetire(
      std::uint64_t admission_sequence) noexcept {
    if (auto* destination = std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      return InMemoryEncodedDestinationAccess::TryRetire(*destination, admission_sequence);
    }
    if (auto* sink = FileSink()) {
      return sink->TryRetire(admission_sequence);
    }
    return std::nullopt;
  }

  [[nodiscard]] InFlightSummary SummarizeInFlight(std::uint64_t watermark) const noexcept {
    if (const auto* destination = std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      return InMemoryEncodedDestinationAccess::SummarizeInFlight(*destination, watermark);
    }
    if (const auto* sink = FileSink()) {
      return sink->SummarizeInFlight(watermark);
    }
    return {};
  }

  /// Returns the libuv error that failed a file route; in-memory routes never fail this way.
  [[nodiscard]] std::int32_t TerminalError() const noexcept {
    const auto* sink = FileSink();
    return sink != nullptr ? sink->FailureError() : 0;
  }

  void Stop() noexcept {
    if (auto* structured_destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      InMemoryDestinationAccess::Stop(*structured_destination);
    } else if (auto* encoded_destination =
                   std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      InMemoryEncodedDestinationAccess::Stop(*encoded_destination);
    } else if (auto* sink = FileSink()) {
      sink->Stop();
    }
  }

  void RequestClose() noexcept {
    if (auto* sink = FileSink()) {
      sink->RequestClose();
    }
  }

  /// Returns the close result; in-memory routes have nothing to close.
  [[nodiscard]] std::optional<std::int32_t> CloseResult() const noexcept {
    const auto* sink = FileSink();
    return sink != nullptr ? sink->CloseResult() : std::optional<std::int32_t>{0};
  }

  bool JoinIo(std::chrono::steady_clock::time_point deadline) noexcept {
    auto* sink = FileSink();
    return sink == nullptr || sink->JoinUntil(deadline);
  }

  void DiscardInFlight() noexcept {
    if (auto* destination = std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      InMemoryEncodedDestinationAccess::DiscardInFlight(*destination);
    } else if (auto* sink = FileSink()) {
      sink->DiscardInFlight();
    }
  }

  [[nodiscard]] std::size_t FixedBackingBytes() const noexcept {
    if (const auto* destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      return InMemoryDestinationAccess::FixedBackingBytes(*destination);
    }
    if (const auto* destination = std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      return InMemoryEncodedDestinationAccess::FixedBackingBytes(*destination);
    }
    if (const auto* sink = FileSink()) {
      return sink->FixedBackingBytes();
    }
    return 0U;
  }

 private:
  template <typename Claim>
  struct RawStoreContext final {
    Claim* claim{nullptr};
    std::uint64_t admission_sequence{0};
    StoreResult result{};
  };

  [[nodiscard]] RawFileSink* FileSink() noexcept {
    auto* sink = std::get_if<std::unique_ptr<RawFileSink>>(&destination_);
    return sink != nullptr ? sink->get() : nullptr;
  }
  [[nodiscard]] const RawFileSink* FileSink() const noexcept {
    const auto* sink = std::get_if<std::unique_ptr<RawFileSink>>(&destination_);
    return sink != nullptr ? sink->get() : nullptr;
  }

  template <typename Claim>
  [[nodiscard]] static RouteDeliveryResult DeliverRaw(ProducerKernel& producer,
                                                      Claim claim) noexcept {
    if (!claim) {
      return {};
    }
    RawStoreContext<Claim> context{.claim = &claim};
    const ConsumeStatus consumed = producer.TryConsume(&context, &StoreRawRecord<Claim>);
    return {.consumed = consumed,
            .admission_sequence = context.admission_sequence,
            .encoded_bytes = context.result.encoded_bytes,
            .submitted = context.result.submitted};
  }

  static void StoreStructuredRecord(void* context, std::uint64_t sequence,
                                    const detail::producer::RecordView& record) noexcept {
    static_cast<DestinationWriteClaim*>(context)->Store(sequence, record);
  }

  template <typename Claim>
  static void StoreRawRecord(void* context, std::uint64_t sequence,
                             const detail::producer::RecordView& record) noexcept {
    auto& store = *static_cast<RawStoreContext<Claim>*>(context);
    store.admission_sequence = sequence;
    store.result = store.claim->StoreRaw(sequence, record);
  }

  std::variant<testing::InMemoryDestination, testing::InMemoryEncodedDestination,
               std::unique_ptr<RawFileSink>>
      destination_;
  bool attached_{false};
};

class RuntimeDomain final {
 public:
  RuntimeDomain(RuntimeConfig runtime_config, RuntimeRoute runtime_route)
      : config_(runtime_config),
        route_(std::move(runtime_route)),
        producer_(KernelConfig{
            .threshold = config_.threshold,
            .payload_capacity_bytes = config_.payload_capacity_bytes,
            .maximum_record_bytes = config_.maximum_record_bytes,
            .producer_slots = config_.producer_slots,
            .ingress_cells = config_.ingress_cells,
            .consumer_notification = {this, &RuntimeDomain::NotifyConsumer},
        }),
        control_reserve_(config_.control_operations),
        actions_(std::make_unique<ControlAction[]>(config_.control_operations)) {}

  RuntimeDomain(const RuntimeDomain&) = delete;
  RuntimeDomain& operator=(const RuntimeDomain&) = delete;
  RuntimeDomain(RuntimeDomain&&) = delete;
  RuntimeDomain& operator=(RuntimeDomain&&) = delete;

  // Detach before the wake members are destroyed so late destination completions cannot wake us.
  ~RuntimeDomain() { route_.Detach(); }

  [[nodiscard]] std::optional<RuntimeCreateFailure> AttachRoute(
      std::chrono::steady_clock::time_point deadline) noexcept {
    return route_.Attach({.context = this, .notify = &RuntimeDomain::NotifyFromDestination},
                         deadline);
  }

  bool JoinIo(std::chrono::steady_clock::time_point deadline) noexcept {
    return route_.JoinIo(deadline);
  }

  [[nodiscard]] Logger GetLogger() noexcept {
    std::lock_guard lock{state_mutex_};
    const Logger logger = producer_.GetLogger();
    if (!accepting_actions_ || !producer_.IsAdmissionOpen()) {
      return logger;
    }

    auto registration = producer_.TryRegisterProducer();
    if (!registration) {
      return logger;
    }
    for (auto& stored : registrations_) {
      if (!stored.has_value()) {
        stored.emplace(std::move(registration));
        return logger;
      }
    }
    return logger;
  }

  [[nodiscard]] RuntimeSnapshot GetSnapshot() const noexcept {
    const KernelSnapshot producer = producer_.GetSnapshot();
    const auto controls = control_reserve_.GetSnapshot();
    RouteLedger ledger;
    bool route_failed = false;
    std::int32_t route_io_error = 0;
    {
      std::lock_guard lock{state_mutex_};
      ledger = ledger_;
      route_failed = route_failed_;
      route_io_error = route_io_error_;
    }
    const std::uint64_t rejected =
        producer.rejected_no_producer + producer.rejected_lane_full + producer.rejected_budget;
    return RuntimeSnapshot{
        .accepted_records = producer.accepted_records,
        .processed_records = ledger.processed_records,
        .processed_bytes = ledger.processed_bytes,
        .completed_records =
            ledger.delivered_records + ledger.failed_records + ledger.encoding_failed_records,
        .delivered_records = ledger.delivered_records,
        .delivered_bytes = ledger.delivered_bytes,
        .delivery_failed_records = ledger.failed_records,
        .delivery_failed_bytes = ledger.failed_bytes,
        .encoding_failed_records = ledger.encoding_failed_records,
        .rejected_no_producer = producer.rejected_no_producer,
        .rejected_lane_full = producer.rejected_lane_full,
        .rejected_budget = producer.rejected_budget,
        .dropped_newest_records = rejected,
        .retained_records = producer.retained_records,
        .logical_retained_bytes = producer.logical_retained_bytes,
        .physical_retained_bytes = producer.physical_retained_bytes,
        .payload_capacity_bytes = producer.payload_capacity_bytes,
        .fixed_backing_bytes = producer.fixed_backing_bytes + route_.FixedBackingBytes() +
                               controls.node_backing_bytes + controls.dispatcher_state_bytes +
                               config_.control_operations * sizeof(ControlAction),
        .admission_open = producer_.IsAdmissionOpen(),
        .worker_running = worker_running_.load(std::memory_order_acquire),
        .route_failed = route_failed,
        .route_io_error = route_io_error,
    };
  }

  [[nodiscard]] OperationStartResult StartAction(ControlActionKind kind) noexcept {
    auto started = control_reserve_.TryStart();
    if (!started) {
      return {.failure = started.failure};
    }

    std::optional<OperationOutcome> immediate_outcome;
    OperationReport immediate_report{};
    {
      std::lock_guard lock{state_mutex_};
      if (!accepting_actions_) {
        if (destruction_stop_requested_.load(std::memory_order_acquire)) {
          immediate_outcome = OperationOutcome::kCancelled;
        } else if (route_failed_) {
          immediate_outcome = OperationOutcome::kFailed;
        } else {
          immediate_outcome = OperationOutcome::kSucceeded;
        }
        immediate_report = BuildReportLocked(producer_.GetSnapshot().accepted_records);
      } else {
        if (kind == ControlActionKind::kShutdown && !shutdown_requested_) {
          shutdown_requested_ = true;
          CloseAdmissionLocked();
        }

        const std::uint64_t watermark = producer_.GetSnapshot().accepted_records;
        ControlAction* free_action = nullptr;
        for (std::size_t index = 0; index < config_.control_operations; ++index) {
          if (!actions_[index].active) {
            free_action = &actions_[index];
            break;
          }
        }
        if (kind == ControlActionKind::kDrain && watermark == ledger_.retired_records) {
          // Every Record through the watermark already retired, so the ledger is exact now.
          immediate_outcome = OperationOutcome::kSucceeded;
          immediate_report = BuildReportLocked(watermark);
        } else if (free_action == nullptr) {
          immediate_outcome = OperationOutcome::kFailed;
          immediate_report = BuildReportLocked(watermark);
        } else {
          free_action->kind = kind;
          free_action->watermark = watermark;
          free_action->completion = std::move(started.completion);
          free_action->active = true;
        }
      }
    }

    if (immediate_outcome) {
      static_cast<void>(started.completion.TryComplete(*immediate_outcome, immediate_report));
    }
    NotifyControl();
    return {.operation = std::move(started.operation)};
  }

  void Run() noexcept {
    const detail::control::ScopedUlogThreadRole worker_role{
        detail::control::UlogThreadRole::kWorker};
    {
      std::lock_guard lock{state_mutex_};
      worker_started_ = true;
      worker_running_.store(true, std::memory_order_release);
    }
    lifecycle_condition_.notify_all();

    std::optional<OperationOutcome> shutdown_outcome;
    std::uint64_t observed_epoch = wake_epoch_.load(std::memory_order_acquire);
    while (true) {
      if (DestructionStopRequested()) {
        PrepareDestructionStop();
        CompleteActions(OperationOutcome::kCancelled);
        SettleInFlight();
        DrainDiscardedRecords();
        break;
      }

      RetireCompletedDeliveries();
      if (const std::int32_t error = route_.TerminalError(); error != 0) {
        RecordRouteIoError(error);
        PrepareRouteFailure();
        CompleteActions(OperationOutcome::kFailed);
        SettleInFlight();
        DrainDiscardedRecords();
        break;
      }
      shutdown_outcome = TryFinishShutdown();
      if (shutdown_outcome) {
        break;
      }

      const KernelSnapshot snapshot = producer_.GetSnapshot();
      if (snapshot.retained_records != 0U) {
        const RouteDeliveryResult delivery = route_.Deliver(producer_, kWorkerRecheckInterval);
        if (delivery.consumed == ConsumeStatus::kRecord) {
          if (!delivery.submitted) {
            RecordEncodingFailure(delivery.admission_sequence);
            PrepareRouteFailure();
            CompleteActions(OperationOutcome::kFailed);
            SettleInFlight();
            DrainDiscardedRecords();
            break;
          }
          RecordProcessed(delivery);
          continue;
        }
      }

      std::unique_lock lock{wake_mutex_};
      wake_condition_.wait_for(lock, kWorkerRecheckInterval, [&] {
        return wake_epoch_.load(std::memory_order_acquire) != observed_epoch;
      });
      observed_epoch = wake_epoch_.load(std::memory_order_acquire);
    }

    {
      std::lock_guard lock{state_mutex_};
      accepting_actions_ = false;
      worker_running_.store(false, std::memory_order_release);
    }
    if (shutdown_outcome) {
      CompleteActions(*shutdown_outcome);
    }
    {
      std::lock_guard lock{state_mutex_};
      worker_stopped_ = true;
    }
    lifecycle_condition_.notify_all();
  }

  [[nodiscard]] bool WaitUntilStarted(
      const std::chrono::steady_clock::time_point deadline) noexcept {
    std::unique_lock lock{state_mutex_};
    return lifecycle_condition_.wait_until(lock, deadline, [&] { return worker_started_; });
  }

  [[nodiscard]] bool WaitUntilStopped(
      const std::chrono::steady_clock::time_point deadline) noexcept {
    std::unique_lock lock{state_mutex_};
    return lifecycle_condition_.wait_until(lock, deadline, [&] { return worker_stopped_; });
  }

  void RequestDestructionStop() noexcept {
    destruction_stop_requested_.store(true, std::memory_order_release);
    producer_.CloseAdmission();
    route_.Stop();
    NotifyControl();
  }

 private:
  static void NotifyConsumer(void* context) noexcept {
    static_cast<RuntimeDomain*>(context)->Notify();
  }

  static void NotifyFromDestination(void* context) noexcept {
    static_cast<RuntimeDomain*>(context)->NotifyControl();
  }

  static void DiscardRecord(void*, std::uint64_t, const detail::producer::RecordView&) noexcept {}

  void Notify() noexcept {
    wake_epoch_.fetch_add(1, std::memory_order_release);
    wake_condition_.notify_one();
  }

  /// Publishes a wake under the worker's wait lock so the change cannot fall between the worker's
  /// predicate check and its wait. Producer publication keeps the lock-free Notify().
  void NotifyControl() noexcept {
    {
      std::lock_guard lock{wake_mutex_};
      wake_epoch_.fetch_add(1, std::memory_order_release);
    }
    wake_condition_.notify_one();
  }

  void CloseAdmissionLocked() noexcept {
    producer_.CloseAdmission();
    for (auto& registration : registrations_) {
      registration.reset();
    }
  }

  [[nodiscard]] bool DestructionStopRequested() const noexcept {
    return destruction_stop_requested_.load(std::memory_order_acquire);
  }

  void PrepareDestructionStop() noexcept {
    {
      std::lock_guard lock{state_mutex_};
      accepting_actions_ = false;
      CloseAdmissionLocked();
    }
    // Stop again on the worker so each completion either precedes every cancellation report or is
    // rejected as cancelled; the destructor may not have reached its own Stop yet.
    route_.Stop();
  }

  void PrepareRouteFailure() noexcept {
    {
      std::lock_guard lock{state_mutex_};
      route_failed_ = true;
      accepting_actions_ = false;
      CloseAdmissionLocked();
    }
    route_.Stop();
  }

  /// Returns exact accounting for admitted sequences below `watermark`. Registered actions keep
  /// `watermark >= ledger_.retired_records`, so the in-order ledger never includes later Records.
  [[nodiscard]] OperationReport BuildReportLocked(std::uint64_t watermark) const noexcept {
    OperationReport report{.watermark_records = watermark};
    if (ledger_.settled) {
      report.processed_records = ledger_.processed_records;
      report.processed_bytes = ledger_.processed_bytes;
      report.delivered_records = ledger_.delivered_records;
      report.delivered_bytes = ledger_.delivered_bytes;
      report.failed_records = ledger_.failed_records + ledger_.encoding_failed_records;
      report.failed_bytes = ledger_.failed_bytes;
    } else {
      const InFlightSummary in_flight = ledger_.retired_records < watermark
                                            ? route_.SummarizeInFlight(watermark)
                                            : InFlightSummary{};
      const std::uint64_t encoding_failed =
          ledger_.encoding_failed_sequence && *ledger_.encoding_failed_sequence < watermark ? 1U
                                                                                            : 0U;
      report.processed_records = ledger_.retired_records + in_flight.records + encoding_failed;
      report.processed_bytes = ledger_.delivered_bytes + ledger_.failed_bytes + in_flight.bytes;
      report.delivered_records = ledger_.delivered_records + in_flight.delivered_records;
      report.delivered_bytes = ledger_.delivered_bytes + in_flight.delivered_bytes;
      report.failed_records = ledger_.failed_records + in_flight.failed_records + encoding_failed;
      report.failed_bytes = ledger_.failed_bytes + in_flight.failed_bytes;
    }
    report.unfinished_records = watermark - report.delivered_records - report.failed_records;
    report.unfinished_bytes = report.processed_bytes - report.delivered_bytes - report.failed_bytes;
    return report;
  }

  void RecordProcessed(const RouteDeliveryResult& delivery) noexcept {
    {
      std::lock_guard lock{state_mutex_};
      ++ledger_.processed_records;
      ledger_.processed_bytes += delivery.encoded_bytes;
      if (delivery.retired) {
        ++ledger_.retired_records;
        ++ledger_.delivered_records;
        ledger_.delivered_bytes += delivery.encoded_bytes;
      }
    }
    if (delivery.retired) {
      CompleteReadyDrains();
    }
  }

  void RecordEncodingFailure(std::uint64_t admission_sequence) noexcept {
    std::lock_guard lock{state_mutex_};
    ++ledger_.processed_records;
    ++ledger_.encoding_failed_records;
    ledger_.encoding_failed_sequence = admission_sequence;
  }

  /// Retires completed deliveries strictly in admission order, even when the destination
  /// completes them out of order, and completes each Drain exactly at its watermark.
  void RetireCompletedDeliveries() noexcept {
    // Only this worker writes the ledger, so it may read its own values without the lock.
    while (ledger_.retired_records < ledger_.processed_records) {
      const auto retirement = route_.TryRetire(ledger_.retired_records);
      if (!retirement) {
        return;
      }
      {
        std::lock_guard lock{state_mutex_};
        ++ledger_.retired_records;
        if (retirement->outcome == DeliveryOutcome::kDelivered) {
          ++ledger_.delivered_records;
          ledger_.delivered_bytes += retirement->encoded_bytes;
        } else {
          ++ledger_.failed_records;
          ledger_.failed_bytes += retirement->encoded_bytes;
        }
      }
      if (retirement->outcome == DeliveryOutcome::kFailed && route_.TerminalError() != 0) {
        // A route-stopping failure completes every pending action, including Drains that now
        // reach their watermark, as failed on the route-failure path.
        return;
      }
      CompleteReadyDrains();
    }
  }

  /// Folds unretired outcomes into the ledger once the route has stopped for good.
  void SettleInFlight() noexcept {
    {
      std::lock_guard lock{state_mutex_};
      const InFlightSummary in_flight =
          route_.SummarizeInFlight(std::numeric_limits<std::uint64_t>::max());
      ledger_.delivered_records += in_flight.delivered_records;
      ledger_.delivered_bytes += in_flight.delivered_bytes;
      ledger_.failed_records += in_flight.failed_records;
      ledger_.failed_bytes += in_flight.failed_bytes;
      ledger_.settled = true;
    }
    route_.DiscardInFlight();
  }

  [[nodiscard]] std::uint64_t ActionWatermarkLocked(const ControlAction& action) const noexcept {
    // Shutdown covers every admitted Record, including writers that finish after admission closes.
    return action.kind == ControlActionKind::kDrain ? action.watermark
                                                    : producer_.GetSnapshot().accepted_records;
  }

  void CompleteReadyDrains() noexcept {
    while (true) {
      OperationCompletion completion;
      OperationReport report;
      {
        std::lock_guard lock{state_mutex_};
        ControlAction* ready = nullptr;
        for (std::size_t index = 0; index < config_.control_operations; ++index) {
          auto& action = actions_[index];
          if (action.active && action.kind == ControlActionKind::kDrain &&
              action.watermark <= ledger_.retired_records) {
            ready = &action;
            break;
          }
        }
        if (ready == nullptr) {
          return;
        }
        ready->active = false;
        completion = std::move(ready->completion);
        report = BuildReportLocked(ready->watermark);
      }
      static_cast<void>(completion.TryComplete(OperationOutcome::kSucceeded, report));
    }
  }

  /// Returns the Shutdown outcome once every admitted Record retired and the route closed.
  [[nodiscard]] std::optional<OperationOutcome> TryFinishShutdown() noexcept {
    {
      std::lock_guard lock{state_mutex_};
      if (!shutdown_requested_) {
        return std::nullopt;
      }
    }
    if (!producer_.IsQuiescent()) {
      return std::nullopt;
    }
    const auto snapshot = producer_.GetSnapshot();
    {
      std::lock_guard lock{state_mutex_};
      if (ledger_.retired_records != snapshot.accepted_records) {
        return std::nullopt;
      }
    }
    route_.RequestClose();
    const auto closed = route_.CloseResult();
    if (!closed) {
      return std::nullopt;
    }
    if (*closed != 0) {
      RecordRouteIoError(*closed);
      return OperationOutcome::kFailed;
    }
    return OperationOutcome::kSucceeded;
  }

  void RecordRouteIoError(std::int32_t error) noexcept {
    std::lock_guard lock{state_mutex_};
    route_failed_ = true;
    route_io_error_ = error;
  }

  void CompleteActions(OperationOutcome outcome) noexcept {
    while (true) {
      OperationCompletion completion;
      OperationReport report;
      {
        std::lock_guard lock{state_mutex_};
        ControlAction* active = nullptr;
        for (std::size_t index = 0; index < config_.control_operations; ++index) {
          if (actions_[index].active) {
            active = &actions_[index];
            break;
          }
        }
        if (active == nullptr) {
          return;
        }
        active->active = false;
        completion = std::move(active->completion);
        report = BuildReportLocked(ActionWatermarkLocked(*active));
      }
      static_cast<void>(completion.TryComplete(outcome, report));
    }
  }

  void DrainDiscardedRecords() noexcept {
    while (!producer_.IsQuiescent()) {
      const ConsumeStatus consumed = producer_.TryConsume(nullptr, &DiscardRecord);
      if (consumed == ConsumeStatus::kRecord) {
        continue;
      }
      const std::uint64_t observed = wake_epoch_.load(std::memory_order_acquire);
      std::unique_lock lock{wake_mutex_};
      wake_condition_.wait_for(lock, kWorkerRecheckInterval, [&] {
        return wake_epoch_.load(std::memory_order_acquire) != observed;
      });
    }
  }

  RuntimeConfig config_;
  RuntimeRoute route_;
  ProducerKernel producer_;
  ControlReserve control_reserve_;
  std::unique_ptr<ControlAction[]> actions_;
  std::array<std::optional<ProducerKernel::ProducerRegistration>,
             detail::producer::kMaximumProducerSlots>
      registrations_{};
  mutable std::mutex state_mutex_;
  std::condition_variable lifecycle_condition_;
  RouteLedger ledger_{};
  bool worker_started_{false};
  bool worker_stopped_{false};
  bool accepting_actions_{true};
  bool shutdown_requested_{false};
  bool route_failed_{false};
  std::int32_t route_io_error_{0};
  std::atomic<bool> destruction_stop_requested_{false};
  std::atomic<bool> worker_running_{false};
  std::atomic<std::uint64_t> wake_epoch_{0};
  std::mutex wake_mutex_;
  std::condition_variable wake_condition_;
};

enum class WorkerStartStatus : std::uint8_t { kStarted, kThreadFailed, kTimedOut };

}  // namespace

struct Runtime::Impl final {
  static void CreateInto(RuntimeConfig config, RuntimeRoute route,
                         RuntimeCreateResult& result) noexcept {
    result = {};
    try {
      auto domain = std::make_shared<RuntimeDomain>(config, std::move(route));
      if (auto failure =
              domain->AttachRoute(std::chrono::steady_clock::now() + config.startup_timeout)) {
        result.failure = failure;
        return;
      }
      auto impl = std::make_unique<Impl>(domain, config.destruction_timeout);
      switch (impl->Start(config.startup_timeout)) {
        case WorkerStartStatus::kStarted:
          result.runtime.reset(new Runtime{std::move(impl)});
          return;
        case WorkerStartStatus::kThreadFailed:
          result.failure.emplace(RuntimeCreateErrorCode::kWorkerStartFailed);
          return;
        case WorkerStartStatus::kTimedOut:
          result.failure.emplace(RuntimeCreateErrorCode::kWorkerStartupTimedOut);
          return;
      }
    } catch (const std::bad_alloc&) {
      result.failure.emplace(RuntimeCreateErrorCode::kAllocationFailed);
      return;
    } catch (const std::system_error&) {
      result.failure.emplace(RuntimeCreateErrorCode::kWorkerStartFailed);
      return;
    } catch (...) {
      result.failure.emplace(RuntimeCreateErrorCode::kAllocationFailed);
      return;
    }
    result.failure.emplace(RuntimeCreateErrorCode::kWorkerStartFailed);
  }

  static void CreateFileInto(RuntimeConfig config, const RawFileRouteConfig& route,
                             const detail::io::FileFaultPlan& faults,
                             RuntimeCreateResult& result) noexcept {
    result = {};
    if (auto failure = ValidateRuntimeConfig(config)) {
      result.failure = failure;
      return;
    }
    if (auto failure = ValidateFileRoute(route)) {
      result.failure = failure;
      return;
    }
    std::unique_ptr<RawFileSink> sink;
    try {
      auto path = FilePathBytes(route.path);
      if (!path) {
        result.failure.emplace(RuntimeCreateErrorCode::kInvalidFilePath);
        return;
      }
      sink = std::make_unique<RawFileSink>(std::move(*path), route.write_buffers,
                                           config.maximum_record_bytes, faults);
    } catch (const std::bad_alloc&) {
      result.failure.emplace(RuntimeCreateErrorCode::kAllocationFailed);
      return;
    } catch (...) {
      // Path conversion rejects names that have no UTF-8 representation.
      result.failure.emplace(RuntimeCreateErrorCode::kInvalidFilePath);
      return;
    }
    CreateInto(config, RuntimeRoute{std::move(sink)}, result);
  }

  Impl(std::shared_ptr<RuntimeDomain> runtime_domain,
       std::chrono::milliseconds runtime_destruction_timeout) noexcept
      : domain(std::move(runtime_domain)), destruction_timeout(runtime_destruction_timeout) {}

  ~Impl() { StopAndJoin(); }

  [[nodiscard]] WorkerStartStatus Start(std::chrono::milliseconds startup_timeout) noexcept {
    try {
      worker = std::thread{[runtime_domain = domain] { runtime_domain->Run(); }};
    } catch (const std::system_error&) {
      return WorkerStartStatus::kThreadFailed;
    }
    if (!domain->WaitUntilStarted(std::chrono::steady_clock::now() + startup_timeout)) {
      return WorkerStartStatus::kTimedOut;
    }
    return WorkerStartStatus::kStarted;
  }

  void StopAndJoin() noexcept {
    const auto deadline = std::chrono::steady_clock::now() + destruction_timeout;
    domain->RequestDestructionStop();
    if (worker.joinable()) {
      if (domain->WaitUntilStopped(deadline)) {
        worker.join();
      } else {
        worker.detach();
      }
    }
    // A file route's loop thread finishes its active append and closes the file after Stop.
    static_cast<void>(domain->JoinIo(deadline));
  }

  std::shared_ptr<RuntimeDomain> domain;
  std::thread worker;
  std::chrono::milliseconds destruction_timeout;
};

std::string_view RuntimeCreateFailure::Message() const noexcept { return FailureMessage(code); }

std::string_view RuntimeCreateFailure::HowToFix() const noexcept { return FailureFix(code); }

RuntimeCreateResult Runtime::Create(RuntimeConfig config,
                                    testing::InMemoryDestination destination) noexcept {
  if (const auto failure = ValidateConfig(config, destination)) {
    return {.failure = failure};
  }
  RuntimeCreateResult result;
  Impl::CreateInto(config, RuntimeRoute{std::move(destination)}, result);
  return result;
}

RuntimeCreateResult Runtime::Create(RuntimeConfig config,
                                    testing::InMemoryEncodedDestination destination) noexcept {
  if (const auto failure = ValidateConfig(config, destination)) {
    return {.failure = failure};
  }
  RuntimeCreateResult result;
  Impl::CreateInto(config, RuntimeRoute{std::move(destination)}, result);
  return result;
}

RuntimeCreateResult Runtime::Create(RuntimeConfig config, RawFileRouteConfig route) noexcept {
  RuntimeCreateResult result;
  Impl::CreateFileInto(config, route, {}, result);
  return result;
}

RuntimeCreateResult detail::RuntimeFactoryAccess::CreateRawFileRuntime(
    RuntimeConfig config, const RawFileRouteConfig& route,
    const io::FileFaultPlan& faults) noexcept {
  RuntimeCreateResult result;
  Runtime::Impl::CreateFileInto(config, route, faults, result);
  return result;
}

Runtime::Runtime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

Runtime::~Runtime() = default;

Logger Runtime::GetLogger() noexcept { return impl_->domain->GetLogger(); }

RuntimeSnapshot Runtime::GetSnapshot() const noexcept { return impl_->domain->GetSnapshot(); }

OperationStartResult Runtime::Drain() noexcept {
  return impl_->domain->StartAction(ControlActionKind::kDrain);
}

OperationStartResult Runtime::Shutdown() noexcept {
  return impl_->domain->StartAction(ControlActionKind::kShutdown);
}

}  // namespace ulog
