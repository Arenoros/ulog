#include "producer/record_storage.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace ulog::detail::producer::record {
namespace {

template <typename Value>
void WriteObject(std::byte* storage, std::size_t offset, const Value& value) noexcept {
  static_assert(std::is_trivially_copyable_v<Value>);
  std::memcpy(storage + offset, &value, sizeof(value));
}

template <typename Value>
[[nodiscard]] Value ReadObject(const void* storage, std::size_t offset) noexcept {
  static_assert(std::is_trivially_copyable_v<Value>);
  Value value{};
  std::memcpy(&value, static_cast<const std::byte*>(storage) + offset, sizeof(value));
  return value;
}

struct SaturatingAddInput final {
  std::uint64_t value;
  std::size_t increment;
};

[[nodiscard]] std::uint64_t SaturatingAdd(SaturatingAddInput input) noexcept {
  std::uint64_t converted = 0;
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    converted = input.increment > std::numeric_limits<std::uint64_t>::max()
                    ? std::numeric_limits<std::uint64_t>::max()
                    : static_cast<std::uint64_t>(input.increment);
  } else {
    converted = static_cast<std::uint64_t>(input.increment);
  }
  return converted > std::numeric_limits<std::uint64_t>::max() - input.value
             ? std::numeric_limits<std::uint64_t>::max()
             : input.value + converted;
}

[[nodiscard]] bool IsContinuationByte(unsigned char value) noexcept {
  return (value & 0xC0U) == 0x80U;
}

[[nodiscard]] std::size_t ValidUtf8PrefixLength(std::span<const std::byte> value) noexcept {
  std::size_t valid_size = 0;
  while (valid_size < value.size()) {
    const auto first = std::to_integer<unsigned char>(value[valid_size]);
    std::size_t sequence_size = 0;
    if (first <= 0x7FU) {
      sequence_size = 1;
    } else if (first >= 0xC2U && first <= 0xDFU) {
      sequence_size = 2;
    } else if (first >= 0xE0U && first <= 0xEFU) {
      sequence_size = 3;
    } else if (first >= 0xF0U && first <= 0xF4U) {
      sequence_size = 4;
    } else {
      break;
    }
    if (sequence_size > value.size() - valid_size) {
      break;
    }
    bool valid_sequence = true;
    for (std::size_t index = 1; index < sequence_size; ++index) {
      valid_sequence =
          valid_sequence &&
          IsContinuationByte(std::to_integer<unsigned char>(value[valid_size + index]));
    }
    if (!valid_sequence) {
      break;
    }
    if (sequence_size == 3) {
      const auto second = std::to_integer<unsigned char>(value[valid_size + 1U]);
      if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second >= 0xA0U)) {
        break;
      }
    } else if (sequence_size == 4) {
      const auto second = std::to_integer<unsigned char>(value[valid_size + 1U]);
      if ((first == 0xF0U && second < 0x90U) || (first == 0xF4U && second >= 0x90U)) {
        break;
      }
    }
    valid_size += sequence_size;
  }
  return valid_size;
}

[[nodiscard]] std::span<const std::byte> Bytes(std::string_view value) noexcept {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

}  // namespace

std::size_t AccountingCharge(std::size_t serialized_bytes) noexcept {
  const std::size_t at_least_one_quantum = std::max(serialized_bytes, kAccountingQuantumBytes);
  constexpr std::size_t kRoundingMask = kAccountingQuantumBytes - 1U;
  if (at_least_one_quantum > std::numeric_limits<std::size_t>::max() - kRoundingMask) {
    return 0;
  }
  return (at_least_one_quantum + kRoundingMask) & ~kRoundingMask;
}

SerializedRecordHeader Header(const void* storage) noexcept {
  return storage != nullptr ? ReadObject<SerializedRecordHeader>(storage, 0)
                            : SerializedRecordHeader{};
}

SerializedField Field(const void* storage, std::uint32_t offset) noexcept {
  return storage != nullptr ? ReadObject<SerializedField>(storage, offset) : SerializedField{};
}

std::string_view Text(const void* storage, std::uint32_t offset, std::uint32_t size) noexcept {
  if (storage == nullptr) {
    return {};
  }
  return {reinterpret_cast<const char*>(static_cast<const std::byte*>(storage) + offset), size};
}

RecordWriter::RecordWriter(RecordSlot& slot, InitialState initial_state) noexcept
    : slot_(&slot),
      front_end_(initial_state.message_offset),
      field_tail_(initial_state.serialized_limit),
      message_offset_(initial_state.message_offset),
      source_payload_bytes_(initial_state.source_payload_bytes) {}

RecordWriter::RecordWriter(RecordWriter&& other) noexcept { MoveFrom(other); }

RecordWriter& RecordWriter::operator=(RecordWriter&& other) noexcept {
  if (this != &other) {
    Abandon();
    MoveFrom(other);
  }
  return *this;
}

RecordWriter::~RecordWriter() { Abandon(); }

void RecordWriter::MoveFrom(RecordWriter& other) noexcept {
  slot_ = std::exchange(other.slot_, nullptr);
  front_end_ = other.front_end_;
  field_tail_ = other.field_tail_;
  message_offset_ = other.message_offset_;
  message_size_ = other.message_size_;
  requested_message_bytes_ = other.requested_message_bytes_;
  source_payload_bytes_ = other.source_payload_bytes_;
  field_payload_bytes_ = other.field_payload_bytes_;
  dead_field_bytes_ = other.dead_field_bytes_;
  field_count_ = other.field_count_;
  first_field_offset_ = other.first_field_offset_;
  last_field_offset_ = other.last_field_offset_;
  fields_truncated_ = other.fields_truncated_;
}

WriteResult RecordWriter::Append(std::string_view value) noexcept {
  return AppendBytes(Bytes(value));
}

WriteResult RecordWriter::Append(std::int64_t value) noexcept { return AppendNumber(value); }

WriteResult RecordWriter::Append(std::uint64_t value) noexcept { return AppendNumber(value); }

WriteResult RecordWriter::Append(double value) noexcept {
  std::array<char, 64> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                    std::chars_format::general);
  if (result.ec != std::errc{}) {
    return {.truncated = true};
  }
  return Append(
      std::string_view{buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
}

WriteResult RecordWriter::Append(bool value) noexcept {
  return Append(value ? std::string_view{"true"} : std::string_view{"false"});
}

WriteResult RecordWriter::AppendBytes(std::span<const std::byte> value) noexcept {
  requested_message_bytes_ =
      SaturatingAdd({.value = requested_message_bytes_, .increment = value.size()});
  if (slot_ == nullptr) {
    return {.requested_bytes = value.size(), .stored_bytes = 0, .truncated = !value.empty()};
  }
  const std::size_t stored = std::min(value.size(), field_tail_ - front_end_);
  if (stored != 0U) {
    std::memcpy(slot_->storage_.data() + front_end_, value.data(), stored);
  }
  front_end_ += stored;
  message_size_ += stored;
  return {
      .requested_bytes = value.size(), .stored_bytes = stored, .truncated = stored != value.size()};
}

template <typename Value>
WriteResult RecordWriter::AppendNumber(Value value) noexcept {
  std::array<char, std::numeric_limits<Value>::digits10 + 3U> buffer{};
  const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (result.ec != std::errc{}) {
    return {.truncated = true};
  }
  return Append(
      std::string_view{buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
}

namespace {

[[nodiscard]] bool Stored(FieldStatus status) noexcept {
  return status == FieldStatus::kAdded || status == FieldStatus::kReplaced;
}

[[nodiscard]] std::size_t StoredValueBytes(const SerializedField& field) noexcept {
  const auto kind = static_cast<FieldKind>(field.kind);
  return kind == FieldKind::kString || kind == FieldKind::kJson ? field.value_high : 0U;
}

}  // namespace

bool RecordWriter::AddField(std::string_view key, std::string_view value) noexcept {
  return Stored(PutField(key, FieldValue{value}, FieldMode::kReplaceable));
}

bool RecordWriter::AddField(std::string_view key, std::int64_t value) noexcept {
  return Stored(PutField(key, FieldValue{value}, FieldMode::kReplaceable));
}

bool RecordWriter::AddField(std::string_view key, std::uint64_t value) noexcept {
  return Stored(PutField(key, FieldValue{value}, FieldMode::kReplaceable));
}

bool RecordWriter::AddField(std::string_view key, double value) noexcept {
  return Stored(PutField(key, FieldValue{value}, FieldMode::kReplaceable));
}

bool RecordWriter::AddField(std::string_view key, bool value) noexcept {
  return Stored(PutField(key, FieldValue{value}, FieldMode::kReplaceable));
}

bool RecordWriter::AddField(std::string_view key, NullValue) noexcept {
  return Stored(PutField(key, FieldValue{kNull}, FieldMode::kReplaceable));
}

FieldStatus RecordWriter::PutField(std::string_view key, const FieldValue& value,
                                   FieldMode mode) noexcept {
  if (slot_ == nullptr) {
    return FieldStatus::kTruncated;
  }
  if (key.empty()) {
    return FieldStatus::kInvalidKey;
  }
  if (IsReservedFieldKey(key)) {
    return FieldStatus::kReserved;
  }
  std::uint32_t previous = 0;
  // ponytail: linear key scan per field; add an index if call sites carry hundreds of fields.
  const std::uint32_t existing = FindField(key, previous);
  if (existing != 0U && (ReadObject<SerializedField>(slot_->storage_.data(), existing).flags &
                         kFieldFrozenFlag) != 0U) {
    return FieldStatus::kFrozen;
  }
  std::uint8_t flags = mode == FieldMode::kFrozen ? kFieldFrozenFlag : std::uint8_t{0};
  if (value.Kind() == FieldKind::kBool && value.Bool()) {
    flags |= kFieldBoolTrueFlag;
  }
  const auto offset = WriteFieldNode(key, value, flags);
  if (!offset) {
    fields_truncated_ = true;
    return FieldStatus::kTruncated;
  }
  if (existing == 0U) {
    LinkLast(*offset);
    return FieldStatus::kAdded;
  }
  ReplaceNode(previous, existing, *offset);
  return FieldStatus::kReplaced;
}

std::uint32_t RecordWriter::FindField(std::string_view key,
                                      std::uint32_t& previous) const noexcept {
  previous = 0;
  std::uint32_t offset = first_field_offset_;
  for (std::uint32_t index = 0; index < field_count_ && offset != 0U; ++index) {
    const auto field = ReadObject<SerializedField>(slot_->storage_.data(), offset);
    if (Text(slot_->storage_.data(), field.key_offset, field.key_size) == key) {
      return offset;
    }
    previous = offset;
    offset = field.next_offset;
  }
  return 0;
}

std::optional<std::uint32_t> RecordWriter::WriteFieldNode(std::string_view key,
                                                          const FieldValue& value,
                                                          std::uint8_t flags) noexcept {
  const bool has_text = value.Kind() == FieldKind::kString || value.Kind() == FieldKind::kJson;
  const std::string_view text = has_text ? value.Text() : std::string_view{};
  if (field_count_ == std::numeric_limits<std::uint32_t>::max() ||
      key.size() > kMaximumRecordBytes || text.size() > kMaximumRecordBytes) {
    return std::nullopt;
  }
  const std::size_t field_size = kSerializedFieldMetadataBytes + key.size() + text.size();
  const std::size_t available = field_tail_ - front_end_;
  if (field_size > available) {
    return std::nullopt;
  }

  field_tail_ -= field_size;
  const auto field_offset = static_cast<std::uint32_t>(field_tail_);
  const auto key_offset = static_cast<std::uint32_t>(field_tail_ + kSerializedFieldMetadataBytes);
  const auto value_offset = static_cast<std::uint32_t>(key_offset + key.size());
  if (!key.empty()) {
    std::memcpy(slot_->storage_.data() + key_offset, key.data(), key.size());
  }
  if (!text.empty()) {
    std::memcpy(slot_->storage_.data() + value_offset, text.data(), text.size());
  }

  const bool scalar = value.Kind() == FieldKind::kInt64 || value.Kind() == FieldKind::kUInt64 ||
                      value.Kind() == FieldKind::kDouble;
  const std::uint64_t bits = scalar ? value.UInt64() : 0U;
  const SerializedField field{
      .next_offset = 0,
      .key_offset = key_offset,
      .key_size = static_cast<std::uint32_t>(key.size()),
      .value_low = has_text ? value_offset : static_cast<std::uint32_t>(bits),
      .value_high = has_text ? static_cast<std::uint32_t>(text.size())
                             : static_cast<std::uint32_t>(bits >> 32U),
      .kind = static_cast<std::uint8_t>(value.Kind()),
      .flags = flags,
  };
  WriteObject(slot_->storage_.data(), field_offset, field);
  ++field_count_;
  field_payload_bytes_ += key.size() + text.size();
  return field_offset;
}

void RecordWriter::LinkLast(std::uint32_t offset) noexcept {
  if (last_field_offset_ == 0U) {
    first_field_offset_ = offset;
  } else {
    auto previous = ReadObject<SerializedField>(slot_->storage_.data(), last_field_offset_);
    previous.next_offset = offset;
    WriteObject(slot_->storage_.data(), last_field_offset_, previous);
  }
  last_field_offset_ = offset;
}

void RecordWriter::ReplaceNode(std::uint32_t previous, std::uint32_t old_offset,
                               std::uint32_t new_offset) noexcept {
  std::byte* const storage = slot_->storage_.data();
  const auto old_field = ReadObject<SerializedField>(storage, old_offset);
  auto new_field = ReadObject<SerializedField>(storage, new_offset);
  new_field.next_offset = old_field.next_offset;
  new_field.replaced_offset = static_cast<std::uint16_t>(old_offset);
  WriteObject(storage, new_offset, new_field);
  if (previous == 0U) {
    first_field_offset_ = new_offset;
  } else {
    auto previous_field = ReadObject<SerializedField>(storage, previous);
    previous_field.next_offset = new_offset;
    WriteObject(storage, previous, previous_field);
  }
  if (last_field_offset_ == old_offset) {
    last_field_offset_ = new_offset;
  }
  const std::size_t old_payload = old_field.key_size + StoredValueBytes(old_field);
  --field_count_;
  field_payload_bytes_ -= old_payload;
  dead_field_bytes_ += kSerializedFieldMetadataBytes + old_payload;
}

void RecordWriter::DropNewestField() noexcept {
  // Nodes are written downwards, so the node at the field tail is the newest and always linked.
  // Dropping a replacement relinks the node it replaced, which is older and therefore still intact.
  std::byte* const storage = slot_->storage_.data();
  const auto newest_offset = static_cast<std::uint32_t>(field_tail_);
  const auto newest = ReadObject<SerializedField>(storage, newest_offset);
  std::uint32_t previous = 0;
  std::uint32_t offset = first_field_offset_;
  while (offset != 0U && offset != newest_offset) {
    previous = offset;
    offset = ReadObject<SerializedField>(storage, offset).next_offset;
  }
  std::uint32_t successor = newest.next_offset;
  const std::size_t newest_payload = newest.key_size + StoredValueBytes(newest);
  if (newest.replaced_offset != 0U) {
    auto restored = ReadObject<SerializedField>(storage, newest.replaced_offset);
    restored.next_offset = newest.next_offset;
    WriteObject(storage, newest.replaced_offset, restored);
    successor = newest.replaced_offset;
    const std::size_t restored_payload = restored.key_size + StoredValueBytes(restored);
    field_payload_bytes_ += restored_payload;
    dead_field_bytes_ -= kSerializedFieldMetadataBytes + restored_payload;
    ++field_count_;
  }
  if (previous == 0U) {
    first_field_offset_ = successor;
  } else {
    auto previous_field = ReadObject<SerializedField>(storage, previous);
    previous_field.next_offset = successor;
    WriteObject(storage, previous, previous_field);
  }
  if (last_field_offset_ == newest_offset) {
    last_field_offset_ = successor != newest.next_offset ? successor : previous;
  }
  field_tail_ += kSerializedFieldMetadataBytes + newest_payload;
  field_payload_bytes_ -= newest_payload;
  --field_count_;
}

bool RecordWriter::AddTruncatedField() noexcept {
  // Make room for the marker from the message tail first, then from the newest fields.
  if (field_tail_ - front_end_ < kTruncatedFieldBytes) {
    const std::size_t reclaimed =
        std::min(kTruncatedFieldBytes - (field_tail_ - front_end_), message_size_);
    message_size_ -= reclaimed;
    message_size_ = ValidUtf8PrefixLength(
        std::span<const std::byte>{slot_->storage_.data() + message_offset_, message_size_});
    front_end_ = message_offset_ + message_size_;
  }
  while (field_tail_ - front_end_ < kTruncatedFieldBytes && field_count_ != 0U) {
    DropNewestField();
  }
  const auto offset = WriteFieldNode(kTruncatedFieldKey, FieldValue{true}, kFieldBoolTrueFlag);
  if (!offset) {
    return false;
  }
  LinkLast(*offset);
  return true;
}

StoredRecord RecordWriter::Publish() && noexcept {
  if (slot_ == nullptr || slot_->state_ != RecordSlot::State::kBuilding) {
    return {};
  }
  message_size_ = ValidUtf8PrefixLength(
      std::span<const std::byte>{slot_->storage_.data() + message_offset_, message_size_});
  front_end_ = message_offset_ + message_size_;
  bool truncated =
      fields_truncated_ || requested_message_bytes_ != static_cast<std::uint64_t>(message_size_);
  if (truncated && !AddTruncatedField()) {
    Abandon();
    return {};
  }

  auto header = ReadObject<SerializedRecordHeader>(slot_->storage_.data(), 0);
  header.message_size = static_cast<std::uint32_t>(message_size_);
  header.first_field_offset = first_field_offset_;
  header.field_count = field_count_;
  header.flags = truncated ? 1U : 0U;
  WriteObject(slot_->storage_.data(), 0, header);

  const std::size_t owned_payload_bytes =
      source_payload_bytes_ + message_size_ + field_payload_bytes_;
  const std::size_t metadata_bytes =
      kSerializedRecordMetadataBytes +
      static_cast<std::size_t>(field_count_) * kSerializedFieldMetadataBytes;
  const std::size_t serialized_bytes = owned_payload_bytes + metadata_bytes + dead_field_bytes_;
  const std::size_t accounting_charge_bytes = AccountingCharge(serialized_bytes);
  const std::size_t serialized_limit = field_tail_ + kSerializedFieldMetadataBytes * field_count_ +
                                       field_payload_bytes_ + dead_field_bytes_;
  if (serialized_bytes > serialized_limit || accounting_charge_bytes == 0U ||
      accounting_charge_bytes > serialized_limit) {
    Abandon();
    return {};
  }

  RecordSlot* const slot = std::exchange(slot_, nullptr);
  slot->state_ = RecordSlot::State::kPublished;
  return {.storage = slot->storage_.data(),
          .serialized_bytes = serialized_bytes,
          .accounting_charge_bytes = accounting_charge_bytes,
          .truncated = truncated};
}

void RecordWriter::Abandon() noexcept {
  if (slot_ != nullptr) {
    slot_->Reset();
    slot_ = nullptr;
  }
}

RecordWriter RecordSlot::Begin(std::size_t serialized_limit, Level level,
                               const SourceLocation& source, EventTimestamp timestamp) noexcept {
  const auto level_value = static_cast<std::uint8_t>(level);
  const std::string_view source_path = source.GetFileName();
  const std::string_view source_function = source.GetFunctionName();
  if (state_ != State::kReset || serialized_limit < kSerializedRecordMetadataBytes ||
      serialized_limit > kMaximumRecordBytes ||
      level_value >= static_cast<std::uint8_t>(Level::kNone) ||
      source_path.size() > std::numeric_limits<std::uint32_t>::max() ||
      source_function.size() > std::numeric_limits<std::uint32_t>::max() ||
      source.GetLine() > std::numeric_limits<std::uint32_t>::max() ||
      source.GetColumn() > std::numeric_limits<std::uint32_t>::max() ||
      source_path.size() > serialized_limit - kSerializedRecordMetadataBytes ||
      source_function.size() >
          serialized_limit - kSerializedRecordMetadataBytes - source_path.size()) {
    return {};
  }

  const std::size_t source_path_offset = kSerializedRecordMetadataBytes;
  const std::size_t source_function_offset = source_path_offset + source_path.size();
  const std::size_t message_offset = source_function_offset + source_function.size();
  const SerializedRecordHeader header{
      .event_timestamp = timestamp,
      .source_path_offset = static_cast<std::uint32_t>(source_path_offset),
      .source_path_size = static_cast<std::uint32_t>(source_path.size()),
      .source_function_offset = static_cast<std::uint32_t>(source_function_offset),
      .source_function_size = static_cast<std::uint32_t>(source_function.size()),
      .message_offset = static_cast<std::uint32_t>(message_offset),
      .source_line = static_cast<std::uint32_t>(source.GetLine()),
      .source_column = static_cast<std::uint32_t>(source.GetColumn()),
      .level = level_value,
  };
  WriteObject(storage_.data(), 0, header);
  if (!source_path.empty()) {
    std::memcpy(storage_.data() + source_path_offset, source_path.data(), source_path.size());
  }
  if (!source_function.empty()) {
    std::memcpy(storage_.data() + source_function_offset, source_function.data(),
                source_function.size());
  }
  state_ = State::kBuilding;
  return RecordWriter{*this,
                      {.serialized_limit = serialized_limit,
                       .message_offset = message_offset,
                       .source_payload_bytes = source_path.size() + source_function.size()}};
}

void RecordSlot::Reset() noexcept { state_ = State::kReset; }

}  // namespace ulog::detail::producer::record

namespace ulog::detail::producer {
namespace {

[[nodiscard]] record::RecordWriter* Writer(void* writer) noexcept {
  return static_cast<record::RecordWriter*>(writer);
}

}  // namespace

RecordAppender::FormatOutputIterator& RecordAppender::FormatOutputIterator::operator=(
    char value) noexcept {
  if (appender_ != nullptr) {
    static_cast<void>(appender_->Append(std::string_view{&value, 1}));
  }
  return *this;
}

WriteResult RecordAppender::Append(std::string_view value) noexcept {
  return writer_ != nullptr
             ? Writer(writer_)->Append(value)
             : WriteResult{
                   .requested_bytes = value.size(), .stored_bytes = 0, .truncated = !value.empty()};
}

WriteResult RecordAppender::Append(std::int64_t value) noexcept {
  return writer_ != nullptr ? Writer(writer_)->Append(value) : WriteResult{.truncated = true};
}

WriteResult RecordAppender::Append(std::uint64_t value) noexcept {
  return writer_ != nullptr ? Writer(writer_)->Append(value) : WriteResult{.truncated = true};
}

WriteResult RecordAppender::Append(double value) noexcept {
  return writer_ != nullptr ? Writer(writer_)->Append(value) : WriteResult{.truncated = true};
}

WriteResult RecordAppender::Append(bool value) noexcept {
  return writer_ != nullptr ? Writer(writer_)->Append(value) : WriteResult{.truncated = true};
}

bool RecordAppender::AddField(std::string_view key, std::string_view value) noexcept {
  return writer_ != nullptr && Writer(writer_)->AddField(key, value);
}

bool RecordAppender::AddField(std::string_view key, std::int64_t value) noexcept {
  return writer_ != nullptr && Writer(writer_)->AddField(key, value);
}

bool RecordAppender::AddField(std::string_view key, std::uint64_t value) noexcept {
  return writer_ != nullptr && Writer(writer_)->AddField(key, value);
}

bool RecordAppender::AddField(std::string_view key, double value) noexcept {
  return writer_ != nullptr && Writer(writer_)->AddField(key, value);
}

bool RecordAppender::AddField(std::string_view key, bool value) noexcept {
  return writer_ != nullptr && Writer(writer_)->AddField(key, value);
}

bool RecordAppender::AddField(std::string_view key, NullValue value) noexcept {
  return writer_ != nullptr && Writer(writer_)->AddField(key, value);
}

FieldStatus RecordAppender::PutField(std::string_view key, const FieldValue& value,
                                     FieldMode mode) noexcept {
  return writer_ != nullptr ? Writer(writer_)->PutField(key, value, mode) : FieldStatus::kTruncated;
}

std::string_view FieldView::key() const noexcept {
  const auto field = record::Field(storage_, offset_);
  return record::Text(storage_, field.key_offset, field.key_size);
}

FieldKind FieldView::kind() const noexcept {
  return static_cast<FieldKind>(record::Field(storage_, offset_).kind);
}

std::optional<std::string_view> FieldView::AsString() const noexcept {
  const auto field = record::Field(storage_, offset_);
  if (static_cast<FieldKind>(field.kind) != FieldKind::kString) {
    return std::nullopt;
  }
  return record::Text(storage_, field.value_low, field.value_high);
}

std::optional<std::int64_t> FieldView::AsInt64() const noexcept {
  const auto field = record::Field(storage_, offset_);
  if (static_cast<FieldKind>(field.kind) != FieldKind::kInt64) {
    return std::nullopt;
  }
  const std::uint64_t bits = static_cast<std::uint64_t>(field.value_low) |
                             (static_cast<std::uint64_t>(field.value_high) << 32U);
  return std::bit_cast<std::int64_t>(bits);
}

std::optional<std::uint64_t> FieldView::AsUInt64() const noexcept {
  const auto field = record::Field(storage_, offset_);
  if (static_cast<FieldKind>(field.kind) != FieldKind::kUInt64) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(field.value_low) |
         (static_cast<std::uint64_t>(field.value_high) << 32U);
}

std::optional<double> FieldView::AsDouble() const noexcept {
  const auto field = record::Field(storage_, offset_);
  if (static_cast<FieldKind>(field.kind) != FieldKind::kDouble) {
    return std::nullopt;
  }
  const std::uint64_t bits = static_cast<std::uint64_t>(field.value_low) |
                             (static_cast<std::uint64_t>(field.value_high) << 32U);
  return std::bit_cast<double>(bits);
}

std::optional<bool> FieldView::AsBool() const noexcept {
  const auto field = record::Field(storage_, offset_);
  if (static_cast<FieldKind>(field.kind) != FieldKind::kBool) {
    return std::nullopt;
  }
  return (field.flags & record::kFieldBoolTrueFlag) != 0U;
}

bool FieldView::IsNull() const noexcept { return kind() == FieldKind::kNull; }

std::optional<std::string_view> FieldView::AsJson() const noexcept {
  const auto field = record::Field(storage_, offset_);
  if (static_cast<FieldKind>(field.kind) != FieldKind::kJson) {
    return std::nullopt;
  }
  return record::Text(storage_, field.value_low, field.value_high);
}

bool FieldView::IsFrozen() const noexcept {
  return (record::Field(storage_, offset_).flags & record::kFieldFrozenFlag) != 0U;
}

FieldValue FieldView::Value() const noexcept {
  const auto field = record::Field(storage_, offset_);
  const auto kind = static_cast<FieldKind>(field.kind);
  if (kind == FieldKind::kString || kind == FieldKind::kJson) {
    return FieldValue::FromParts(kind, record::Text(storage_, field.value_low, field.value_high),
                                 0U);
  }
  if (kind == FieldKind::kBool) {
    return FieldValue{(field.flags & record::kFieldBoolTrueFlag) != 0U};
  }
  return FieldValue::FromParts(kind, {},
                               static_cast<std::uint64_t>(field.value_low) |
                                   (static_cast<std::uint64_t>(field.value_high) << 32U));
}

std::optional<FieldView> FieldCursor::Next() noexcept {
  if (storage_ == nullptr || remaining_ == 0U || offset_ == 0U) {
    remaining_ = 0U;
    return std::nullopt;
  }
  const std::uint32_t current_offset = offset_;
  offset_ = record::Field(storage_, current_offset).next_offset;
  --remaining_;
  return FieldView{storage_, current_offset};
}

Level RecordView::level() const noexcept {
  return static_cast<Level>(record::Header(storage_).level);
}

EventTimestamp RecordView::event_timestamp() const noexcept {
  return record::Header(storage_).event_timestamp;
}

std::string_view RecordView::source_path() const noexcept {
  const auto header = record::Header(storage_);
  return record::Text(storage_, header.source_path_offset, header.source_path_size);
}

std::string_view RecordView::source_function() const noexcept {
  const auto header = record::Header(storage_);
  return record::Text(storage_, header.source_function_offset, header.source_function_size);
}

std::uint32_t RecordView::source_line() const noexcept {
  return record::Header(storage_).source_line;
}

std::uint32_t RecordView::source_column() const noexcept {
  return record::Header(storage_).source_column;
}

std::string_view RecordView::message() const noexcept {
  const auto header = record::Header(storage_);
  return record::Text(storage_, header.message_offset, header.message_size);
}

bool RecordView::truncated() const noexcept { return (record::Header(storage_).flags & 1U) != 0U; }

std::size_t RecordView::field_count() const noexcept {
  return record::Header(storage_).field_count;
}

FieldCursor RecordView::Fields() const noexcept {
  const auto header = record::Header(storage_);
  return FieldCursor{storage_, header.first_field_offset, header.field_count};
}

std::optional<FieldView> RecordView::FieldAt(std::size_t index) const noexcept {
  auto fields = Fields();
  if (index >= field_count()) {
    return std::nullopt;
  }
  for (std::size_t current = 0; current < index; ++current) {
    static_cast<void>(fields.Next());
  }
  return fields.Next();
}

}  // namespace ulog::detail::producer
