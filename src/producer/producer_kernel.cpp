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

struct RegistrationLiveness final {
  std::atomic<std::uint64_t> registered_generation{0};
};

struct KernelLifetimeState final {
  std::atomic<std::size_t> references{1};
  std::atomic<bool> admission_open{true};
  std::array<RegistrationLiveness, kMaximumProducerSlots> registrations{};
};

class KernelLifetimeRef final {
 public:
  KernelLifetimeRef() noexcept = default;

  [[nodiscard]] static KernelLifetimeRef Create() {
    return KernelLifetimeRef{new KernelLifetimeState};
  }

  KernelLifetimeRef(const KernelLifetimeRef& other) noexcept : state_(other.state_) { Retain(); }
  KernelLifetimeRef& operator=(const KernelLifetimeRef& other) noexcept {
    if (this != &other) {
      Reset();
      state_ = other.state_;
      Retain();
    }
    return *this;
  }
  KernelLifetimeRef(KernelLifetimeRef&& other) noexcept
      : state_(std::exchange(other.state_, nullptr)) {}
  KernelLifetimeRef& operator=(KernelLifetimeRef&& other) noexcept {
    if (this != &other) {
      Reset();
      state_ = std::exchange(other.state_, nullptr);
    }
    return *this;
  }
  ~KernelLifetimeRef() { Reset(); }

  [[nodiscard]] bool IsAdmissionOpen() const noexcept {
    return state_ != nullptr && state_->admission_open.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool IsRegistrationCurrent(std::size_t slot_index,
                                           std::uint64_t generation) const noexcept {
    return IsAdmissionOpen() && slot_index < state_->registrations.size() &&
           state_->registrations[slot_index].registered_generation.load(
               std::memory_order_acquire) == generation;
  }

  void ActivateRegistration(std::size_t slot_index, std::uint64_t generation) noexcept {
    if (state_ != nullptr && slot_index < state_->registrations.size()) {
      state_->registrations[slot_index].registered_generation.store(generation,
                                                                    std::memory_order_release);
    }
  }

  void RetireRegistration(std::size_t slot_index, std::uint64_t generation) noexcept {
    if (state_ == nullptr || slot_index >= state_->registrations.size()) {
      return;
    }
    state_->registrations[slot_index].registered_generation.compare_exchange_strong(
        generation, 0, std::memory_order_acq_rel);
  }

  void CloseAdmission() noexcept {
    if (state_ != nullptr) {
      state_->admission_open.store(false, std::memory_order_release);
    }
  }

 private:
  explicit KernelLifetimeRef(KernelLifetimeState* state) noexcept : state_(state) {}

  void Retain() noexcept {
    if (state_ != nullptr) {
      state_->references.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void Reset() noexcept {
    KernelLifetimeState* const state = std::exchange(state_, nullptr);
    if (state != nullptr && state->references.fetch_sub(1, std::memory_order_acq_rel) == 1U) {
      delete state;
    }
  }

  KernelLifetimeState* state_{nullptr};
};

struct ThreadRegistration final {
  ProducerKernel* owner{nullptr};
  KernelLifetimeRef lifetime{};
  std::uint64_t kernel_identity{0};
  std::size_t slot_index{0};
  std::uint64_t generation{0};
};

struct ThreadRegistry final {
  std::array<ThreadRegistration, kMaximumProducerSlots> entries{};
  std::size_t last_hit{kMaximumProducerSlots};
  std::size_t missing_producer_shard{kMaximumProducerSlots};
};

std::atomic<std::size_t> next_missing_producer_shard{0};
std::atomic<std::uint64_t> next_kernel_identity{1};
thread_local ThreadRegistry thread_registry;

[[nodiscard]] std::size_t MissingProducerShard() noexcept {
  if (thread_registry.missing_producer_shard == kMaximumProducerSlots) {
    thread_registry.missing_producer_shard =
        next_missing_producer_shard.fetch_add(1, std::memory_order_relaxed) % kMaximumProducerSlots;
  }
  return thread_registry.missing_producer_shard;
}

[[nodiscard]] ThreadRegistration* FindThreadRegistration(ProducerKernel& owner,
                                                         std::uint64_t kernel_identity) noexcept {
  if (thread_registry.last_hit < thread_registry.entries.size()) {
    auto& cached = thread_registry.entries[thread_registry.last_hit];
    if (cached.owner == &owner && cached.kernel_identity == kernel_identity) {
      return &cached;
    }
  }
  for (std::size_t index = 0; index < thread_registry.entries.size(); ++index) {
    if (thread_registry.entries[index].owner == &owner &&
        thread_registry.entries[index].kernel_identity == kernel_identity) {
      thread_registry.last_hit = index;
      return &thread_registry.entries[index];
    }
  }
  return nullptr;
}

[[nodiscard]] ThreadRegistration* FindFreeThreadRegistration() noexcept {
  for (std::size_t index = 0; index < thread_registry.entries.size(); ++index) {
    auto& entry = thread_registry.entries[index];
    if (entry.owner == nullptr) {
      return &entry;
    }
    if (!entry.lifetime.IsRegistrationCurrent(entry.slot_index, entry.generation)) {
      entry = {};
      if (thread_registry.last_hit == index) {
        thread_registry.last_hit = kMaximumProducerSlots;
      }
      return &entry;
    }
  }
  return nullptr;
}

void RemoveThreadRegistration(ProducerKernel& owner, std::uint64_t kernel_identity,
                              std::size_t slot_index, std::uint64_t generation) noexcept {
  for (std::size_t index = 0; index < thread_registry.entries.size(); ++index) {
    auto& entry = thread_registry.entries[index];
    if (entry.owner == &owner && entry.kernel_identity == kernel_identity &&
        entry.slot_index == slot_index && entry.generation == generation) {
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

LevelChangeStatus ChangeLoggerLevel(void* context, Level threshold) noexcept {
  return static_cast<ProducerKernel*>(context)->ChangeLevel(threshold);
}

struct MessageCall final {
  void* builder_context;
  MessageBuilder build_message;
};

struct NotifyConsumerOnExit final {
  ConsumerNotification notification;

  ~NotifyConsumerOnExit() { notification.Notify(); }
};

void AppendText(void* context, std::string_view text) {
  auto& appender = *static_cast<RecordAppender*>(context);
  static_cast<void>(appender.Append(text));
}

FieldStatus PutField(void* context, std::string_view key, const FieldValue& value,
                     FieldMode mode) noexcept {
  return static_cast<RecordAppender*>(context)->PutField(key, value, mode);
}

BuildStatus BuildMessage(void* context, RecordAppender& appender) {
  auto& call = *static_cast<MessageCall*>(context);
  MessageSink sink{&appender, &AppendText, &PutField};
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

  /// The Record a producer slot is writing between BeginSlot and FinishSlot. A slot holds at most
  /// one, because its publication claim is exclusive until it is published or released.
  struct OpenRecordState final {
    ProducerKernel* owner;
    std::size_t slot_index;
    ingress::ProducerLanes::PublicationClaim publication;
    std::size_t cell_index;
    credit::CreditLedger::Reservation reservation;
    record::RecordWriter writer;
  };

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
  KernelLifetimeRef lifetime{KernelLifetimeRef::Create()};
  std::unique_ptr<record::RecordSlot[]> record_slots;
  std::atomic<std::uint64_t> consumed_records{0};
  std::atomic<std::uint64_t> consumer_validation_errors{0};
  EventClock clock;
  LoggerState logger_state;
  KernelConfig config;
  std::array<std::uint64_t, kMaximumIngressCells> record_generations{};
  std::array<record::StoredRecord, kMaximumIngressCells> records{};
  std::array<std::optional<credit::CreditLedger::Ownership>, kMaximumIngressCells> ownerships{};
  std::array<std::optional<OpenRecordState>, kMaximumProducerSlots> open_records{};
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

ProducerKernel::ProducerKernel(KernelConfig config, EventClock clock)
    : identity_(next_kernel_identity.fetch_add(1, std::memory_order_relaxed)) {
  ValidateConfiguration(config, clock);
  impl_ = std::make_unique<Impl>(*this, config, clock);
  static const ProducerOperations operations{&ProducerKernel::LogMessage, &ChangeLoggerLevel,
                                             &ProducerKernel::BeginRecord};
  impl_->logger_state.producer_operations = &operations;
}

ProducerKernel::~ProducerKernel() { impl_->lifetime.CloseAdmission(); }

Logger ProducerKernel::GetLogger() noexcept {
  return LoggerAccess::FromState(&impl_->logger_state);
}

ProducerKernel::ProducerRegistration ProducerKernel::TryRegisterProducer() noexcept {
  static_cast<void>(MissingProducerShard());
  if (!impl_->lifetime.IsAdmissionOpen()) {
    return {};
  }
  if (ThreadRegistration* const existing = FindThreadRegistration(*this, identity_);
      existing != nullptr) {
    if (impl_->IsActive(existing->slot_index, existing->generation)) {
      return {};
    }
    RemoveThreadRegistration(*this, identity_, existing->slot_index, existing->generation);
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
    if (!impl_->lifetime.IsAdmissionOpen()) {
      impl_->controls[slot_index].state.store(ProducerSlotState::kRetiring,
                                              std::memory_order_release);
      impl_->TryFinishRetirement(slot_index);
      return {};
    }
    const std::uint64_t generation =
        impl_->controls[slot_index].generation.load(std::memory_order_relaxed);
    impl_->lifetime.ActivateRegistration(slot_index, generation);
    *thread_entry = ThreadRegistration{.owner = this,
                                       .lifetime = impl_->lifetime,
                                       .kernel_identity = identity_,
                                       .slot_index = slot_index,
                                       .generation = generation};
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

LevelChangeStatus ProducerKernel::ChangeLevel(Level threshold) noexcept {
  // Pairs with the fence in CloseAdmission: either this check sees the closed flag and resets the
  // threshold, or CloseAdmission's kNone store follows this store. A closed kernel ends at kNone.
  SetLevel(threshold);
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!impl_->lifetime.IsAdmissionOpen()) {
    SetLevel(Level::kNone);
    return LevelChangeStatus::kClosed;
  }
  return LevelChangeStatus::kChanged;
}

void ProducerKernel::CloseAdmission() noexcept {
  impl_->lifetime.CloseAdmission();
  std::atomic_thread_fence(std::memory_order_seq_cst);
  SetLevel(Level::kNone);
  impl_->config.consumer_notification.Notify();
}

bool ProducerKernel::IsAdmissionOpen() const noexcept { return impl_->lifetime.IsAdmissionOpen(); }

bool ProducerKernel::IsQuiescent() noexcept {
  bool quiescent = true;
  for (std::size_t slot_index = 0; slot_index < impl_->config.producer_slots; ++slot_index) {
    impl_->TryFinishRetirement(slot_index);
    quiescent = quiescent && impl_->lanes.IsProducerDrained(slot_index) &&
                impl_->controls[slot_index].state.load(std::memory_order_acquire) ==
                    ProducerSlotState::kFree;
  }
  return quiescent;
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
  const PublishOutcome begun = BeginSlot(slot_index, generation, level, source);
  if (begun != PublishOutcome::kAccepted) {
    return {.outcome = begun};
  }
  auto& open = impl_->open_records[slot_index];
  if (!open.has_value()) {
    return {.outcome = PublishOutcome::kInvalidRecord};
  }
  RecordAppender appender{&open->writer};
  BuildStatus build_status = BuildStatus::kInvalid;
  if (operation.invoke != nullptr) {
    try {
      build_status = operation.invoke(operation.context, appender);
    } catch (...) {
      static_cast<void>(FinishSlot(slot_index, BuildStatus::kInvalid));
      throw;
    }
  }
  return FinishSlot(slot_index, build_status);
}

PublishOutcome ProducerKernel::BeginSlot(std::size_t slot_index, std::uint64_t generation,
                                         Level level, const SourceLocation& source) noexcept {
  if (!impl_->IsActive(slot_index, generation)) {
    impl_->CountMissingProducer();
    return PublishOutcome::kNoProducerSlot;
  }

  auto& counters = impl_->counters[slot_index];
  auto publication = impl_->lanes.TryClaimPublication(slot_index);
  if (!publication) {
    if (publication.status() == ingress::ClaimStatus::kFull ||
        publication.status() == ingress::ClaimStatus::kContended) {
      counters.rejected_lane_full.fetch_add(1, std::memory_order_relaxed);
      return PublishOutcome::kLaneFull;
    }
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return PublishOutcome::kInvalidRecord;
  }
  // A released claim may let the consumer make progress, so every failure below notifies it.
  NotifyConsumerOnExit notify_consumer{impl_->config.consumer_notification};
  if (!impl_->lifetime.IsAdmissionOpen() || !impl_->IsActive(slot_index, generation)) {
    impl_->CountMissingProducer();
    return PublishOutcome::kNoProducerSlot;
  }

  const auto cell_index = publication.cell_index();
  if (!cell_index || *cell_index >= impl_->config.ingress_cells) {
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return PublishOutcome::kInvalidRecord;
  }
  auto reservation = impl_->ledger.TryReserve(slot_index, impl_->config.maximum_record_bytes);
  if (!reservation) {
    counters.rejected_budget.fetch_add(1, std::memory_order_relaxed);
    return PublishOutcome::kBudgetExhausted;
  }

  const EventTimestamp timestamp = impl_->clock.now(impl_->clock.context);
  auto writer = impl_->record_slots[*cell_index].Begin(impl_->config.maximum_record_bytes, level,
                                                       source, timestamp);
  if (!writer) {
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return PublishOutcome::kInvalidRecord;
  }

  impl_->open_records[slot_index].emplace(Impl::OpenRecordState{
      .owner = this,
      .slot_index = slot_index,
      .publication = std::move(publication),
      .cell_index = *cell_index,
      .reservation = std::move(reservation),
      .writer = std::move(writer),
  });
  notify_consumer.notification = {};
  return PublishOutcome::kAccepted;
}

PublishResult ProducerKernel::FinishSlot(std::size_t slot_index,
                                         BuildStatus build_status) noexcept {
  auto& open = impl_->open_records[slot_index];
  if (!open.has_value()) {
    return {.outcome = PublishOutcome::kInvalidRecord};
  }
  const NotifyConsumerOnExit notify_consumer{impl_->config.consumer_notification};
  // Releases whatever the Record still holds once it is published or abandoned.
  struct CloseOnExit final {
    std::optional<Impl::OpenRecordState>& open;
    ~CloseOnExit() { open.reset(); }
  } const close_on_exit{open};

  auto& counters = impl_->counters[slot_index];
  if (build_status != BuildStatus::kComplete) {
    counters.abandoned_builds.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }

  const std::size_t cell_index = open->cell_index;
  const record::StoredRecord record = std::move(open->writer).Publish();
  if (!record || record.serialized_bytes > impl_->config.maximum_record_bytes ||
      record.accounting_charge_bytes > impl_->config.maximum_record_bytes) {
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }
  auto ownership = std::move(open->reservation).Commit(record.serialized_bytes);
  if (!ownership || ownership.retained_bytes() != record.serialized_bytes ||
      ownership.charge_bytes() != record.accounting_charge_bytes) {
    impl_->record_slots[cell_index].Reset();
    counters.invalid_records.fetch_add(1, std::memory_order_relaxed);
    return {.outcome = PublishOutcome::kInvalidRecord};
  }

  const std::uint64_t record_generation = ++impl_->record_generations[cell_index];
  impl_->records[cell_index] = record;
  impl_->ownerships[cell_index].emplace(std::move(ownership));
  const auto sequence = impl_->lanes.Publish(
      std::move(open->publication), ingress::RecordHandle{
                                        .slot_index = static_cast<std::uint32_t>(cell_index),
                                        .producer_index = static_cast<std::uint32_t>(slot_index),
                                        .generation = record_generation,
                                        .serialized_bytes = record.serialized_bytes,
                                        .accounting_charge_bytes = record.accounting_charge_bytes,
                                    });
  if (!sequence) {
    impl_->ownerships[cell_index].reset();
    impl_->records[cell_index] = {};
    impl_->record_slots[cell_index].Reset();
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
  ThreadRegistration* const registration = FindThreadRegistration(kernel, kernel.identity_);
  if (registration == nullptr) {
    kernel.impl_->CountMissingProducer();
    return;
  }
  if (!kernel.impl_->IsActive(registration->slot_index, registration->generation)) {
    const auto slot_index = registration->slot_index;
    const auto generation = registration->generation;
    RemoveThreadRegistration(kernel, kernel.identity_, slot_index, generation);
    kernel.impl_->CountMissingProducer();
    return;
  }
  MessageCall call{builder_context, build_message};
  static_cast<void>(kernel.TryPublishSlot(registration->slot_index, registration->generation, level,
                                          source, BuildOperation{&call, &BuildMessage}));
}

bool ProducerKernel::BeginRecord(void* context, Level level, const SourceLocation& source,
                                 OpenRecord& record) noexcept {
  auto& kernel = *static_cast<ProducerKernel*>(context);
  ThreadRegistration* const registration = FindThreadRegistration(kernel, kernel.identity_);
  if (registration == nullptr) {
    kernel.impl_->CountMissingProducer();
    return false;
  }
  if (!kernel.impl_->IsActive(registration->slot_index, registration->generation)) {
    const auto slot_index = registration->slot_index;
    const auto generation = registration->generation;
    RemoveThreadRegistration(kernel, kernel.identity_, slot_index, generation);
    kernel.impl_->CountMissingProducer();
    return false;
  }
  const std::size_t slot_index = registration->slot_index;
  if (kernel.BeginSlot(slot_index, registration->generation, level, source) !=
      PublishOutcome::kAccepted) {
    return false;
  }
  auto& open = kernel.impl_->open_records[slot_index];
  if (!open.has_value()) {
    return false;
  }
  record = OpenRecord{
      .context = &*open,
      .append_text = &ProducerKernel::AppendOpenText,
      .put_field = &ProducerKernel::PutOpenField,
      .finish = &ProducerKernel::FinishOpenRecord,
  };
  return true;
}

bool ProducerKernel::AppendOpenText(void* context, std::string_view text) noexcept {
  RecordAppender appender{&static_cast<Impl::OpenRecordState*>(context)->writer};
  return !appender.Append(text).truncated;
}

FieldStatus ProducerKernel::PutOpenField(void* context, std::string_view key,
                                         const FieldValue& value, FieldMode mode) noexcept {
  RecordAppender appender{&static_cast<Impl::OpenRecordState*>(context)->writer};
  return appender.PutField(key, value, mode);
}

void ProducerKernel::FinishOpenRecord(void* context, bool publish) noexcept {
  const auto& state = *static_cast<Impl::OpenRecordState*>(context);
  static_cast<void>(state.owner->FinishSlot(
      state.slot_index, publish ? BuildStatus::kComplete : BuildStatus::kInvalid));
}

void ProducerKernel::RetireProducer(ProducerRegistration& producer) noexcept {
  RemoveThreadRegistration(*this, identity_, producer.slot_index_, producer.generation_);
  impl_->lifetime.RetireRegistration(producer.slot_index_, producer.generation_);
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
