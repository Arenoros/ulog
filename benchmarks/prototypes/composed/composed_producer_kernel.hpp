#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#include "prototypes/ingress/per_producer_lanes.hpp"
#include "prototypes/record_storage/record_storage.hpp"
#include "prototypes/reservation/producer_credit_ledger.hpp"
#include "support/workload_harness.hpp"

namespace ulog::benchmark_support::composed {

using Writer = record_storage::ContiguousRecordSlot::Writer;

template <typename Callback>
concept WriterCallback = std::is_nothrow_invocable_v<Callback&&, Writer&> &&
                         (std::same_as<std::invoke_result_t<Callback&&, Writer&>, void> ||
                          std::same_as<std::invoke_result_t<Callback&&, Writer&>, bool>);

template <WriterCallback Callback>
[[nodiscard]] bool InvokeWriterCallback(Callback&& callback, Writer& writer) noexcept {
  if constexpr (std::same_as<std::invoke_result_t<Callback&&, Writer&>, bool>) {
    return std::invoke(std::forward<Callback>(callback), writer);
  } else {
    std::invoke(std::forward<Callback>(callback), writer);
    return true;
  }
}

class RecordPlan final {
 public:
  [[nodiscard]] static constexpr RecordPlan Benchmark(std::size_t maximum_message_bytes) noexcept {
    return RecordPlan{record_storage::BenchmarkRecordSeed(),
                      record_storage::BenchmarkRecordShape<record_storage::ContiguousPolicy>(
                          maximum_message_bytes)};
  }

  [[nodiscard]] constexpr const record_storage::RecordSeed& seed() const noexcept { return seed_; }
  [[nodiscard]] constexpr const RecordFootprint& maximum_footprint() const noexcept {
    return maximum_footprint_;
  }

 private:
  constexpr RecordPlan(record_storage::RecordSeed seed, RecordFootprint maximum_footprint) noexcept
      : seed_(seed), maximum_footprint_(maximum_footprint) {}

  record_storage::RecordSeed seed_;
  RecordFootprint maximum_footprint_;
};

enum class ProduceStatus : std::uint8_t {
  kAccepted,
  kIngressRejected,
  kBudgetRejected,
  kInvalid,
};

struct ProduceResult final {
  ProduceStatus status{ProduceStatus::kInvalid};
  std::optional<std::uint64_t> admission_sequence{};
};

struct ComposedSnapshot final {
  std::uint64_t attempted_records{0};
  std::uint64_t accepted_records{0};
  std::uint64_t rejected_records{0};
  std::uint64_t message_callback_count{0};
  std::uint64_t context_callback_count{0};
  std::uint64_t published_records{0};
  std::uint64_t consumed_records{0};
  std::uint64_t logical_retained_bytes{0};
  std::uint64_t physical_retained_bytes{0};
  std::uint64_t retained_limit_bytes{0};
  std::uint64_t fifo_error_count{0};
  std::uint64_t record_validation_error_count{0};
  std::uint64_t publication_error_count{0};
  ingress::TopologySnapshot topology{};
};

template <std::size_t IngressCapacity = 64>
class ComposedProducerPath final {
  static_assert(IngressCapacity > 0);
  static_assert(IngressCapacity <= std::numeric_limits<std::uint32_t>::max());

 public:
  ComposedProducerPath(std::size_t capacity_bytes, std::size_t baseline_bytes,
                       std::size_t producer_count)
      : topology_(producer_count),
        record_slots_(std::make_unique<RecordSlots>()),
        producer_count_(producer_count) {
    if (producer_count == 0 || producer_count > ingress::kMaximumConcurrentPublishers ||
        producer_count > IngressCapacity) {
      throw std::invalid_argument(
          "Composed producer_count must be between 1 and min(32, ingress capacity); assign one "
          "stable producer index per producer thread and retry.");
    }
    ledger_.Reset(capacity_bytes, baseline_bytes, producer_count);
  }

  ComposedProducerPath(const ComposedProducerPath&) = delete;
  ComposedProducerPath& operator=(const ComposedProducerPath&) = delete;
  ComposedProducerPath(ComposedProducerPath&&) = delete;
  ComposedProducerPath& operator=(ComposedProducerPath&&) = delete;

  [[nodiscard]] static constexpr std::size_t RecordBackingStorageBytes() noexcept {
    return IngressCapacity * sizeof(record_storage::ContiguousRecordSlot);
  }

  template <typename MessageCallback, typename ContextCallback>
    requires WriterCallback<MessageCallback> && WriterCallback<ContextCallback>
  [[nodiscard]] ProduceResult TryProduce(std::size_t producer_index, const RecordPlan& plan,
                                         MessageCallback&& message_callback,
                                         ContextCallback&& context_callback) noexcept {
    if (producer_index >= producer_count_) {
      invalid_attempted_records_.fetch_add(1, std::memory_order_relaxed);
      invalid_rejected_records_.fetch_add(1, std::memory_order_relaxed);
      return {.status = ProduceStatus::kInvalid};
    }

    ProducerCounters& counters = producer_counters_[producer_index];
    ++counters.attempted_records;

    auto publication = topology_.TryClaimPublication(producer_index);
    if (!publication) {
      ++counters.rejected_records;
      return {.status = publication.status() == ingress::PublishStatus::kInvalid
                            ? ProduceStatus::kInvalid
                            : ProduceStatus::kIngressRejected};
    }

    const auto cell_index = publication.cell_index();
    if (!cell_index || *cell_index >= IngressCapacity) {
      ++counters.rejected_records;
      publication_error_count_.fetch_add(1, std::memory_order_relaxed);
      return {.status = ProduceStatus::kInvalid};
    }

    auto reservation = ledger_.TryReserve(
        producer_index, static_cast<std::size_t>(plan.maximum_footprint().SerializedBytes()));
    if (!reservation) {
      ++counters.rejected_records;
      return {.status = ProduceStatus::kBudgetRejected};
    }
    auto& slot = (*record_slots_)[*cell_index];
    auto writer = slot.Begin(plan.seed(), plan.maximum_footprint());
    if (!writer) {
      ++counters.rejected_records;
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return {.status = ProduceStatus::kInvalid};
    }

    ++counters.context_callback_count;
    const bool context_stored =
        InvokeWriterCallback(std::forward<ContextCallback>(context_callback), writer);
    ++counters.message_callback_count;
    const bool message_stored =
        InvokeWriterCallback(std::forward<MessageCallback>(message_callback), writer);
    if (!context_stored || !message_stored) {
      slot.Reset();
      ++counters.rejected_records;
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return {.status = ProduceStatus::kInvalid};
    }
    const record_storage::RecordView view = std::move(writer).Publish();
    if (!view || view.footprint().SerializedBytes() > plan.maximum_footprint().SerializedBytes() ||
        view.footprint().accounting_charge_bytes >
            plan.maximum_footprint().accounting_charge_bytes) {
      slot.Reset();
      ++counters.rejected_records;
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return {.status = ProduceStatus::kInvalid};
    }

    auto ownership =
        std::move(reservation).Commit(static_cast<std::size_t>(view.footprint().SerializedBytes()));
    if (!ownership || ownership.retained_bytes() != view.footprint().SerializedBytes() ||
        ownership.charge_bytes() != view.footprint().accounting_charge_bytes) {
      slot.Reset();
      ++counters.rejected_records;
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      return {.status = ProduceStatus::kInvalid};
    }

    const std::uint64_t generation = ++slot_generations_[*cell_index];
    record_views_[*cell_index] = view;
    ownerships_[*cell_index].emplace(std::move(ownership));
    const ingress::PublishResult published =
        topology_.Publish(std::move(publication),
                          ingress::RecordHandle{
                              .slot_index = static_cast<std::uint32_t>(*cell_index),
                              .generation = generation,
                              .serialized_bytes = view.footprint().SerializedBytes(),
                              .accounting_charge_bytes = view.footprint().accounting_charge_bytes,
                          });
    if (published.status != ingress::PublishStatus::kAccepted || !published.admission_sequence) {
      ownerships_[*cell_index].reset();
      record_views_[*cell_index] = {};
      slot.Reset();
      ++counters.rejected_records;
      publication_error_count_.fetch_add(1, std::memory_order_relaxed);
      return {.status = ProduceStatus::kInvalid};
    }

    ++counters.accepted_records;
    return {
        .status = ProduceStatus::kAccepted,
        .admission_sequence = published.admission_sequence,
    };
  }

  template <typename ConsumerCallback>
    requires std::is_nothrow_invocable_r_v<void, ConsumerCallback&&, std::uint64_t,
                                           const record_storage::RecordView&>
  [[nodiscard]] ingress::ConsumeStatus TryConsume(ConsumerCallback&& consumer_callback) noexcept {
    auto consumption = topology_.TryClaimConsumption();
    if (!consumption) {
      return consumption.status();
    }

    const auto& envelope = consumption.record();
    if (!envelope || envelope->record.slot_index >= IngressCapacity) {
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
      consumption.Acknowledge();
      return ingress::ConsumeStatus::kRecord;
    }

    const std::size_t cell_index = envelope->record.slot_index;
    const auto& view = record_views_[cell_index];
    const auto& ownership = ownerships_[cell_index];
    if (envelope->admission_sequence != expected_consumption_sequence_) {
      fifo_error_count_.fetch_add(1, std::memory_order_relaxed);
    }
    ++expected_consumption_sequence_;

    const bool valid_record =
        envelope->record.generation == slot_generations_[cell_index] && view && ownership &&
        ownership->retained_bytes() == envelope->record.serialized_bytes &&
        ownership->charge_bytes() == envelope->record.accounting_charge_bytes &&
        view.footprint().SerializedBytes() == envelope->record.serialized_bytes &&
        view.footprint().accounting_charge_bytes == envelope->record.accounting_charge_bytes;
    if (!valid_record) {
      record_validation_error_count_.fetch_add(1, std::memory_order_relaxed);
    } else {
      std::invoke(std::forward<ConsumerCallback>(consumer_callback), envelope->admission_sequence,
                  std::as_const(view));
    }

    ownerships_[cell_index].reset();
    record_views_[cell_index] = {};
    (*record_slots_)[cell_index].Reset();
    consumed_records_.fetch_add(1, std::memory_order_relaxed);
    consumption.Acknowledge();
    return ingress::ConsumeStatus::kRecord;
  }

  [[nodiscard]] ComposedSnapshot Snapshot() const noexcept {
    std::uint64_t attempted_records = invalid_attempted_records_.load(std::memory_order_relaxed);
    std::uint64_t accepted_records = 0;
    std::uint64_t rejected_records = invalid_rejected_records_.load(std::memory_order_relaxed);
    std::uint64_t message_callback_count = 0;
    std::uint64_t context_callback_count = 0;
    for (std::size_t producer = 0; producer < producer_count_; ++producer) {
      const ProducerCounters& counters = producer_counters_[producer];
      attempted_records += counters.attempted_records;
      accepted_records += counters.accepted_records;
      rejected_records += counters.rejected_records;
      message_callback_count += counters.message_callback_count;
      context_callback_count += counters.context_callback_count;
    }

    const auto ledger = ledger_.GetSnapshot();
    const auto topology = MeasurementTopologySnapshot();
    return {
        .attempted_records = attempted_records,
        .accepted_records = accepted_records,
        .rejected_records = rejected_records,
        .message_callback_count = message_callback_count,
        .context_callback_count = context_callback_count,
        .published_records = topology.enqueued_records,
        .consumed_records = consumed_records_.load(std::memory_order_relaxed),
        .logical_retained_bytes = ledger.logical_retained_bytes,
        .physical_retained_bytes = ledger.physical_retained_bytes,
        .retained_limit_bytes = ledger.capacity_bytes,
        .fifo_error_count = fifo_error_count_.load(std::memory_order_relaxed),
        .record_validation_error_count =
            record_validation_error_count_.load(std::memory_order_relaxed),
        .publication_error_count = publication_error_count_.load(std::memory_order_relaxed),
        .topology = topology,
    };
  }

  void BeginMeasurement() noexcept {
    topology_measurement_baseline_ = topology_.GetSnapshot();
    for (auto& counters : producer_counters_) {
      counters = ProducerCounters{};
    }
    invalid_attempted_records_.store(0, std::memory_order_relaxed);
    invalid_rejected_records_.store(0, std::memory_order_relaxed);
    consumed_records_.store(0, std::memory_order_relaxed);
    fifo_error_count_.store(0, std::memory_order_relaxed);
    record_validation_error_count_.store(0, std::memory_order_relaxed);
    publication_error_count_.store(0, std::memory_order_relaxed);
  }

  [[nodiscard]] ingress::TopologySnapshot MeasurementTopologySnapshot() const noexcept {
    const auto current = topology_.GetSnapshot();
    return {
        .attempted_records =
            current.attempted_records - topology_measurement_baseline_.attempted_records,
        .enqueued_records =
            current.enqueued_records - topology_measurement_baseline_.enqueued_records,
        .dequeued_records =
            current.dequeued_records - topology_measurement_baseline_.dequeued_records,
        .rejected_records =
            current.rejected_records - topology_measurement_baseline_.rejected_records,
        .full_rejections = current.full_rejections - topology_measurement_baseline_.full_rejections,
        .contention_rejections =
            current.contention_rejections - topology_measurement_baseline_.contention_rejections,
        .invalid_rejections =
            current.invalid_rejections - topology_measurement_baseline_.invalid_rejections,
        .retained_records = current.retained_records,
        .retained_serialized_bytes = current.retained_serialized_bytes,
        .retained_charge_bytes = current.retained_charge_bytes,
    };
  }

  void ReturnAllCredits() { ledger_.ReturnAllCredits(); }

 private:
  static constexpr std::size_t kCacheLineBytes = 64;

  struct alignas(kCacheLineBytes) ProducerCounters final {
    std::uint64_t attempted_records{0};
    std::uint64_t accepted_records{0};
    std::uint64_t rejected_records{0};
    std::uint64_t message_callback_count{0};
    std::uint64_t context_callback_count{0};
    std::array<std::byte, kCacheLineBytes - 5U * sizeof(std::uint64_t)> cache_line_padding{};
  };

  static_assert(sizeof(ProducerCounters) == kCacheLineBytes);

  reservation::ProducerCreditLedger ledger_{};
  ingress::PerProducerLanes<IngressCapacity> topology_;
  using RecordSlots = std::array<record_storage::ContiguousRecordSlot, IngressCapacity>;
  std::unique_ptr<RecordSlots> record_slots_;
  std::array<record_storage::RecordView, IngressCapacity> record_views_{};
  std::array<std::optional<reservation::ProducerCreditLedger::Ownership>, IngressCapacity>
      ownerships_{};
  std::array<std::uint64_t, IngressCapacity> slot_generations_{};
  std::array<ProducerCounters, ingress::kMaximumConcurrentPublishers> producer_counters_{};
  ingress::TopologySnapshot topology_measurement_baseline_{};
  std::size_t producer_count_{0};
  std::uint64_t expected_consumption_sequence_{0};
  std::atomic<std::uint64_t> invalid_attempted_records_{0};
  std::atomic<std::uint64_t> invalid_rejected_records_{0};
  std::atomic<std::uint64_t> consumed_records_{0};
  std::atomic<std::uint64_t> fifo_error_count_{0};
  std::atomic<std::uint64_t> record_validation_error_count_{0};
  std::atomic<std::uint64_t> publication_error_count_{0};
};

[[nodiscard]] inline bool AddBenchmarkContext(Writer& writer) noexcept {
  return record_storage::AddBenchmarkFields(writer);
}

class ComposedProducerKernel final {
 public:
  class Attempt final {
   public:
    Attempt(Attempt&& other) noexcept
        : status_(other.status_), active_(std::exchange(other.active_, false)) {}
    Attempt& operator=(Attempt&&) = delete;
    Attempt(const Attempt&) = delete;
    Attempt& operator=(const Attempt&) = delete;

    [[nodiscard]] AttemptStatus status() const noexcept { return status_; }

   private:
    friend class ComposedProducerKernel;

    Attempt(AttemptStatus status, bool active) noexcept : status_(status), active_(active) {}

    AttemptStatus status_{AttemptStatus::kRejected};
    bool active_{false};
  };

  ComposedProducerKernel() noexcept = default;
  ComposedProducerKernel(const ComposedProducerKernel&) = delete;
  ComposedProducerKernel& operator=(const ComposedProducerKernel&) = delete;
  ComposedProducerKernel(ComposedProducerKernel&&) = delete;
  ComposedProducerKernel& operator=(ComposedProducerKernel&&) = delete;
  ~ComposedProducerKernel() { StopConsumer(); }

  [[nodiscard]] static constexpr std::string_view Name() noexcept { return "composed-producer"; }

  [[nodiscard]] static constexpr WorkloadAdmissionModel AdmissionModel() noexcept {
    return WorkloadAdmissionModel::kExactCapacity;
  }

  void Prepare(const WorkloadCase& workload) {
    if (prepared_) {
      throw std::logic_error(
          "ComposedProducerKernel may be prepared only once because publication sequences are "
          "monotonic; construct a fresh kernel and retry.");
    }
    ValidateWorkloadCase(workload);
    core_.emplace(workload.capacity_bytes, InitialOccupancyBytes(workload),
                  workload.producer_count);
    producer_count_ = workload.producer_count;
    release_arrivals_.store(0, std::memory_order_relaxed);
    drain_requested_.store(0, std::memory_order_relaxed);
    drain_completed_.store(0, std::memory_order_relaxed);
    stop_requested_.store(false, std::memory_order_relaxed);
    consumer_thread_ = std::thread{[this] { ConsumerLoop(); }};
    prepared_ = true;
  }

  void BeginMeasurement() noexcept {
    if (!core_) {
      lifecycle_error_count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    core_->BeginMeasurement();
    const auto initial = core_->Snapshot();
    logical_initial_bytes_ = initial.logical_retained_bytes;
    logical_high_water_bytes_ = logical_initial_bytes_;
    physical_initial_bytes_ = initial.physical_retained_bytes;
    physical_high_water_bytes_ = physical_initial_bytes_;
    lifecycle_error_count_.store(0, std::memory_order_relaxed);
  }

  void ObserveRetainedHighWater() noexcept {
    if (!core_) {
      lifecycle_error_count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const auto snapshot = core_->Snapshot();
    logical_high_water_bytes_ =
        std::max(logical_high_water_bytes_, snapshot.logical_retained_bytes);
    physical_high_water_bytes_ =
        std::max(physical_high_water_bytes_, snapshot.physical_retained_bytes);
  }

  void EndMeasurement() {
    if (!core_) {
      throw std::logic_error(
          "ComposedProducerKernel cannot end measurement before Prepare; prepare a workload "
          "and retry.");
    }
    if (release_arrivals_.load(std::memory_order_relaxed) != 0U ||
        core_->Snapshot().topology.retained_records != 0U) {
      lifecycle_error_count_.fetch_add(1, std::memory_order_relaxed);
    }
    StopConsumer();
    core_->ReturnAllCredits();
  }

  [[nodiscard]] RecordFootprint DescribeRecord(std::span<const std::byte> payload) const noexcept {
    return record_storage::DescribeRecord<record_storage::ContiguousPolicy>(payload);
  }

  [[nodiscard]] Attempt TryProduce(std::size_t producer_index,
                                   std::span<const std::byte> payload) noexcept {
    if (!prepared_ || !core_ || producer_index >= producer_count_) {
      lifecycle_error_count_.fetch_add(1, std::memory_order_relaxed);
      return Attempt{AttemptStatus::kRejected, false};
    }

    const ProduceResult produced = core_->TryProduce(
        producer_index, RecordPlan::Benchmark(payload.size()),
        [payload](Writer& writer) noexcept {
          const auto written = writer.Append(payload);
          return written.stored_bytes ==
                 std::min(payload.size(), record_storage::kMaximumBenchmarkStoredMessageBytes);
        },
        [](Writer& writer) noexcept { return AddBenchmarkContext(writer); });
    const AttemptStatus status = produced.status == ProduceStatus::kAccepted
                                     ? AttemptStatus::kAccepted
                                     : AttemptStatus::kRejected;
    return Attempt{status, true};
  }

  void Release(Attempt& attempt) noexcept {
    if (!attempt.active_) {
      return;
    }
    attempt.active_ = false;
    const std::uint64_t completed_before = drain_completed_.load(std::memory_order_acquire);
    const std::size_t arrivals = release_arrivals_.fetch_add(1, std::memory_order_acq_rel) + 1U;
    if (arrivals > producer_count_) {
      lifecycle_error_count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (arrivals == producer_count_) {
      release_arrivals_.store(0, std::memory_order_release);
      drain_requested_.fetch_add(1, std::memory_order_release);
      drain_requested_.notify_one();
    }

    std::uint64_t completed = drain_completed_.load(std::memory_order_acquire);
    while (completed <= completed_before) {
      drain_completed_.wait(completed, std::memory_order_acquire);
      completed = drain_completed_.load(std::memory_order_acquire);
    }
  }

  [[nodiscard]] KernelSnapshot Snapshot() const noexcept {
    if (!core_) {
      return {};
    }
    const auto snapshot = core_->Snapshot();
    return {
        .attempted_records = snapshot.attempted_records,
        .accepted_records = snapshot.accepted_records,
        .rejected_records = snapshot.rejected_records,
        .allocation_count = 0,
        .allocation_failure_count = 0,
        .logical_retained_initial_bytes = logical_initial_bytes_,
        .logical_retained_high_water_bytes =
            std::max(logical_high_water_bytes_, snapshot.logical_retained_bytes),
        .logical_retained_current_bytes = snapshot.logical_retained_bytes,
        .logical_retained_limit_bytes = snapshot.retained_limit_bytes,
        .physical_retained_initial_bytes = physical_initial_bytes_,
        .physical_retained_high_water_bytes =
            std::max(physical_high_water_bytes_, snapshot.physical_retained_bytes),
        .physical_retained_current_bytes = snapshot.physical_retained_bytes,
        .physical_retained_limit_bytes = snapshot.retained_limit_bytes,
    };
  }

  [[nodiscard]] ingress::TopologySnapshot MeasurementTopologySnapshot() const noexcept {
    return core_ ? core_->Snapshot().topology : ingress::TopologySnapshot{};
  }

  [[nodiscard]] std::uint64_t message_callback_count() const noexcept {
    return core_ ? core_->Snapshot().message_callback_count : 0U;
  }
  [[nodiscard]] std::uint64_t context_callback_count() const noexcept {
    return core_ ? core_->Snapshot().context_callback_count : 0U;
  }
  [[nodiscard]] std::uint64_t fifo_error_count() const noexcept {
    return core_ ? core_->Snapshot().fifo_error_count : 0U;
  }
  [[nodiscard]] std::uint64_t record_validation_error_count() const noexcept {
    return core_ ? core_->Snapshot().record_validation_error_count : 0U;
  }
  [[nodiscard]] std::uint64_t publication_error_count() const noexcept {
    return core_ ? core_->Snapshot().publication_error_count : 0U;
  }
  [[nodiscard]] std::uint64_t lifecycle_error_count() const noexcept {
    return lifecycle_error_count_.load(std::memory_order_relaxed);
  }

 private:
  void DrainPublishedRecords() noexcept {
    if (!core_) {
      lifecycle_error_count_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    for (;;) {
      const ingress::ConsumeStatus status =
          core_->TryConsume([](std::uint64_t, const record_storage::RecordView&) noexcept {});
      if (status == ingress::ConsumeStatus::kEmpty) {
        return;
      }
      if (status == ingress::ConsumeStatus::kPending) {
        std::this_thread::yield();
      }
    }
  }

  void ConsumerLoop() noexcept {
    std::uint64_t completed = 0;
    for (;;) {
      std::uint64_t requested = drain_requested_.load(std::memory_order_acquire);
      if (requested == completed) {
        if (stop_requested_.load(std::memory_order_acquire)) {
          return;
        }
        drain_requested_.wait(requested, std::memory_order_acquire);
        continue;
      }

      DrainPublishedRecords();
      completed = requested;
      drain_completed_.store(completed, std::memory_order_release);
      drain_completed_.notify_all();
      if (stop_requested_.load(std::memory_order_acquire)) {
        return;
      }
    }
  }

  void StopConsumer() noexcept {
    if (!consumer_thread_.joinable()) {
      return;
    }
    stop_requested_.store(true, std::memory_order_release);
    drain_requested_.fetch_add(1, std::memory_order_release);
    drain_requested_.notify_one();
    consumer_thread_.join();
  }

  bool prepared_{false};
  std::size_t producer_count_{0};
  std::optional<ComposedProducerPath<64>> core_{};
  std::thread consumer_thread_{};
  std::atomic<std::size_t> release_arrivals_{0};
  std::atomic<std::uint64_t> drain_requested_{0};
  std::atomic<std::uint64_t> drain_completed_{0};
  std::atomic<bool> stop_requested_{false};
  std::atomic<std::uint64_t> lifecycle_error_count_{0};
  std::uint64_t logical_initial_bytes_{0};
  std::uint64_t logical_high_water_bytes_{0};
  std::uint64_t physical_initial_bytes_{0};
  std::uint64_t physical_high_water_bytes_{0};
};

static_assert(WorkloadKernel<ComposedProducerKernel>);

}  // namespace ulog::benchmark_support::composed
