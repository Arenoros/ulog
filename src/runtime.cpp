#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
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
#include "producer/producer_kernel.hpp"
#include "testing/in_memory_destination_access.hpp"
#include "testing/in_memory_encoded_destination_access.hpp"

namespace ulog {
namespace {

using namespace std::chrono_literals;
using detail::control::ControlReserve;
using detail::control::OperationCompletion;
using detail::producer::ConsumeStatus;
using detail::producer::KernelConfig;
using detail::producer::KernelSnapshot;
using detail::producer::ProducerKernel;
using detail::testing::DestinationWriteClaim;
using detail::testing::EncodedDestinationStoreResult;
using detail::testing::EncodedDestinationWriteClaim;
using detail::testing::InMemoryDestinationAccess;
using detail::testing::InMemoryEncodedDestinationAccess;

constexpr std::size_t kMinimumRecordBytes = 128;
constexpr std::size_t kMaximumControlOperations = 64;
constexpr auto kWorkerRecheckInterval = 10ms;
constexpr auto kMaximumLifecycleTimeout = std::chrono::hours{24};

template <typename Destination>
[[nodiscard]] std::optional<RuntimeCreateFailure> ValidateConfig(
    const RuntimeConfig& config, const Destination& destination) noexcept {
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
      return "The in-memory Runtime tracer supports exactly one worker.";
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
      return "Set worker_threads to 1 for the in-memory tracer.";
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
  }
  return "Correct the Runtime configuration and retry.";
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
  std::size_t delivered_bytes{0};
  bool committed{false};
};

class RuntimeRoute final {
 public:
  explicit RuntimeRoute(testing::InMemoryDestination destination) noexcept
      : destination_(std::move(destination)) {}
  explicit RuntimeRoute(testing::InMemoryEncodedDestination destination) noexcept
      : destination_(std::move(destination)) {}

  RuntimeRoute(RuntimeRoute&& other) noexcept
      : destination_(std::move(other.destination_)),
        attached_(std::exchange(other.attached_, false)) {}
  RuntimeRoute& operator=(RuntimeRoute&&) = delete;
  RuntimeRoute(const RuntimeRoute&) = delete;
  RuntimeRoute& operator=(const RuntimeRoute&) = delete;

  ~RuntimeRoute() { Detach(); }

  [[nodiscard]] bool TryAttach() noexcept {
    if (auto* structured_destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      attached_ = InMemoryDestinationAccess::TryAttachRuntime(*structured_destination);
    } else if (auto* encoded_destination =
                   std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      attached_ = InMemoryEncodedDestinationAccess::TryAttachRuntime(*encoded_destination);
    } else {
      attached_ = false;
    }
    return attached_;
  }

  [[nodiscard]] RouteDeliveryResult WaitAndDeliver(
      ProducerKernel& producer, std::chrono::steady_clock::duration recheck_interval) noexcept {
    if (auto* destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      DestinationWriteClaim claim =
          InMemoryDestinationAccess::WaitForWrite(*destination, recheck_interval);
      if (!claim) {
        return {};
      }
      const ConsumeStatus consumed = producer.TryConsume(&claim, &StoreStructuredRecord);
      return {.consumed = consumed,
              .delivered_bytes = 0U,
              .committed = consumed == ConsumeStatus::kRecord};
    }
    if (auto* destination = std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      EncodedDestinationWriteClaim claim =
          InMemoryEncodedDestinationAccess::WaitForWrite(*destination, recheck_interval);
      if (!claim) {
        return {};
      }
      EncodedStoreContext context{.claim = &claim};
      const ConsumeStatus consumed = producer.TryConsume(&context, &StoreEncodedRecord);
      return {.consumed = consumed,
              .delivered_bytes = context.result.encoded_bytes,
              .committed = context.result.committed};
    }
    return {};
  }

  void Stop() noexcept {
    if (auto* structured_destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      InMemoryDestinationAccess::Stop(*structured_destination);
    } else if (auto* encoded_destination =
                   std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      InMemoryEncodedDestinationAccess::Stop(*encoded_destination);
    }
  }

  [[nodiscard]] std::size_t FixedBackingBytes() const noexcept {
    if (const auto* destination = std::get_if<testing::InMemoryDestination>(&destination_)) {
      return InMemoryDestinationAccess::FixedBackingBytes(*destination);
    }
    if (const auto* destination = std::get_if<testing::InMemoryEncodedDestination>(&destination_)) {
      return InMemoryEncodedDestinationAccess::FixedBackingBytes(*destination);
    }
    return 0U;
  }

 private:
  struct EncodedStoreContext final {
    EncodedDestinationWriteClaim* claim{nullptr};
    EncodedDestinationStoreResult result{};
  };

  static void StoreStructuredRecord(void* context, std::uint64_t sequence,
                                    const detail::producer::RecordView& record) noexcept {
    static_cast<DestinationWriteClaim*>(context)->Store(sequence, record);
  }

  static void StoreEncodedRecord(void* context, std::uint64_t sequence,
                                 const detail::producer::RecordView& record) noexcept {
    auto& store = *static_cast<EncodedStoreContext*>(context);
    store.result = store.claim->StoreRaw(sequence, record);
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
    }
    attached_ = false;
  }

  std::variant<testing::InMemoryDestination, testing::InMemoryEncodedDestination> destination_;
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

  [[nodiscard]] bool TryAttachDestination() noexcept { return route_.TryAttach(); }

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
    const std::uint64_t rejected =
        producer.rejected_no_producer + producer.rejected_lane_full + producer.rejected_budget;
    return RuntimeSnapshot{
        .accepted_records = producer.accepted_records,
        .completed_records = completed_records_.load(std::memory_order_relaxed),
        .delivered_records = delivered_records_.load(std::memory_order_relaxed),
        .delivered_bytes = delivered_bytes_.load(std::memory_order_relaxed),
        .encoding_failed_records = encoding_failed_records_.load(std::memory_order_relaxed),
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
    };
  }

  [[nodiscard]] OperationStartResult StartAction(ControlActionKind kind) noexcept {
    auto started = control_reserve_.TryStart();
    if (!started) {
      return {.failure = started.failure};
    }

    bool complete_immediately = false;
    OperationOutcome immediate_outcome = OperationOutcome::kSucceeded;
    {
      std::lock_guard lock{state_mutex_};
      if (!accepting_actions_) {
        complete_immediately = true;
        if (destruction_stop_requested_.load(std::memory_order_acquire)) {
          immediate_outcome = OperationOutcome::kCancelled;
        } else if (delivery_failed_) {
          immediate_outcome = OperationOutcome::kFailed;
        } else {
          immediate_outcome = OperationOutcome::kSucceeded;
        }
      } else {
        if (kind == ControlActionKind::kShutdown && !shutdown_requested_) {
          shutdown_requested_ = true;
          CloseAdmissionLocked();
        }

        ControlAction* free_action = nullptr;
        for (std::size_t index = 0; index < config_.control_operations; ++index) {
          if (!actions_[index].active) {
            free_action = &actions_[index];
            break;
          }
        }
        if (free_action == nullptr) {
          complete_immediately = true;
          immediate_outcome = OperationOutcome::kFailed;
        } else {
          free_action->kind = kind;
          free_action->watermark = producer_.GetSnapshot().accepted_records;
          free_action->completion = std::move(started.completion);
          free_action->active = true;
        }
      }
    }

    if (complete_immediately) {
      static_cast<void>(started.completion.TryComplete(immediate_outcome));
    }
    Notify();
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

    bool shutdown_succeeded = false;
    std::uint64_t observed_epoch = wake_epoch_.load(std::memory_order_acquire);
    while (true) {
      if (DestructionStopRequested()) {
        PrepareDestructionStop();
        CancelActions();
        DrainDiscardedRecords();
        break;
      }

      CompleteReadyDrains();
      if (TryFinishShutdown()) {
        shutdown_succeeded = true;
        break;
      }

      const KernelSnapshot snapshot = producer_.GetSnapshot();
      if (snapshot.retained_records != 0U) {
        const RouteDeliveryResult delivery =
            route_.WaitAndDeliver(producer_, kWorkerRecheckInterval);
        if (delivery.consumed == ConsumeStatus::kRecord) {
          completed_records_.fetch_add(1, std::memory_order_relaxed);
          if (delivery.committed) {
            delivered_records_.fetch_add(1, std::memory_order_relaxed);
            delivered_bytes_.fetch_add(delivery.delivered_bytes, std::memory_order_relaxed);
          } else {
            encoding_failed_records_.fetch_add(1, std::memory_order_relaxed);
            PrepareDeliveryFailure();
            CompleteActions(OperationOutcome::kFailed);
            DrainDiscardedRecords();
            break;
          }
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
    if (shutdown_succeeded) {
      CompleteActions(OperationOutcome::kSucceeded);
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
    Notify();
  }

 private:
  static void NotifyConsumer(void* context) noexcept {
    static_cast<RuntimeDomain*>(context)->Notify();
  }

  static void DiscardRecord(void*, std::uint64_t, const detail::producer::RecordView&) noexcept {}

  void Notify() noexcept {
    wake_epoch_.fetch_add(1, std::memory_order_release);
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
    std::lock_guard lock{state_mutex_};
    accepting_actions_ = false;
    CloseAdmissionLocked();
  }

  void PrepareDeliveryFailure() noexcept {
    {
      std::lock_guard lock{state_mutex_};
      delivery_failed_ = true;
      accepting_actions_ = false;
      CloseAdmissionLocked();
    }
    route_.Stop();
  }

  [[nodiscard]] std::optional<OperationCompletion> PopReadyDrain(std::uint64_t delivered) noexcept {
    std::lock_guard lock{state_mutex_};
    for (std::size_t index = 0; index < config_.control_operations; ++index) {
      auto& action = actions_[index];
      if (action.active && action.kind == ControlActionKind::kDrain &&
          action.watermark <= delivered) {
        action.active = false;
        return std::move(action.completion);
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<OperationCompletion> PopAnyAction() noexcept {
    std::lock_guard lock{state_mutex_};
    for (std::size_t index = 0; index < config_.control_operations; ++index) {
      auto& action = actions_[index];
      if (action.active) {
        action.active = false;
        return std::move(action.completion);
      }
    }
    return std::nullopt;
  }

  void CompleteReadyDrains() noexcept {
    const std::uint64_t delivered = delivered_records_.load(std::memory_order_relaxed);
    while (auto completion = PopReadyDrain(delivered)) {
      static_cast<void>(completion->TryComplete(OperationOutcome::kSucceeded));
    }
  }

  [[nodiscard]] bool TryFinishShutdown() noexcept {
    {
      std::lock_guard lock{state_mutex_};
      if (!shutdown_requested_) {
        return false;
      }
    }
    if (!producer_.IsQuiescent()) {
      return false;
    }
    const auto snapshot = producer_.GetSnapshot();
    if (delivered_records_.load(std::memory_order_relaxed) != snapshot.accepted_records) {
      return false;
    }

    return true;
  }

  void CompleteActions(OperationOutcome outcome) noexcept {
    while (auto completion = PopAnyAction()) {
      static_cast<void>(completion->TryComplete(outcome));
    }
  }

  void CancelActions() noexcept { CompleteActions(OperationOutcome::kCancelled); }

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
  bool worker_started_{false};
  bool worker_stopped_{false};
  bool accepting_actions_{true};
  bool shutdown_requested_{false};
  bool delivery_failed_{false};
  std::atomic<bool> destruction_stop_requested_{false};
  std::atomic<bool> worker_running_{false};
  std::atomic<std::uint64_t> completed_records_{0};
  std::atomic<std::uint64_t> delivered_records_{0};
  std::atomic<std::uint64_t> delivered_bytes_{0};
  std::atomic<std::uint64_t> encoding_failed_records_{0};
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
      if (!domain->TryAttachDestination()) {
        result.failure.emplace(RuntimeCreateErrorCode::kInvalidDestination);
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
    if (!worker.joinable()) {
      return;
    }
    const auto deadline = std::chrono::steady_clock::now() + destruction_timeout;
    domain->RequestDestructionStop();
    if (domain->WaitUntilStopped(deadline)) {
      worker.join();
    } else {
      worker.detach();
    }
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
