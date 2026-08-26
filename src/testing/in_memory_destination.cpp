#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "testing/in_memory_destination_access.hpp"

namespace ulog::detail::testing {
namespace {

enum class SlotState : std::uint8_t { kFree, kReserved, kReady, kHeld };

struct SlotMetadata final {
  SlotState state{SlotState::kFree};
  std::uint64_t generation{0};
  std::uint64_t admission_sequence{0};
  producer::EventTimestamp event_timestamp{0};
  std::size_t serialized_bytes{0};
  std::size_t accounting_charge_bytes{0};
  std::uint32_t source_path_offset{0};
  std::uint32_t source_path_size{0};
  std::uint32_t source_function_offset{0};
  std::uint32_t source_function_size{0};
  std::uint32_t message_offset{0};
  std::uint32_t message_size{0};
  std::uint32_t source_line{0};
  std::uint32_t source_column{0};
  Level level{Level::kNone};
  bool truncated{false};
};

[[noreturn]] void ThrowInvalidConfiguration(std::string_view detail, std::string_view correction) {
  throw std::invalid_argument("Invalid in-memory destination configuration: " +
                              std::string{detail} + " Set " + std::string{correction} + ".");
}

[[nodiscard]] std::size_t ValidateAndGetFixedBackingBytes(
    const ulog::testing::InMemoryDestinationConfig& config) {
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
  const std::size_t maximum_size = std::numeric_limits<std::size_t>::max();
  if (config.capacity_records > maximum_size / config.maximum_record_bytes ||
      config.capacity_records > maximum_size / sizeof(SlotMetadata)) {
    ThrowInvalidConfiguration("the requested fixed backing size overflows size_t;",
                              "capacity_records or maximum_record_bytes to a smaller value");
  }
  const std::size_t record_backing_bytes = config.capacity_records * config.maximum_record_bytes;
  const std::size_t metadata_backing_bytes = config.capacity_records * sizeof(SlotMetadata);
  if (metadata_backing_bytes > maximum_size - record_backing_bytes) {
    ThrowInvalidConfiguration("the requested fixed backing size overflows size_t;",
                              "capacity_records or maximum_record_bytes to a smaller value");
  }
  return record_backing_bytes + metadata_backing_bytes;
}

[[nodiscard]] std::uint64_t NextGeneration(std::uint64_t generation) noexcept {
  ++generation;
  return generation == 0U ? 1U : generation;
}

}  // namespace

struct InMemoryDestinationState final {
  InMemoryDestinationState(const ulog::testing::InMemoryDestinationConfig& config,
                           std::size_t validated_fixed_backing_bytes)
      : backing(
            std::make_unique<std::byte[]>(config.capacity_records * config.maximum_record_bytes)),
        slots(std::make_unique<SlotMetadata[]>(config.capacity_records)),
        capacity_records(config.capacity_records),
        maximum_record_bytes(config.maximum_record_bytes),
        fixed_backing_bytes(validated_fixed_backing_bytes),
        paused(config.start_paused) {}

  [[nodiscard]] std::byte* SlotBacking(std::size_t slot_index) noexcept {
    return backing.get() + slot_index * maximum_record_bytes;
  }
  [[nodiscard]] const std::byte* SlotBacking(std::size_t slot_index) const noexcept {
    return backing.get() + slot_index * maximum_record_bytes;
  }
  [[nodiscard]] std::optional<std::size_t> FindFreeSlot() const noexcept {
    for (std::size_t index = 0; index < capacity_records; ++index) {
      if (slots[index].state == SlotState::kFree) {
        return index;
      }
    }
    return std::nullopt;
  }

  std::mutex mutex;
  std::condition_variable writable;
  std::unique_ptr<std::byte[]> backing;
  std::unique_ptr<SlotMetadata[]> slots;
  const std::size_t capacity_records;
  const std::size_t maximum_record_bytes;
  const std::size_t fixed_backing_bytes;
  bool paused{false};
  bool stopped{false};
};

namespace {

[[nodiscard]] std::shared_ptr<InMemoryDestinationState> MakeState(
    const ulog::testing::InMemoryDestinationConfig& config) {
  const std::size_t fixed_backing_bytes = ValidateAndGetFixedBackingBytes(config);
  return std::make_shared<InMemoryDestinationState>(config, fixed_backing_bytes);
}

[[nodiscard]] const SlotMetadata* FindHeldSlot(
    const std::shared_ptr<InMemoryDestinationState>& state, std::size_t slot_index,
    std::uint64_t generation) noexcept {
  if (state == nullptr || slot_index >= state->capacity_records) {
    return nullptr;
  }
  const auto& slot = state->slots[slot_index];
  return slot.state == SlotState::kHeld && slot.generation == generation ? &slot : nullptr;
}

[[nodiscard]] std::string_view StoredText(const std::shared_ptr<InMemoryDestinationState>& state,
                                          std::size_t slot_index, std::uint32_t offset,
                                          std::uint32_t size) noexcept {
  if (state == nullptr || slot_index >= state->capacity_records ||
      offset > state->maximum_record_bytes ||
      size > state->maximum_record_bytes - static_cast<std::size_t>(offset)) {
    return {};
  }
  return {reinterpret_cast<const char*>(state->SlotBacking(slot_index) + offset), size};
}

struct StoredTextLocation final {
  std::uint32_t offset{0};
  std::uint32_t size{0};
  bool complete{true};
};

[[nodiscard]] StoredTextLocation CopyText(std::byte* destination, std::size_t capacity,
                                          std::size_t& cursor, std::string_view source) noexcept {
  const std::size_t available = cursor <= capacity ? capacity - cursor : 0U;
  const std::size_t stored_size = std::min(available, source.size());
  const std::size_t offset = cursor;
  if (stored_size != 0U) {
    std::memcpy(destination + cursor, source.data(), stored_size);
  }
  cursor += stored_size;
  return {.offset = static_cast<std::uint32_t>(offset),
          .size = static_cast<std::uint32_t>(stored_size),
          .complete = stored_size == source.size()};
}

}  // namespace

DestinationWriteClaim::DestinationWriteClaim(std::shared_ptr<InMemoryDestinationState> state,
                                             std::size_t slot_index,
                                             std::uint64_t generation) noexcept
    : state_(std::move(state)), slot_index_(slot_index), generation_(generation) {}

DestinationWriteClaim::DestinationWriteClaim(DestinationWriteClaim&& other) noexcept
    : state_(std::move(other.state_)),
      slot_index_(std::exchange(other.slot_index_, 0U)),
      generation_(std::exchange(other.generation_, 0U)) {}

DestinationWriteClaim& DestinationWriteClaim::operator=(DestinationWriteClaim&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    slot_index_ = std::exchange(other.slot_index_, 0U);
    generation_ = std::exchange(other.generation_, 0U);
  }
  return *this;
}

DestinationWriteClaim::~DestinationWriteClaim() { Reset(); }

void DestinationWriteClaim::Reset() noexcept {
  if (state_ == nullptr) {
    return;
  }
  bool released = false;
  {
    std::lock_guard lock{state_->mutex};
    if (slot_index_ < state_->capacity_records) {
      auto& slot = state_->slots[slot_index_];
      if (slot.state == SlotState::kReserved && slot.generation == generation_) {
        slot.state = SlotState::kFree;
        released = true;
      }
    }
  }
  auto state = std::move(state_);
  slot_index_ = 0;
  generation_ = 0;
  if (released) {
    state->writable.notify_one();
  }
}

void DestinationWriteClaim::Store(std::uint64_t admission_sequence,
                                  const producer::RecordView& record) noexcept {
  if (state_ == nullptr || slot_index_ >= state_->capacity_records) {
    return;
  }

  auto& slot = state_->slots[slot_index_];
  std::byte* const destination = state_->SlotBacking(slot_index_);
  std::size_t cursor = 0;
  const StoredTextLocation source_path =
      CopyText(destination, state_->maximum_record_bytes, cursor, record.source_path());
  const StoredTextLocation source_function =
      CopyText(destination, state_->maximum_record_bytes, cursor, record.source_function());
  const StoredTextLocation message =
      CopyText(destination, state_->maximum_record_bytes, cursor, record.message());

  bool committed = false;
  {
    std::lock_guard lock{state_->mutex};
    if (slot.state == SlotState::kReserved && slot.generation == generation_) {
      slot.admission_sequence = admission_sequence;
      slot.event_timestamp = record.event_timestamp();
      slot.serialized_bytes = record.serialized_bytes();
      slot.accounting_charge_bytes = record.accounting_charge_bytes();
      slot.source_path_offset = source_path.offset;
      slot.source_path_size = source_path.size;
      slot.source_function_offset = source_function.offset;
      slot.source_function_size = source_function.size;
      slot.message_offset = message.offset;
      slot.message_size = message.size;
      slot.source_line = record.source_line();
      slot.source_column = record.source_column();
      slot.level = record.level();
      slot.truncated = record.truncated() || !source_path.complete || !source_function.complete ||
                       !message.complete;
      slot.state = SlotState::kReady;
      committed = true;
    }
  }

  if (committed) {
    state_.reset();
    slot_index_ = 0;
    generation_ = 0;
  } else {
    Reset();
  }
}

DestinationWriteClaim InMemoryDestinationAccess::WaitForWrite(
    ulog::testing::InMemoryDestination& destination) {
  auto state = destination.state_;
  if (state == nullptr) {
    return {};
  }
  std::unique_lock lock{state->mutex};
  state->writable.wait(lock, [&state] {
    return state->stopped || (!state->paused && state->FindFreeSlot().has_value());
  });
  if (state->stopped) {
    return {};
  }
  const auto slot_index = state->FindFreeSlot();
  if (!slot_index) {
    return {};
  }
  auto& slot = state->slots[*slot_index];
  slot.generation = NextGeneration(slot.generation);
  slot.state = SlotState::kReserved;
  return DestinationWriteClaim{std::move(state), *slot_index, slot.generation};
}

void InMemoryDestinationAccess::Stop(ulog::testing::InMemoryDestination& destination) noexcept {
  auto state = destination.state_;
  if (state == nullptr) {
    return;
  }
  {
    std::lock_guard lock{state->mutex};
    state->stopped = true;
  }
  state->writable.notify_all();
}

std::size_t InMemoryDestinationAccess::FixedBackingBytes(
    const ulog::testing::InMemoryDestination& destination) noexcept {
  return destination.state_ != nullptr ? destination.state_->fixed_backing_bytes : 0U;
}

}  // namespace ulog::detail::testing

namespace ulog::testing {

ObservedRecord::ObservedRecord(std::shared_ptr<detail::testing::InMemoryDestinationState> state,
                               std::size_t slot_index, std::uint64_t generation) noexcept
    : state_(std::move(state)), slot_index_(slot_index), generation_(generation) {}

ObservedRecord::ObservedRecord(ObservedRecord&& other) noexcept
    : state_(std::move(other.state_)),
      slot_index_(std::exchange(other.slot_index_, 0U)),
      generation_(std::exchange(other.generation_, 0U)) {}

ObservedRecord& ObservedRecord::operator=(ObservedRecord&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    slot_index_ = std::exchange(other.slot_index_, 0U);
    generation_ = std::exchange(other.generation_, 0U);
  }
  return *this;
}

ObservedRecord::~ObservedRecord() { Reset(); }

void ObservedRecord::Reset() noexcept {
  if (state_ == nullptr) {
    return;
  }
  bool released = false;
  {
    std::lock_guard lock{state_->mutex};
    if (slot_index_ < state_->capacity_records) {
      auto& slot = state_->slots[slot_index_];
      if (slot.state == detail::testing::SlotState::kHeld && slot.generation == generation_) {
        slot.state = detail::testing::SlotState::kFree;
        released = true;
      }
    }
  }
  auto state = std::move(state_);
  slot_index_ = 0;
  generation_ = 0;
  if (released) {
    state->writable.notify_one();
  }
}

std::uint64_t ObservedRecord::AdmissionSequence() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? slot->admission_sequence : 0U;
}

Level ObservedRecord::GetLevel() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? slot->level : Level::kNone;
}

std::int64_t ObservedRecord::EventTimestamp() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? slot->event_timestamp : 0;
}

std::string_view ObservedRecord::SourcePath() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr
             ? detail::testing::StoredText(state_, slot_index_, slot->source_path_offset,
                                           slot->source_path_size)
             : std::string_view{};
}

std::string_view ObservedRecord::SourceFunction() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr
             ? detail::testing::StoredText(state_, slot_index_, slot->source_function_offset,
                                           slot->source_function_size)
             : std::string_view{};
}

std::uint32_t ObservedRecord::SourceLine() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? slot->source_line : 0U;
}

std::uint32_t ObservedRecord::SourceColumn() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? slot->source_column : 0U;
}

std::string_view ObservedRecord::Message() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? detail::testing::StoredText(state_, slot_index_, slot->message_offset,
                                                       slot->message_size)
                         : std::string_view{};
}

bool ObservedRecord::Truncated() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr && slot->truncated;
}

std::size_t ObservedRecord::SerializedBytes() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? slot->serialized_bytes : 0U;
}

std::size_t ObservedRecord::AccountingChargeBytes() const noexcept {
  const auto* slot = detail::testing::FindHeldSlot(state_, slot_index_, generation_);
  return slot != nullptr ? slot->accounting_charge_bytes : 0U;
}

InMemoryDestination::InMemoryDestination(InMemoryDestinationConfig config)
    : state_(detail::testing::MakeState(config)) {}

std::optional<ObservedRecord> InMemoryDestination::TryTake() noexcept {
  if (state_ == nullptr) {
    return std::nullopt;
  }
  std::lock_guard lock{state_->mutex};
  std::optional<std::size_t> selected;
  for (std::size_t index = 0; index < state_->capacity_records; ++index) {
    const auto& slot = state_->slots[index];
    if (slot.state != detail::testing::SlotState::kReady) {
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
  slot.state = detail::testing::SlotState::kHeld;
  ObservedRecord record{state_, *selected, slot.generation};
  return std::optional<ObservedRecord>{std::move(record)};
}

void InMemoryDestination::Resume() noexcept {
  if (state_ == nullptr) {
    return;
  }
  {
    std::lock_guard lock{state_->mutex};
    state_->paused = false;
  }
  state_->writable.notify_all();
}

std::size_t InMemoryDestination::Capacity() const noexcept {
  return state_ != nullptr ? state_->capacity_records : 0U;
}

std::size_t InMemoryDestination::MaximumRecordBytes() const noexcept {
  return state_ != nullptr ? state_->maximum_record_bytes : 0U;
}

}  // namespace ulog::testing
