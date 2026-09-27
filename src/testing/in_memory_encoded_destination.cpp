#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "encoding/raw_encoder.hpp"
#include "testing/in_memory_encoded_destination_access.hpp"

namespace ulog::detail::testing {
namespace {

/// Application-visible ownership of one slot's frame backing.
enum class EncodedSlotState : std::uint8_t {
  kFree,
  kReserved,
  kPending,
  kDelivering,
  kReady,
  kHeld
};

/// Runtime-visible delivery state. A slot is reusable only after the worker retires it.
enum class EncodedDeliveryState : std::uint8_t {
  kRetired,
  kInFlight,
  kDelivered,
  kFailed,
  kCancelled,
};

struct EncodedSlotMetadata final {
  EncodedSlotState state{EncodedSlotState::kFree};
  EncodedDeliveryState delivery{EncodedDeliveryState::kRetired};
  std::uint64_t generation{0};
  std::uint64_t admission_sequence{0};
  std::size_t encoded_bytes{0};
};

[[noreturn]] void ThrowInvalidConfiguration(std::string_view detail, std::string_view correction) {
  throw std::invalid_argument("Invalid in-memory encoded destination configuration: " +
                              std::string{detail} + " Set " + std::string{correction} + ".");
}

[[nodiscard]] std::size_t ValidateAndGetFixedBackingBytes(
    const ulog::testing::InMemoryEncodedDestinationConfig& config) {
  if (config.capacity_records == 0U) {
    ThrowInvalidConfiguration("capacity_records must be greater than zero;",
                              "capacity_records to at least 1");
  }
  constexpr std::size_t kMinimumRecordBytes = 128;
  if (config.maximum_record_bytes < kMinimumRecordBytes ||
      config.maximum_record_bytes > producer::kMaximumRecordBytes ||
      config.maximum_record_bytes % producer::kAccountingQuantumBytes != 0U) {
    ThrowInvalidConfiguration(
        "maximum_record_bytes must be a 64-byte multiple from 128 through 16384;",
        "maximum_record_bytes to the same supported bound used by Runtime");
  }
  const std::size_t maximum_encoded_record_bytes =
      encoding::MaximumRawEncodedBytes(config.maximum_record_bytes);
  const std::size_t maximum_size = std::numeric_limits<std::size_t>::max();
  if (maximum_encoded_record_bytes == 0U ||
      config.capacity_records > maximum_size / maximum_encoded_record_bytes ||
      config.capacity_records > maximum_size / sizeof(EncodedSlotMetadata)) {
    ThrowInvalidConfiguration("the requested fixed backing size overflows size_t;",
                              "capacity_records or maximum_record_bytes to a smaller value");
  }
  const std::size_t frame_backing_bytes = config.capacity_records * maximum_encoded_record_bytes;
  const std::size_t metadata_backing_bytes = config.capacity_records * sizeof(EncodedSlotMetadata);
  if (metadata_backing_bytes > maximum_size - frame_backing_bytes) {
    ThrowInvalidConfiguration("the requested fixed backing size overflows size_t;",
                              "capacity_records or maximum_record_bytes to a smaller value");
  }
  return frame_backing_bytes + metadata_backing_bytes;
}

[[nodiscard]] std::uint64_t NextGeneration(std::uint64_t generation) noexcept {
  ++generation;
  return generation == 0U ? 1U : generation;
}

[[nodiscard]] constexpr bool IsKnownDeliveryMode(ulog::testing::EncodedDeliveryMode mode) noexcept {
  switch (mode) {
    case ulog::testing::EncodedDeliveryMode::kCompleteInline:
    case ulog::testing::EncodedDeliveryMode::kHold:
    case ulog::testing::EncodedDeliveryMode::kFailInline:
      return true;
  }
  return false;
}

}  // namespace

struct InMemoryEncodedDestinationState final {
  InMemoryEncodedDestinationState(const ulog::testing::InMemoryEncodedDestinationConfig& config,
                                  std::size_t validated_fixed_backing_bytes)
      : backing(std::make_unique<char[]>(
            config.capacity_records *
            encoding::MaximumRawEncodedBytes(config.maximum_record_bytes))),
        slots(std::make_unique<EncodedSlotMetadata[]>(config.capacity_records)),
        capacity_records(config.capacity_records),
        maximum_record_bytes(config.maximum_record_bytes),
        maximum_encoded_record_bytes(encoding::MaximumRawEncodedBytes(config.maximum_record_bytes)),
        fixed_backing_bytes(validated_fixed_backing_bytes),
        paused(config.start_paused) {}

  [[nodiscard]] char* SlotBacking(std::size_t slot_index) noexcept {
    return backing.get() + slot_index * maximum_encoded_record_bytes;
  }
  [[nodiscard]] const char* SlotBacking(std::size_t slot_index) const noexcept {
    return backing.get() + slot_index * maximum_encoded_record_bytes;
  }
  [[nodiscard]] std::optional<std::size_t> FindClaimableSlot() const noexcept {
    for (std::size_t index = 0; index < capacity_records; ++index) {
      if (slots[index].state == EncodedSlotState::kFree &&
          slots[index].delivery == EncodedDeliveryState::kRetired) {
        return index;
      }
    }
    return std::nullopt;
  }
  [[nodiscard]] std::optional<std::size_t> FindLowestSequence(
      EncodedSlotState state) const noexcept {
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < capacity_records; ++index) {
      const auto& slot = slots[index];
      if (slot.state != state) {
        continue;
      }
      if (!selected || slot.admission_sequence < slots[*selected].admission_sequence) {
        selected = index;
      }
    }
    return selected;
  }
  void WakeRuntimeLocked() const noexcept {
    if (wake.notify != nullptr) {
      wake.notify(wake.context);
    }
  }

  std::mutex mutex;
  std::unique_ptr<char[]> backing;
  std::unique_ptr<EncodedSlotMetadata[]> slots;
  const std::size_t capacity_records;
  const std::size_t maximum_record_bytes;
  const std::size_t maximum_encoded_record_bytes;
  const std::size_t fixed_backing_bytes;
  EncodedDestinationWake wake{};
  ulog::testing::EncodedDeliveryMode delivery_mode{
      ulog::testing::EncodedDeliveryMode::kCompleteInline};
  bool runtime_attached{false};
  bool paused{false};
  bool stopped{false};
};

namespace {

[[nodiscard]] std::shared_ptr<InMemoryEncodedDestinationState> MakeState(
    const ulog::testing::InMemoryEncodedDestinationConfig& config) {
  const std::size_t fixed_backing_bytes = ValidateAndGetFixedBackingBytes(config);
  return std::make_shared<InMemoryEncodedDestinationState>(config, fixed_backing_bytes);
}

void ReleaseSlotHandle(std::shared_ptr<InMemoryEncodedDestinationState>& owner,
                       EncodedDestinationSlotIdentity& identity,
                       EncodedSlotState expected_state) noexcept {
  if (owner == nullptr) {
    return;
  }
  auto state = std::move(owner);
  {
    std::lock_guard lock{state->mutex};
    if (identity.index < state->capacity_records) {
      auto& slot = state->slots[identity.index];
      if (slot.state == expected_state && slot.generation == identity.generation) {
        // Keep encoded_bytes: an unretired delivery still reports them to the worker.
        slot.state = EncodedSlotState::kFree;
        state->WakeRuntimeLocked();
      }
    }
  }
  identity = {};
}

[[nodiscard]] const EncodedSlotMetadata* FindHeldSlot(
    const std::shared_ptr<InMemoryEncodedDestinationState>& state,
    EncodedDestinationSlotIdentity identity) noexcept {
  if (state == nullptr || identity.index >= state->capacity_records) {
    return nullptr;
  }
  const auto& slot = state->slots[identity.index];
  return slot.state == EncodedSlotState::kHeld && slot.generation == identity.generation ? &slot
                                                                                         : nullptr;
}

}  // namespace

EncodedDestinationWriteClaim::EncodedDestinationWriteClaim(
    std::shared_ptr<InMemoryEncodedDestinationState> state,
    EncodedDestinationSlotIdentity identity) noexcept
    : state_(std::move(state)), identity_(identity) {}

EncodedDestinationWriteClaim::EncodedDestinationWriteClaim(
    EncodedDestinationWriteClaim&& other) noexcept
    : state_(std::move(other.state_)), identity_(std::exchange(other.identity_, {})) {}

EncodedDestinationWriteClaim& EncodedDestinationWriteClaim::operator=(
    EncodedDestinationWriteClaim&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    identity_ = std::exchange(other.identity_, {});
  }
  return *this;
}

EncodedDestinationWriteClaim::~EncodedDestinationWriteClaim() { Reset(); }

void EncodedDestinationWriteClaim::Reset() noexcept {
  ReleaseSlotHandle(state_, identity_, EncodedSlotState::kReserved);
}

EncodedDestinationStoreResult EncodedDestinationWriteClaim::StoreRaw(
    std::uint64_t admission_sequence, const producer::RecordView& record) noexcept {
  if (state_ == nullptr || identity_.index >= state_->capacity_records) {
    return {};
  }

  auto& slot = state_->slots[identity_.index];
  const auto encoded = encoding::EncodeRawRecord(
      record,
      std::span<char>{state_->SlotBacking(identity_.index), state_->maximum_encoded_record_bytes});
  if (!encoded.complete) {
    Reset();
    return {};
  }

  bool submitted = false;
  {
    std::lock_guard lock{state_->mutex};
    if (slot.state == EncodedSlotState::kReserved && slot.generation == identity_.generation) {
      slot.admission_sequence = admission_sequence;
      slot.encoded_bytes = encoded.encoded_bytes;
      if (state_->stopped) {
        slot.state = EncodedSlotState::kFree;
        slot.delivery = EncodedDeliveryState::kCancelled;
      } else {
        switch (state_->delivery_mode) {
          case ulog::testing::EncodedDeliveryMode::kCompleteInline:
            slot.state = EncodedSlotState::kReady;
            slot.delivery = EncodedDeliveryState::kDelivered;
            break;
          case ulog::testing::EncodedDeliveryMode::kHold:
            slot.state = EncodedSlotState::kPending;
            slot.delivery = EncodedDeliveryState::kInFlight;
            break;
          case ulog::testing::EncodedDeliveryMode::kFailInline:
            slot.state = EncodedSlotState::kFree;
            slot.delivery = EncodedDeliveryState::kFailed;
            break;
        }
      }
      submitted = true;
    }
  }

  if (!submitted) {
    Reset();
    return {};
  }
  state_.reset();
  identity_ = {};
  return {.encoded_bytes = encoded.encoded_bytes, .submitted = true};
}

bool InMemoryEncodedDestinationAccess::TryAttachRuntime(
    ulog::testing::InMemoryEncodedDestination& destination, EncodedDestinationWake wake) noexcept {
  const auto& state = destination.state_;
  if (state == nullptr) {
    return false;
  }
  std::lock_guard lock{state->mutex};
  if (state->stopped || state->runtime_attached) {
    return false;
  }
  state->runtime_attached = true;
  state->wake = wake;
  return true;
}

void InMemoryEncodedDestinationAccess::DetachRuntime(
    ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  const auto& state = destination.state_;
  if (state == nullptr) {
    return;
  }
  std::lock_guard lock{state->mutex};
  state->runtime_attached = false;
  state->wake = {};
}

EncodedDestinationWriteClaim InMemoryEncodedDestinationAccess::TryClaimWrite(
    ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  auto state = destination.state_;
  if (state == nullptr) {
    return {};
  }
  std::lock_guard lock{state->mutex};
  if (state->stopped || state->paused) {
    return {};
  }
  const auto slot_index = state->FindClaimableSlot();
  if (!slot_index) {
    return {};
  }
  auto& slot = state->slots[*slot_index];
  slot.generation = NextGeneration(slot.generation);
  slot.state = EncodedSlotState::kReserved;
  const EncodedDestinationSlotIdentity identity{.index = *slot_index,
                                                .generation = slot.generation};
  return EncodedDestinationWriteClaim{std::move(state), identity};
}

std::optional<EncodedDeliveryRetirement> InMemoryEncodedDestinationAccess::TryRetire(
    ulog::testing::InMemoryEncodedDestination& destination,
    std::uint64_t admission_sequence) noexcept {
  const auto& state = destination.state_;
  if (state == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock{state->mutex};
  for (std::size_t index = 0; index < state->capacity_records; ++index) {
    auto& slot = state->slots[index];
    const bool completed = slot.delivery == EncodedDeliveryState::kDelivered ||
                           slot.delivery == EncodedDeliveryState::kFailed;
    if (!completed || slot.admission_sequence != admission_sequence) {
      continue;
    }
    const EncodedDeliveryRetirement retirement{
        .outcome = slot.delivery == EncodedDeliveryState::kDelivered
                       ? EncodedDeliveryOutcome::kDelivered
                       : EncodedDeliveryOutcome::kFailed,
        .encoded_bytes = slot.encoded_bytes,
    };
    slot.delivery = EncodedDeliveryState::kRetired;
    return retirement;
  }
  return std::nullopt;
}

EncodedInFlightSummary InMemoryEncodedDestinationAccess::SummarizeInFlight(
    const ulog::testing::InMemoryEncodedDestination& destination,
    std::uint64_t watermark) noexcept {
  const auto& state = destination.state_;
  EncodedInFlightSummary summary;
  if (state == nullptr) {
    return summary;
  }
  std::lock_guard lock{state->mutex};
  for (std::size_t index = 0; index < state->capacity_records; ++index) {
    const auto& slot = state->slots[index];
    if (slot.delivery == EncodedDeliveryState::kRetired || slot.admission_sequence >= watermark) {
      continue;
    }
    ++summary.records;
    summary.bytes += slot.encoded_bytes;
    if (slot.delivery == EncodedDeliveryState::kDelivered) {
      ++summary.delivered_records;
      summary.delivered_bytes += slot.encoded_bytes;
    } else if (slot.delivery == EncodedDeliveryState::kFailed) {
      ++summary.failed_records;
      summary.failed_bytes += slot.encoded_bytes;
    }
  }
  return summary;
}

void InMemoryEncodedDestinationAccess::Stop(
    ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  const auto& state = destination.state_;
  if (state == nullptr) {
    return;
  }
  std::lock_guard lock{state->mutex};
  state->stopped = true;
  for (std::size_t index = 0; index < state->capacity_records; ++index) {
    auto& slot = state->slots[index];
    if (slot.state == EncodedSlotState::kPending || slot.state == EncodedSlotState::kDelivering) {
      slot.state = EncodedSlotState::kFree;
      slot.delivery = EncodedDeliveryState::kCancelled;
    }
  }
}

void InMemoryEncodedDestinationAccess::DiscardInFlight(
    ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  const auto& state = destination.state_;
  if (state == nullptr) {
    return;
  }
  std::lock_guard lock{state->mutex};
  for (std::size_t index = 0; index < state->capacity_records; ++index) {
    state->slots[index].delivery = EncodedDeliveryState::kRetired;
  }
}

std::size_t InMemoryEncodedDestinationAccess::FixedBackingBytes(
    const ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  return destination.state_ != nullptr ? destination.state_->fixed_backing_bytes : 0U;
}

}  // namespace ulog::detail::testing

namespace ulog::testing {

PendingEncodedDelivery::PendingEncodedDelivery(
    std::shared_ptr<detail::testing::InMemoryEncodedDestinationState> state,
    detail::testing::EncodedDestinationSlotIdentity identity, std::uint64_t admission_sequence,
    std::string_view bytes) noexcept
    : state_(std::move(state)),
      identity_(identity),
      admission_sequence_(admission_sequence),
      bytes_(bytes) {}

PendingEncodedDelivery::PendingEncodedDelivery(PendingEncodedDelivery&& other) noexcept
    : state_(std::move(other.state_)),
      identity_(std::exchange(other.identity_, {})),
      admission_sequence_(std::exchange(other.admission_sequence_, 0)),
      bytes_(std::exchange(other.bytes_, {})) {}

PendingEncodedDelivery& PendingEncodedDelivery::operator=(PendingEncodedDelivery&& other) noexcept {
  if (this != &other) {
    static_cast<void>(Finish(false));
    state_ = std::move(other.state_);
    identity_ = std::exchange(other.identity_, {});
    admission_sequence_ = std::exchange(other.admission_sequence_, 0);
    bytes_ = std::exchange(other.bytes_, {});
  }
  return *this;
}

PendingEncodedDelivery::~PendingEncodedDelivery() { static_cast<void>(Finish(false)); }

EncodedDeliveryCompletionStatus PendingEncodedDelivery::Complete() noexcept { return Finish(true); }

EncodedDeliveryCompletionStatus PendingEncodedDelivery::Fail() noexcept { return Finish(false); }

EncodedDeliveryCompletionStatus PendingEncodedDelivery::Finish(bool delivered) noexcept {
  if (state_ == nullptr) {
    return EncodedDeliveryCompletionStatus::kInvalidHandle;
  }
  const auto state = std::move(state_);
  const auto identity = std::exchange(identity_, {});
  admission_sequence_ = 0;
  bytes_ = {};

  std::lock_guard lock{state->mutex};
  if (identity.index >= state->capacity_records) {
    return EncodedDeliveryCompletionStatus::kCancelled;
  }
  auto& slot = state->slots[identity.index];
  if (slot.state != detail::testing::EncodedSlotState::kDelivering ||
      slot.generation != identity.generation) {
    return EncodedDeliveryCompletionStatus::kCancelled;
  }
  if (delivered) {
    slot.state = detail::testing::EncodedSlotState::kReady;
    slot.delivery = detail::testing::EncodedDeliveryState::kDelivered;
  } else {
    slot.state = detail::testing::EncodedSlotState::kFree;
    slot.delivery = detail::testing::EncodedDeliveryState::kFailed;
  }
  state->WakeRuntimeLocked();
  return EncodedDeliveryCompletionStatus::kCompleted;
}

ObservedEncodedRecord::ObservedEncodedRecord(
    std::shared_ptr<detail::testing::InMemoryEncodedDestinationState> state,
    detail::testing::EncodedDestinationSlotIdentity identity) noexcept
    : state_(std::move(state)), identity_(identity) {}

ObservedEncodedRecord::ObservedEncodedRecord(ObservedEncodedRecord&& other) noexcept
    : state_(std::move(other.state_)), identity_(std::exchange(other.identity_, {})) {}

ObservedEncodedRecord& ObservedEncodedRecord::operator=(ObservedEncodedRecord&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    identity_ = std::exchange(other.identity_, {});
  }
  return *this;
}

ObservedEncodedRecord::~ObservedEncodedRecord() { Reset(); }

void ObservedEncodedRecord::Reset() noexcept {
  detail::testing::ReleaseSlotHandle(state_, identity_, detail::testing::EncodedSlotState::kHeld);
}

std::uint64_t ObservedEncodedRecord::AdmissionSequence() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, identity_);
  return slot != nullptr ? slot->admission_sequence : 0U;
}

std::string_view ObservedEncodedRecord::Bytes() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, identity_);
  return slot != nullptr
             ? std::string_view{state_->SlotBacking(identity_.index), slot->encoded_bytes}
             : std::string_view{};
}

InMemoryEncodedDestination::InMemoryEncodedDestination(InMemoryEncodedDestinationConfig config)
    : state_(detail::testing::MakeState(config)) {}

std::optional<ObservedEncodedRecord> InMemoryEncodedDestination::TryTake() noexcept {
  if (state_ == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock{state_->mutex};
  const auto selected = state_->FindLowestSequence(detail::testing::EncodedSlotState::kReady);
  if (!selected) {
    return std::nullopt;
  }
  auto& slot = state_->slots[*selected];
  slot.state = detail::testing::EncodedSlotState::kHeld;
  ObservedEncodedRecord record{state_, detail::testing::EncodedDestinationSlotIdentity{
                                           .index = *selected, .generation = slot.generation}};
  return std::optional<ObservedEncodedRecord>{std::move(record)};
}

std::optional<PendingEncodedDelivery>
InMemoryEncodedDestination::TryTakePendingDelivery() noexcept {
  if (state_ == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock{state_->mutex};
  const auto selected = state_->FindLowestSequence(detail::testing::EncodedSlotState::kPending);
  if (!selected) {
    return std::nullopt;
  }
  auto& slot = state_->slots[*selected];
  slot.state = detail::testing::EncodedSlotState::kDelivering;
  PendingEncodedDelivery delivery{
      state_,
      detail::testing::EncodedDestinationSlotIdentity{.index = *selected,
                                                      .generation = slot.generation},
      slot.admission_sequence,
      std::string_view{state_->SlotBacking(*selected), slot.encoded_bytes}};
  return std::optional<PendingEncodedDelivery>{std::move(delivery)};
}

void InMemoryEncodedDestination::SetDeliveryMode(EncodedDeliveryMode mode) noexcept {
  if (state_ == nullptr || !detail::testing::IsKnownDeliveryMode(mode)) {
    return;
  }
  std::lock_guard lock{state_->mutex};
  state_->delivery_mode = mode;
}

void InMemoryEncodedDestination::Resume() noexcept {
  if (state_ == nullptr) {
    return;
  }
  std::lock_guard lock{state_->mutex};
  state_->paused = false;
  state_->WakeRuntimeLocked();
}

std::size_t InMemoryEncodedDestination::Capacity() const noexcept {
  return state_ != nullptr ? state_->capacity_records : 0U;
}

std::size_t InMemoryEncodedDestination::MaximumRecordBytes() const noexcept {
  return state_ != nullptr ? state_->maximum_record_bytes : 0U;
}

std::size_t InMemoryEncodedDestination::MaximumEncodedRecordBytes() const noexcept {
  return state_ != nullptr ? state_->maximum_encoded_record_bytes : 0U;
}

}  // namespace ulog::testing
