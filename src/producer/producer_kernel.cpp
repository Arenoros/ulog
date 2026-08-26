#include "producer/producer_kernel.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "logger_state.hpp"
#include "producer/credit_ledger.hpp"
#include "producer/producer_lanes.hpp"
#include "producer/record_storage.hpp"

namespace ulog::detail::producer {
namespace {

enum class ProducerSlotState : std::uint8_t { kFree, kActive, kRetiring, kReconciling };

struct ThreadRegistration final {
  ProducerKernel* owner{nullptr};
  std::size_t slot_index{0};
  std::uint64_t generation{0};
};

struct ThreadRegistry final {
  std::array<ThreadRegistration, kMaximumProducerSlots> entries{};
  std::size_t last_hit{kMaximumProducerSlots};
  std::size_t missing_producer_shard{kMaximumProducerSlots};
};

std::atomic<std::size_t> next_missing_producer_shard{0};
thread_local ThreadRegistry thread_registry;

[[nodiscard]] std::size_t MissingProducerShard() noexcept {
  if (thread_registry.missing_producer_shard == kMaximumProducerSlots) {
    thread_registry.missing_producer_shard =
        next_missing_producer_shard.fetch_add(1, std::memory_order_relaxed) % kMaximumProducerSlots;
  }
  return thread_registry.missing_producer_shard;
}

[[nodiscard]] ThreadRegistration* FindThreadRegistration(ProducerKernel& owner) noexcept {
  if (thread_registry.last_hit < thread_registry.entries.size()) {
    auto& cached = thread_registry.entries[thread_registry.last_hit];
    if (cached.owner == &owner) {
      return &cached;
    }
  }
  for (std::size_t index = 0; index < thread_registry.entries.size(); ++index) {
    if (thread_registry.entries[index].owner == &owner) {
      thread_registry.last_hit = index;
      return &thread_registry.entries[index];
    }
  }
  return nullptr;
}

[[nodiscard]] ThreadRegistration* FindFreeThreadRegistration() noexcept {
  for (auto& entry : thread_registry.entries) {
    if (entry.owner == nullptr) {
      return &entry;
    }
  }
  return nullptr;
}

void RemoveThreadRegistration(ProducerKernel& owner, std::size_t slot_index,
                              std::uint64_t generation) noexcept {
  for (std::size_t index = 0; index < thread_registry.entries.size(); ++index) {
    auto& entry = thread_registry.entries[index];
    if (entry.owner == &owner && entry.slot_index == slot_index && entry.generation == generation) {
      entry = {};
      if (thread_registry.last_hit == index) {
        thread_registry.last_hit = kMaximumProducerSlots;
      }
      return;
    }
  }
}

[[noreturn]] void ThrowInvalidConfiguration(std::string_view field, std::string_view detail,
                                            std::string_view correction) {
  throw std::invalid_argument("Invalid producer kernel configuration: " + std::string{field} + " " +
                              std::string{detail} + " Set " + std::string{field} + " " +
                              std::string{correction} + ".");
}

void ValidateConfiguration(const KernelConfig& config, EventClock clock) {
  const auto threshold = static_cast<std::uint8_t>(config.threshold);
  if (threshold > static_cast<std::uint8_t>(Level::kNone)) {
    ThrowInvalidConfiguration("threshold", "is not a valid ulog::Level enumerator;",
                              "to Trace through None");
  }
  constexpr std::size_t kMinimumRecordBytes = 128;
  if (config.maximum_record_bytes < kMinimumRecordBytes ||
      config.maximum_record_bytes > kMaximumRecordBytes ||
      config.maximum_record_bytes % kAccountingQuantumBytes != 0U) {
    ThrowInvalidConfiguration("maximum_record_bytes",
                              "must be a 64-byte multiple from 128 through 16384;",
                              "to a supported aligned Record bound");
  }
  if (config.payload_capacity_bytes < config.maximum_record_bytes ||
      config.payload_capacity_bytes % kAccountingQuantumBytes != 0U) {
    ThrowInvalidConfiguration(
        "payload_capacity_bytes", "must be 64-byte aligned and at least maximum_record_bytes;",
        "to an aligned value large enough for one complete Record reservation");
  }
  if (config.producer_slots == 0U || config.producer_slots > kMaximumProducerSlots) {
    ThrowInvalidConfiguration("producer_slots", "must be between 1 and 32;",
                              "to the number of concurrently active application producers");
  }
  if (config.ingress_cells < config.producer_slots || config.ingress_cells > kMaximumIngressCells) {
    ThrowInvalidConfiguration("ingress_cells", "must be between producer_slots and 64;",
                              "to at least one private lane cell per producer slot");
  }
  if (clock.now == nullptr) {
    ThrowInvalidConfiguration("event_clock", "has no now callback;",
                              "to SystemEventClock() or a valid deterministic clock");
  }
}

EventTimestamp ReadSystemClock(void*) noexcept {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct MessageCall final {
  void* builder_context;
  MessageBuilder build_message;
};

void AppendText(void* context, std::string_view text) {
  auto& appender = *static_cast<RecordAppender*>(context);
  static_cast<void>(appender.Append(text));
}

BuildStatus BuildMessage(void* context, RecordAppender& appender) {
  auto& call = *static_cast<MessageCall*>(context);
  MessageSink sink{&appender, &AppendText};
  return call.build_message(call.builder_context, sink) ? BuildStatus::kComplete
                                                        : BuildStatus::kInvalid;
}

}  // namespace

struct ProducerKernel::Impl final {
  struct alignas(kAccountingQuantumBytes) ProducerControl final {
    std::atomic<std::uint64_t> generation{1};
    std::atomic<ProducerSlotState> state{ProducerSlotState::kFree};
    std::array<std::byte, kAccountingQuantumBytes - sizeof(std::atomic<std::uint64_t>) -
                              sizeof(std::atomic<ProducerSlotState>)>
        padding{};
  };

  struct alignas(kAccountingQuantumBytes) ProducerCounters final {
    std::atomic<std::uint64_t> rejected_lane_full{0};
    std::atomic<std::uint64_t> rejected_budget{0};
    std::atomic<std::uint64_t> abandoned_builds{0};
    std::atomic<std::uint64_t> invalid_records{0};
    std::atomic<std::uint64_t> truncated_records{0};
    std::array<std::byte, kAccountingQuantumBytes - 5U * sizeof(std::atomic<std::uint64_t>)>
        padding{};
  };

  static_assert(sizeof(ProducerCounters) == kAccountingQuantumBytes);

  struct alignas(kAccountingQuantumBytes) MissingProducerCounter final {
    std::atomic<std::uint64_t> rejections{0};
    std::array<std::byte, kAccountingQuantumBytes - sizeof(std::atomic<std::uint64_t>)> padding{};
  };

  static_assert(sizeof(MissingProducerCounter) == kAccountingQuantumBytes);

  Impl(ProducerKernel& owner, KernelConfig kernel_config, EventClock event_clock)
      : lanes(kernel_config.producer_slots, kernel_config.ingress_cells),
        record_slots(std::make_unique<record::RecordSlot[]>(kernel_config.ingress_cells)),
        clock(event_clock),
        logger_state{
            std::atomic<std::uint8_t>{static_cast<std::uint8_t>(kernel_config.threshold)},
            nullptr,
            &owner,
        },
        config(kernel_config) {
    ledger.Reset(
        {.capacity_bytes = config.payload_capacity_bytes, .producer_count = config.producer_slots});
  }

  [[nodiscard]] bool IsActive(std::size_t slot_index, std::uint64_t generation) const noexcept {
    return slot_index < config.producer_slots &&
           controls[slot_index].state.load(std::memory_order_acquire) ==
               ProducerSlotState::kActive &&
           controls[slot_index].generation.load(std::memory_order_relaxed) == generation;
  }

  void CountMissingProducer() noexcept {
    missing_producer_counters[MissingProducerShard()].rejections.fetch_add(
        1, std::memory_order_relaxed);
  }

  void TryFinishRetirement(std::size_t slot_index) noexcept {
    if (slot_index >= config.producer_slots || !lanes.IsProducerDrained(slot_index)) {
      return;
    }
    auto expected = ProducerSlotState::kRetiring;
    if (!controls[slot_index].state.compare_exchange_strong(
            expected, ProducerSlotState::kReconciling, std::memory_order_acq_rel)) {
      return;
    }
    if (!ledger.TryReconcileProducer(slot_index)) {
      controls[slot_index].state.store(ProducerSlotState::kRetiring, std::memory_order_release);
      return;
    }
    controls[slot_index].generation.fetch_add(1, std::memory_order_relaxed);
    controls[slot_index].state.store(ProducerSlotState::kFree, std::memory_order_release);
  }

  std::array<ProducerControl, kMaximumProducerSlots> controls{};
  std::array<ProducerCounters, kMaximumProducerSlots> counters{};
  std::array<MissingProducerCounter, kMaximumProducerSlots> missing_producer_counters{};
  credit::CreditLedger ledger;
  ingress::ProducerLanes lanes;
  std::unique_ptr<record::RecordSlot[]> record_slots;
  std::atomic<std::uint64_t> consumed_records{0};
  std::atomic<std::uint64_t> consumer_validation_errors{0};
  EventClock clock;
  LoggerState logger_state;
  KernelConfig config;
  std::array<std::uint64_t, kMaximumIngressCells> record_generations{};
  std::array<record::StoredRecord, kMaximumIngressCells> records{};
  std::array<std::optional<credit::CreditLedger::Ownership>, kMaximumIngressCells> ownerships{};
};

EventClock SystemEventClock() noexcept { return EventClock{nullptr, &ReadSystemClock}; }

ProducerKernel::ProducerRegistration::ProducerRegistration(ProducerRegistration&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      slot_index_(std::exchange(other.slot_index_, 0)),
      generation_(std::exchange(other.generation_, 0)) {}

ProducerKernel::ProducerRegistration& ProducerKernel::ProducerRegistration::operator=(
    ProducerRegistration&& other) noexcept {
  if (this != &other) {
    Reset();
    owner_ = std::exchange(other.owner_, nullptr);
    slot_index_ = std::exchange(other.slot_index_, 0);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}

ProducerKernel::ProducerRegistration::~ProducerRegistration() { Reset(); }

void ProducerKernel::ProducerRegistration::Reset() noexcept {
  if (owner_ != nullptr) {
    owner_->RetireProducer(*this);
  }
}

ProducerKernel::ProducerKernel(KernelConfig config, EventClock clock) {
  ValidateConfiguration(config, clock);
  impl_ = std::make_unique<Impl>(*this, config, clock);
  static const ProducerOperations operations{&ProducerKernel::LogMessage};
  impl_->logger_state.producer_operations = &operations;
}

ProducerKernel::~ProducerKernel() = default;

Logger ProducerKernel::GetLogger() noexcept {
  return LoggerAccess::FromState(&impl_->logger_state);
}

ProducerKernel::ProducerRegistration ProducerKernel::TryRegisterProducer() noexcept {
  static_cast<void>(MissingProducerShard());
  if (ThreadRegistration* const existing = FindThreadRegistration(*this); existing != nullptr) {
    if (impl_->IsActive(existing->slot_index, existing->generation)) {
      return {};
    }
    RemoveThreadRegistration(*this, existing->slot_index, existing->generation);
  }
  ThreadRegistration* const thread_entry = FindFreeThreadRegistration();
  if (thread_entry == nullptr) {
    return {};
  }

  for (std::size_t slot_index = 0; slot_index < impl_->config.producer_slots; ++slot_index) {
    auto expected = ProducerSlotState::kFree;
    if (!impl_->controls[slot_index].state.compare_exchange_strong(
            expected, ProducerSlotState::kActive, std::memory_order_acq_rel)) {
      continue;
    }
    const std::uint64_t generation =
        impl_->controls[slot_index].generation.load(std::memory_order_relaxed);
    *thread_entry =
        ThreadRegistration{.owner = this, .slot_index = slot_index, .generation = generation};
    thread_registry.last_hit =
        static_cast<std::size_t>(thread_entry - thread_registry.entries.data());
    return ProducerRegistration{*this, slot_index, generation};
  }
  return {};
}

void ProducerKernel::SetLevel(Level threshold) noexcept {
  const auto value = static_cast<std::uint8_t>(threshold);
  const auto none = static_cast<std::uint8_t>(Level::kNone);
  impl_->logger_state.threshold.store(value <= none ? value : none, std::memory_order_relaxed);
}

PublishResult ProducerKernel::TryPublish(ProducerRegistration& producer, Level level,
                                         const SourceLocation& source, BuildOperation operation) {
  const auto threshold =
      static_cast<Level>(impl_->logger_state.threshold.load(std::memory_order_relaxed));
  if (!IsLevelEnabled(level, threshold)) {
    return {.outcome = PublishOutcome::kFiltered};
  }
  if (producer.owner_ != this) {
    impl_->CountMissingProducer();
    return {.outcome = PublishOutcome::kNoProducerSlot};
  }
  return TryPublishSlot(producer.slot_index_, producer.generation_, level, source, operation);
}

PublishResult ProducerKernel::TryPublishSlot(std::size_t slot_index, std::uint64_t generation,
                                             Level level, const SourceLocation& source,
                                             BuildOperation operation) {
  if (!impl_->IsActive(slot_index, generation)) {
    impl_->CountMissingProducer();
    return {.outcome = PublishOutcome::kNoProducerSlot};
  }

  auto& counters = impl_->counters[slot_index];
  auto publication = impl_->lanes.TryClaimPublication(slot_index);
  if (!publication) {
    if (publication.status() == ingress::ClaimStatus::kFull ||
        publication.status() == ingress::ClaimStatus::kContended) {
      counters.rejected_lane_full.fetch_add(1, std::memory_order_relaxed);
      return {.outcome = PublishOutcome::kLaneFull};
    }
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }

  const auto cell_index = publication.cell_index();
  if (!cell_index || *cell_index >= impl_->config.ingress_cells) {
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }
  auto reservation = impl_->ledger.TryReserve(slot_index, impl_->config.maximum_record_bytes);
  if (!reservation) {
    counters.rejected_budget.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kBudgetExhausted};
  }

  const EventTimestamp timestamp = impl_->clock.now(impl_->clock.context);
  auto writer = impl_->record_slots[*cell_index].Begin(impl_->config.maximum_record_bytes, level,
                                                       source, timestamp);
  if (!writer || operation.invoke == nullptr) {
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }

  RecordAppender appender{&writer};
  BuildStatus build_status = BuildStatus::kInvalid;
  try {
    build_status = operation.invoke(operation.context, appender);
  } catch (...) {
    counters.abandoned_builds.fetch_add(1, std::memory_order_relaxed);
    throw;
  }
  if (build_status != BuildStatus::kComplete) {
    counters.abandoned_builds.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }

  const record::StoredRecord record = std::move(writer).Publish();
  if (!record || record.serialized_bytes > impl_->config.maximum_record_bytes ||
      record.accounting_charge_bytes > impl_->config.maximum_record_bytes) {
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }
  auto ownership = std::move(reservation).Commit(record.serialized_bytes);
  if (!ownership || ownership.retained_bytes() != record.serialized_bytes ||
      ownership.charge_bytes() != record.accounting_charge_bytes) {
    impl_->record_slots[*cell_index].Reset();
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }

  const std::uint64_t record_generation = ++impl_->record_generations[*cell_index];
  impl_->records[*cell_index] = record;
  impl_->ownerships[*cell_index].emplace(std::move(ownership));
  const auto sequence = impl_->lanes.Publish(
      std::move(publication), ingress::RecordHandle{
                                  .slot_index = static_cast<std::uint32_t>(*cell_index),
                                  .producer_index = static_cast<std::uint32_t>(slot_index),
                                  .generation = record_generation,
                                  .serialized_bytes = record.serialized_bytes,
                                  .accounting_charge_bytes = record.accounting_charge_bytes,
                              });
  if (!sequence) {
    impl_->ownerships[*cell_index].reset();
    impl_->records[*cell_index] = {};
    impl_->record_slots[*cell_index].Reset();
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }

  if (record.truncated) {
    counters.truncated_records.fetch_add(1, std::memory_order_relaxed);
  }
  return {.outcome = PublishOutcome::kAccepted,
          .admission_sequence = sequence,
          .truncated = record.truncated};
}

ConsumeStatus ProducerKernel::TryConsume(void* context, RecordConsumer consumer) noexcept {
  auto consumption = impl_->lanes.TryClaimConsumption();
  if (!consumption) {
    return consumption.status() == ingress::ConsumptionStatus::kPending ? ConsumeStatus::kPending
                                                                        : ConsumeStatus::kEmpty;
  }

  const auto envelope = consumption.envelope();
  const std::size_t cell_index = envelope.record.slot_index;
  const std::size_t producer_index = consumption.producer_index();
  bool valid = consumer != nullptr && cell_index < impl_->config.ingress_cells;
  if (valid) {
    const auto& stored = impl_->records[cell_index];
    const auto& ownership = impl_->ownerships[cell_index];
    valid = stored && ownership &&
            envelope.record.generation == impl_->record_generations[cell_index] &&
            envelope.record.producer_index == producer_index &&
            envelope.record.serialized_bytes == stored.serialized_bytes &&
            envelope.record.accounting_charge_bytes == stored.accounting_charge_bytes &&
            ownership->retained_bytes() == stored.serialized_bytes &&
            ownership->charge_bytes() == stored.accounting_charge_bytes;
    if (valid) {
      const RecordView view{stored.storage, stored.serialized_bytes,
                            stored.accounting_charge_bytes};
      consumer(context, envelope.admission_sequence, view);
    }
  }
  if (!valid) {
    impl_->consumer_validation_errors.fetch_add(1, std::memory_order_relaxed);
  }

  if (cell_index < impl_->config.ingress_cells) {
    impl_->ownerships[cell_index].reset();
    impl_->records[cell_index] = {};
    impl_->record_slots[cell_index].Reset();
  }
  impl_->consumed_records.fetch_add(1, std::memory_order_relaxed);
  consumption.Acknowledge();
  impl_->TryFinishRetirement(producer_index);
  return ConsumeStatus::kRecord;
}

KernelSnapshot ProducerKernel::GetSnapshot() const noexcept {
  KernelSnapshot snapshot{
      .accepted_records = impl_->lanes.AcceptedCount(),
      .consumed_records = impl_->consumed_records.load(std::memory_order_relaxed),
      .payload_capacity_bytes = impl_->config.payload_capacity_bytes,
      .fixed_backing_bytes = impl_->config.ingress_cells * sizeof(record::RecordSlot),
  };
  for (const auto& counter : impl_->missing_producer_counters) {
    snapshot.rejected_no_producer += counter.rejections.load(std::memory_order_relaxed);
  }
  std::uint64_t producer_invalid_records = 0;
  for (std::size_t index = 0; index < impl_->config.producer_slots; ++index) {
    const auto& counters = impl_->counters[index];
    snapshot.rejected_lane_full += counters.rejected_lane_full.load(std::memory_order_relaxed);
    snapshot.rejected_budget += counters.rejected_budget.load(std::memory_order_relaxed);
    snapshot.abandoned_builds += counters.abandoned_builds.load(std::memory_order_relaxed);
    producer_invalid_records += counters.invalid_records.load(std::memory_order_relaxed);
    snapshot.truncated_records += counters.truncated_records.load(std::memory_order_relaxed);
    switch (impl_->controls[index].state.load(std::memory_order_relaxed)) {
      case ProducerSlotState::kActive:
        ++snapshot.active_producer_slots;
        break;
      case ProducerSlotState::kRetiring:
      case ProducerSlotState::kReconciling:
        ++snapshot.retiring_producer_slots;
        break;
      case ProducerSlotState::kFree:
        break;
    }
  }
  snapshot.attempted_records = snapshot.accepted_records + snapshot.rejected_no_producer +
                               snapshot.rejected_lane_full + snapshot.rejected_budget +
                               snapshot.abandoned_builds + producer_invalid_records;
  snapshot.invalid_records =
      producer_invalid_records + impl_->consumer_validation_errors.load(std::memory_order_relaxed);
  snapshot.retained_records = snapshot.accepted_records >= snapshot.consumed_records
                                  ? snapshot.accepted_records - snapshot.consumed_records
                                  : 0;
  const auto ledger = impl_->ledger.GetSnapshot();
  snapshot.logical_retained_bytes = ledger.logical_retained_bytes;
  snapshot.physical_retained_bytes = ledger.physical_retained_bytes;
  snapshot.accounting_sample_consistent = ledger.sample_consistent;
  return snapshot;
}

void ProducerKernel::LogMessage(void* context, Level level, const SourceLocation& source,
                                void* builder_context, MessageBuilder build_message) {
  auto& kernel = *static_cast<ProducerKernel*>(context);
  ThreadRegistration* const registration = FindThreadRegistration(kernel);
  if (registration == nullptr) {
    kernel.impl_->CountMissingProducer();
    return;
  }
  if (!kernel.impl_->IsActive(registration->slot_index, registration->generation)) {
    const auto slot_index = registration->slot_index;
    const auto generation = registration->generation;
    RemoveThreadRegistration(kernel, slot_index, generation);
    kernel.impl_->CountMissingProducer();
    return;
  }
  MessageCall call{builder_context, build_message};
  static_cast<void>(kernel.TryPublishSlot(registration->slot_index, registration->generation, level,
                                          source, BuildOperation{&call, &BuildMessage}));
}

void ProducerKernel::RetireProducer(ProducerRegistration& producer) noexcept {
  RemoveThreadRegistration(*this, producer.slot_index_, producer.generation_);
  if (producer.slot_index_ < impl_->config.producer_slots &&
      impl_->controls[producer.slot_index_].generation.load(std::memory_order_relaxed) ==
          producer.generation_) {
    auto expected = ProducerSlotState::kActive;
    if (impl_->controls[producer.slot_index_].state.compare_exchange_strong(
            expected, ProducerSlotState::kRetiring, std::memory_order_acq_rel)) {
      impl_->TryFinishRetirement(producer.slot_index_);
    }
  }
  producer.owner_ = nullptr;
  producer.slot_index_ = 0;
  producer.generation_ = 0;
}

}  // namespace ulog::detail::producer
