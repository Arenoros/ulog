#include <atomic>
#include <condition_variable>
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
#include <system_error>
#include <utility>

#include "encoding/raw_encoder.hpp"
#include "testing/in_memory_encoded_destination_access.hpp"

namespace ulog::detail::testing {
namespace {

enum class EncodedSlotState : std::uint8_t { kFree, kReserved, kReady, kHeld };

struct EncodedSlotMetadata final {
  EncodedSlotState state{EncodedSlotState::kFree};
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
  [[nodiscard]] std::optional<std::size_t> FindFreeSlot() const noexcept {
    for (std::size_t index = 0; index < capacity_records; ++index) {
      if (slots[index].state == EncodedSlotState::kFree) {
        return index;
      }
    }
    return std::nullopt;
  }

  std::mutex mutex;
  std::condition_variable writable;
  std::unique_ptr<char[]> backing;
  std::unique_ptr<EncodedSlotMetadata[]> slots;
  const std::size_t capacity_records;
  const std::size_t maximum_record_bytes;
  const std::size_t maximum_encoded_record_bytes;
  const std::size_t fixed_backing_bytes;
  std::atomic<bool> runtime_attached{false};
  bool paused{false};
  std::atomic<bool> stopped{false};
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
  bool released = false;
  {
    std::lock_guard lock{state->mutex};
    if (identity.index < state->capacity_records) {
      auto& slot = state->slots[identity.index];
      if (slot.state == expected_state && slot.generation == identity.generation) {
        slot.state = EncodedSlotState::kFree;
        slot.encoded_bytes = 0U;
        released = true;
      }
    }
  }
  identity = {};
  if (released) {
    state->writable.notify_one();
  }
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

  bool committed = false;
  {
    std::lock_guard lock{state_->mutex};
    if (slot.state == EncodedSlotState::kReserved && slot.generation == identity_.generation) {
      slot.admission_sequence = admission_sequence;
      slot.encoded_bytes = encoded.encoded_bytes;
      slot.state = EncodedSlotState::kReady;
      committed = true;
    }
  }

  if (!committed) {
    Reset();
    return {};
  }
  state_.reset();
  identity_ = {};
  return {.encoded_bytes = encoded.encoded_bytes, .committed = true};
}

bool InMemoryEncodedDestinationAccess::TryAttachRuntime(
    ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  auto state = destination.state_;
  if (state == nullptr || state->stopped.load(std::memory_order_acquire)) {
    return false;
  }
  bool expected = false;
  if (!state->runtime_attached.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return false;
  }
  if (state->stopped.load(std::memory_order_acquire)) {
    state->runtime_attached.store(false, std::memory_order_release);
    return false;
  }
  return true;
}

void InMemoryEncodedDestinationAccess::DetachRuntime(
    ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  auto state = destination.state_;
  if (state != nullptr) {
    state->runtime_attached.store(false, std::memory_order_release);
  }
}

EncodedDestinationWriteClaim InMemoryEncodedDestinationAccess::WaitForWrite(
    ulog::testing::InMemoryEncodedDestination& destination,
    std::chrono::steady_clock::duration recheck_interval) noexcept {
  try {
    auto state = destination.state_;
    if (state == nullptr) {
      return {};
    }
    std::unique_lock lock{state->mutex};
    state->writable.wait_for(lock, recheck_interval, [&state] {
      return state->stopped.load(std::memory_order_acquire) ||
             (!state->paused && state->FindFreeSlot().has_value());
    });
    if (state->stopped.load(std::memory_order_acquire)) {
      return {};
    }
    const auto slot_index = state->FindFreeSlot();
    if (!slot_index) {
      return {};
    }
    auto& slot = state->slots[*slot_index];
    slot.generation = NextGeneration(slot.generation);
    slot.state = EncodedSlotState::kReserved;
    return EncodedDestinationWriteClaim{
        std::move(state),
        EncodedDestinationSlotIdentity{.index = *slot_index, .generation = slot.generation}};
  } catch (const std::system_error&) {
    return {};
  }
}

void InMemoryEncodedDestinationAccess::Stop(
    ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  auto state = destination.state_;
  if (state == nullptr) {
    return;
  }
  state->stopped.store(true, std::memory_order_release);
  state->writable.notify_all();
}

std::size_t InMemoryEncodedDestinationAccess::FixedBackingBytes(
    const ulog::testing::InMemoryEncodedDestination& destination) noexcept {
  return destination.state_ != nullptr ? destination.state_->fixed_backing_bytes : 0U;
}

}  // namespace ulog::detail::testing

namespace ulog::testing {

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
  std::optional<std::size_t> selected;
  for (std::size_t index = 0; index < state_->capacity_records; ++index) {
    const auto& slot = state_->slots[index];
    if (slot.state != detail::testing::EncodedSlotState::kReady) {
      continue;
    }
    if (!selected || slot.admission_sequence < state_->slots[*selected].admission_sequence) {
      selected = index;
    }
  }
  if (!selected) {
    return std::nullopt;
  }
  auto& slot = state_->slots[*selected];
  slot.state = detail::testing::EncodedSlotState::kHeld;
  ObservedEncodedRecord record{state_, detail::testing::EncodedDestinationSlotIdentity{
                                           .index = *selected, .generation = slot.generation}};
  return std::optional<ObservedEncodedRecord>{std::move(record)};
}

void InMemoryEncodedDestination::Resume() noexcept {
  if (state_ == nullptr) {
    return;
  }
  {
    std::lock_guard lock{state_->mutex};
    state_->paused = false;
  }
  state_->writable.notify_all();
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
