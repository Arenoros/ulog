#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include "support/workload_harness.hpp"

namespace ulog::benchmark_support::record_storage {

inline constexpr std::size_t kMaximumSerializedBytes = 16'384;
inline constexpr std::size_t kRecordSlotAlignmentBytes = 64;
inline constexpr std::size_t kSerializedRecordMetadataBytes = 48;
inline constexpr std::size_t kSerializedFieldMetadataBytes = 24;
inline constexpr std::size_t kMaximumStoredMessageBytes =
    kMaximumSerializedBytes - kSerializedRecordMetadataBytes;
inline constexpr std::size_t kMaximumRequestedMessageBytes =
    std::numeric_limits<std::size_t>::max();

namespace impl {

[[nodiscard]] constexpr std::size_t RoundUp(std::size_t value, std::size_t quantum) noexcept {
  return ((value + quantum - 1U) / quantum) * quantum;
}

[[nodiscard]] constexpr std::uint64_t ToUint64(std::size_t value) noexcept {
  if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
    return value > std::numeric_limits<std::uint64_t>::max()
               ? std::numeric_limits<std::uint64_t>::max()
               : static_cast<std::uint64_t>(value);
  } else {
    return static_cast<std::uint64_t>(value);
  }
}

[[nodiscard]] constexpr std::uint64_t SaturatingAdd(std::uint64_t left,
                                                    std::size_t right) noexcept {
  const std::uint64_t converted = ToUint64(right);
  return converted > std::numeric_limits<std::uint64_t>::max() - left
             ? std::numeric_limits<std::uint64_t>::max()
             : left + converted;
}

struct ContiguousStorage final {
  [[nodiscard]] std::span<std::byte> WritableSegment(std::size_t offset) noexcept {
    return std::span{bytes}.subspan(offset);
  }
  [[nodiscard]] std::span<const std::byte> ReadableSegment(std::size_t offset) const noexcept {
    return std::span{bytes}.subspan(offset);
  }
  std::array<std::byte, kMaximumSerializedBytes> bytes{};
};

struct ChunkedStorage final {
  static constexpr std::size_t kChunkBytes = 256;
  static constexpr std::size_t kChunkCount = kMaximumSerializedBytes / kChunkBytes;

  [[nodiscard]] std::span<std::byte> WritableSegment(std::size_t offset) noexcept {
    return std::span{chunks[offset / kChunkBytes]}.subspan(offset % kChunkBytes);
  }
  [[nodiscard]] std::span<const std::byte> ReadableSegment(std::size_t offset) const noexcept {
    return std::span{chunks[offset / kChunkBytes]}.subspan(offset % kChunkBytes);
  }
  std::array<std::array<std::byte, kChunkBytes>, kChunkCount> chunks{};
};

struct HybridStorage final {
  static constexpr std::size_t kInlineBytes = 512;
  static constexpr std::size_t kOverflowChunkBytes = 1'024;
  static constexpr std::size_t kOverflowChunkCount = 15;
  static constexpr std::size_t kTailBytes = 512;
  static constexpr std::size_t kTailOffset =
      kInlineBytes + kOverflowChunkBytes * kOverflowChunkCount;

  [[nodiscard]] std::span<std::byte> WritableSegment(std::size_t offset) noexcept {
    if (offset < kInlineBytes) {
      return std::span{inline_bytes}.subspan(offset);
    }
    if (offset < kTailOffset) {
      const std::size_t overflow_offset = offset - kInlineBytes;
      return std::span{overflow_chunks[overflow_offset / kOverflowChunkBytes]}.subspan(
          overflow_offset % kOverflowChunkBytes);
    }
    return std::span{tail_bytes}.subspan(offset - kTailOffset);
  }
  [[nodiscard]] std::span<const std::byte> ReadableSegment(std::size_t offset) const noexcept {
    if (offset < kInlineBytes) {
      return std::span{inline_bytes}.subspan(offset);
    }
    if (offset < kTailOffset) {
      const std::size_t overflow_offset = offset - kInlineBytes;
      return std::span{overflow_chunks[overflow_offset / kOverflowChunkBytes]}.subspan(
          overflow_offset % kOverflowChunkBytes);
    }
    return std::span{tail_bytes}.subspan(offset - kTailOffset);
  }
  std::array<std::byte, kInlineBytes> inline_bytes{};
  std::array<std::array<std::byte, kOverflowChunkBytes>, kOverflowChunkCount> overflow_chunks{};
  std::array<std::byte, kTailBytes> tail_bytes{};
};

static_assert(sizeof(ContiguousStorage) == kMaximumSerializedBytes);
static_assert(sizeof(ChunkedStorage) == kMaximumSerializedBytes);
static_assert(sizeof(HybridStorage) == kMaximumSerializedBytes);

template <typename Storage>
void CopyIn(Storage& storage, std::size_t offset, std::span<const std::byte> source) noexcept {
  while (!source.empty()) {
    auto segment = storage.WritableSegment(offset);
    const std::size_t copied = std::min(segment.size(), source.size());
    std::memcpy(segment.data(), source.data(), copied);
    offset += copied;
    source = source.subspan(copied);
  }
}

template <typename Storage>
void CopyOut(const Storage& storage, std::size_t offset,
             std::span<std::byte> destination) noexcept {
  while (!destination.empty()) {
    const auto segment = storage.ReadableSegment(offset);
    const std::size_t copied = std::min(segment.size(), destination.size());
    std::memcpy(destination.data(), segment.data(), copied);
    offset += copied;
    destination = destination.subspan(copied);
  }
}

template <typename Storage>
[[nodiscard]] std::byte ReadByte(const Storage& storage, std::size_t offset) noexcept {
  return storage.ReadableSegment(offset).front();
}

class StorageReader final {
 public:
  StorageReader() noexcept = default;

  template <typename Storage>
  [[nodiscard]] static StorageReader From(const Storage& storage) noexcept {
    return StorageReader{
        &storage,
        [](const void* object, std::size_t offset) noexcept {
          return impl::ReadByte(*static_cast<const Storage*>(object), offset);
        },
        [](const void* object, std::size_t offset, std::span<std::byte> destination) noexcept {
          impl::CopyOut(*static_cast<const Storage*>(object), offset, destination);
        }};
  }

  [[nodiscard]] explicit operator bool() const noexcept { return object_ != nullptr; }
  friend bool operator==(const StorageReader&, const StorageReader&) = default;
  [[nodiscard]] std::byte ReadByte(std::size_t offset) const noexcept {
    return read_byte_(object_, offset);
  }
  void CopyOut(std::size_t offset, std::span<std::byte> destination) const noexcept {
    copy_out_(object_, offset, destination);
  }

 private:
  using ReadByteFunction = std::byte (*)(const void*, std::size_t) noexcept;
  using CopyOutFunction = void (*)(const void*, std::size_t, std::span<std::byte>) noexcept;

  StorageReader(const void* object, ReadByteFunction read_byte, CopyOutFunction copy_out) noexcept
      : object_(object), read_byte_(read_byte), copy_out_(copy_out) {}

  const void* object_{nullptr};
  ReadByteFunction read_byte_{nullptr};
  CopyOutFunction copy_out_{nullptr};
};

struct SerializedRecordHeader final {
  std::int64_t event_timestamp;
  std::uint32_t source_path_offset;
  std::uint32_t source_path_size;
  std::uint32_t source_function_offset;
  std::uint32_t source_function_size;
  std::uint32_t message_offset;
  std::uint32_t message_size;
  std::uint32_t source_line;
  std::uint32_t first_field_offset;
  std::uint32_t field_count;
  std::uint32_t flags;
};

struct SerializedField final {
  std::uint32_t next_offset;
  std::uint32_t key_offset;
  std::uint32_t key_size;
  std::uint32_t value_low;
  std::uint32_t value_high;
  std::uint8_t kind;
  std::uint8_t flags;
  std::uint16_t reserved;
};

static_assert(sizeof(SerializedRecordHeader) == kSerializedRecordMetadataBytes);
static_assert(sizeof(SerializedField) == kSerializedFieldMetadataBytes);
static_assert(std::is_trivially_copyable_v<SerializedRecordHeader>);
static_assert(std::is_trivially_copyable_v<SerializedField>);

template <typename Storage, typename Value>
void WriteObject(Storage& storage, std::size_t offset, const Value& value) noexcept {
  static_assert(std::is_trivially_copyable_v<Value>);
  CopyIn(storage, offset, std::as_bytes(std::span{&value, std::size_t{1}}));
}

template <typename Value>
[[nodiscard]] Value ReadObject(StorageReader storage, std::size_t offset) noexcept {
  static_assert(std::is_trivially_copyable_v<Value>);
  Value value{};
  storage.CopyOut(offset, std::as_writable_bytes(std::span{&value, std::size_t{1}}));
  return value;
}

[[nodiscard]] inline bool IsContinuationByte(unsigned char value) noexcept {
  return (value & 0xC0U) == 0x80U;
}

[[nodiscard]] inline std::size_t ValidUtf8PrefixLength(std::span<const std::byte> value) noexcept {
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
    bool sequence_is_valid = true;
    for (std::size_t index = 1; index < sequence_size; ++index) {
      sequence_is_valid =
          sequence_is_valid &&
          IsContinuationByte(std::to_integer<unsigned char>(value[valid_size + index]));
    }
    if (!sequence_is_valid) {
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

[[nodiscard]] inline std::size_t ValidUtf8PrefixLength(StorageReader storage, std::size_t offset,
                                                       std::size_t size) noexcept {
  std::size_t valid_size = 0;
  while (valid_size < size) {
    const auto first = std::to_integer<unsigned char>(storage.ReadByte(offset + valid_size));
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
    if (sequence_size > size - valid_size) {
      break;
    }
    bool sequence_is_valid = true;
    for (std::size_t index = 1; index < sequence_size; ++index) {
      sequence_is_valid = sequence_is_valid && IsContinuationByte(std::to_integer<unsigned char>(
                                                   storage.ReadByte(offset + valid_size + index)));
    }
    if (!sequence_is_valid) {
      break;
    }
    if (sequence_size == 3) {
      const auto second =
          std::to_integer<unsigned char>(storage.ReadByte(offset + valid_size + 1U));
      if ((first == 0xE0U && second < 0xA0U) || (first == 0xEDU && second >= 0xA0U)) {
        break;
      }
    } else if (sequence_size == 4) {
      const auto second =
          std::to_integer<unsigned char>(storage.ReadByte(offset + valid_size + 1U));
      if ((first == 0xF0U && second < 0x90U) || (first == 0xF4U && second >= 0x90U)) {
        break;
      }
    }
    valid_size += sequence_size;
  }
  return valid_size;
}

}  // namespace impl

struct ContiguousPolicy final {
  using Storage = impl::ContiguousStorage;
  static constexpr std::size_t kMinimumAccountingChargeBytes = 64;
  [[nodiscard]] static constexpr std::size_t AccountingCharge(
      std::size_t serialized_bytes) noexcept {
    return impl::RoundUp(std::max(serialized_bytes, std::size_t{1}), kMinimumAccountingChargeBytes);
  }
};

struct ChunkedPolicy final {
  using Storage = impl::ChunkedStorage;
  static constexpr std::size_t kMinimumAccountingChargeBytes = 256;
  [[nodiscard]] static constexpr std::size_t AccountingCharge(
      std::size_t serialized_bytes) noexcept {
    return impl::RoundUp(std::max(serialized_bytes, std::size_t{1}), kMinimumAccountingChargeBytes);
  }
};

struct HybridPolicy final {
  using Storage = impl::HybridStorage;
  static constexpr std::size_t kMinimumAccountingChargeBytes = 512;
  static constexpr std::size_t kOverflowChunkBytes = 1'024;
  static constexpr std::size_t kLastFullOverflowChargeBytes = 15'872;
  [[nodiscard]] static constexpr std::size_t AccountingCharge(
      std::size_t serialized_bytes) noexcept {
    if (serialized_bytes <= kMinimumAccountingChargeBytes) {
      return kMinimumAccountingChargeBytes;
    }
    if (serialized_bytes <= kLastFullOverflowChargeBytes) {
      return kMinimumAccountingChargeBytes +
             impl::RoundUp(serialized_bytes - kMinimumAccountingChargeBytes, kOverflowChunkBytes);
    }
    return kMaximumSerializedBytes;
  }
};

struct RecordSeed final {
  std::string_view source_path;
  std::string_view source_function;
  std::uint32_t source_line{0};
  std::int64_t event_timestamp{0};
};

inline constexpr std::string_view kBenchmarkSourcePath = "benchmarks/record_storage_benchmark.cpp";
inline constexpr std::string_view kBenchmarkSourceFunction = "TryProduce";
inline constexpr std::uint32_t kBenchmarkSourceLine = 91;
inline constexpr std::int64_t kBenchmarkEventTimestamp = 1'704'067'200'123'456;
inline constexpr std::string_view kBenchmarkStringFieldKey = "kind";
inline constexpr std::string_view kBenchmarkStringFieldValue = "benchmark";
inline constexpr std::string_view kBenchmarkInt64FieldKey = "signed";
inline constexpr std::string_view kBenchmarkUInt64FieldKey = "unsigned";
inline constexpr std::string_view kBenchmarkDoubleFieldKey = "ratio";
inline constexpr std::string_view kBenchmarkBoolFieldKey = "sampled";
inline constexpr std::string_view kBenchmarkNullFieldKey = "optional";
inline constexpr std::size_t kBenchmarkFieldCount = 6;
inline constexpr std::size_t kBenchmarkFixedMetadataBytes =
    kSerializedRecordMetadataBytes + kBenchmarkFieldCount * kSerializedFieldMetadataBytes;
inline constexpr std::size_t kBenchmarkFixedPayloadBytes =
    kBenchmarkSourcePath.size() + kBenchmarkSourceFunction.size() +
    kBenchmarkStringFieldKey.size() + kBenchmarkStringFieldValue.size() +
    kBenchmarkInt64FieldKey.size() + kBenchmarkUInt64FieldKey.size() +
    kBenchmarkDoubleFieldKey.size() + kBenchmarkBoolFieldKey.size() + kBenchmarkNullFieldKey.size();
inline constexpr std::size_t kMaximumBenchmarkStoredMessageBytes =
    kMaximumSerializedBytes - kBenchmarkFixedMetadataBytes - kBenchmarkFixedPayloadBytes;

[[nodiscard]] constexpr RecordSeed BenchmarkRecordSeed() noexcept {
  return RecordSeed{.source_path = kBenchmarkSourcePath,
                    .source_function = kBenchmarkSourceFunction,
                    .source_line = kBenchmarkSourceLine,
                    .event_timestamp = kBenchmarkEventTimestamp};
}

template <typename Policy>
[[nodiscard]] constexpr RecordFootprint MakeBenchmarkRecordFootprint(
    std::size_t requested_message_bytes, std::size_t stored_message_bytes) noexcept {
  const std::size_t owned_payload_bytes = kBenchmarkFixedPayloadBytes + stored_message_bytes;
  const std::size_t serialized_bytes = kBenchmarkFixedMetadataBytes + owned_payload_bytes;
  const std::size_t accounting_charge_bytes = Policy::AccountingCharge(serialized_bytes);
  return RecordFootprint{
      .requested_message_bytes = impl::ToUint64(requested_message_bytes),
      .stored_message_bytes = impl::ToUint64(stored_message_bytes),
      .owned_payload_bytes = impl::ToUint64(owned_payload_bytes),
      .metadata_bytes = kBenchmarkFixedMetadataBytes,
      .fragmentation_bytes = impl::ToUint64(accounting_charge_bytes - serialized_bytes),
      .accounting_charge_bytes = impl::ToUint64(accounting_charge_bytes),
      .minimum_accounting_charge_bytes = Policy::kMinimumAccountingChargeBytes,
      .truncated = stored_message_bytes != requested_message_bytes};
}

template <typename Policy>
[[nodiscard]] constexpr RecordFootprint BenchmarkAsciiRecordShape(
    std::size_t requested_message_bytes) noexcept {
  return MakeBenchmarkRecordFootprint<Policy>(
      requested_message_bytes,
      std::min(requested_message_bytes, kMaximumBenchmarkStoredMessageBytes));
}

template <typename Policy>
[[nodiscard]] constexpr RecordFootprint BenchmarkRecordShape(
    std::size_t requested_message_bytes) noexcept {
  return BenchmarkAsciiRecordShape<Policy>(requested_message_bytes);
}

template <typename Policy>
[[nodiscard]] constexpr RecordFootprint DescribeRecord(
    std::size_t requested_ascii_message_bytes) noexcept {
  return BenchmarkAsciiRecordShape<Policy>(requested_ascii_message_bytes);
}

template <typename Policy>
[[nodiscard]] RecordFootprint DescribeRecord(std::span<const std::byte> message) noexcept {
  const std::size_t inspected_size = std::min(message.size(), kMaximumBenchmarkStoredMessageBytes);
  const std::size_t stored_size = impl::ValidUtf8PrefixLength(message.first(inspected_size));
  return MakeBenchmarkRecordFootprint<Policy>(message.size(), stored_size);
}

enum class FieldKind : std::uint8_t { kString, kInt64, kUInt64, kDouble, kBool, kNull };
struct NullValue final {};
inline constexpr NullValue kNull{};

struct WriteResult final {
  std::size_t requested_bytes;
  std::size_t stored_bytes;
  bool truncated;
};

class TextView final {
 public:
  class Iterator final {
   public:
    using value_type = std::byte;
    using difference_type = std::ptrdiff_t;
    using iterator_category = std::forward_iterator_tag;

    [[nodiscard]] std::byte operator*() const noexcept { return storage_.ReadByte(offset_); }
    Iterator& operator++() noexcept {
      ++offset_;
      return *this;
    }
    Iterator operator++(int) noexcept {
      Iterator previous = *this;
      ++*this;
      return previous;
    }
    friend bool operator==(const Iterator&, const Iterator&) = default;

   private:
    friend class TextView;
    Iterator(impl::StorageReader storage, std::size_t offset) noexcept
        : storage_(storage), offset_(offset) {}
    impl::StorageReader storage_;
    std::size_t offset_{0};
  };

  TextView() noexcept = default;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] Iterator begin() const noexcept { return Iterator{storage_, offset_}; }
  [[nodiscard]] Iterator end() const noexcept { return Iterator{storage_, offset_ + size_}; }

  [[nodiscard]] std::size_t CopyTo(std::span<std::byte> destination) const noexcept {
    const std::size_t copied = std::min(size_, destination.size());
    if (copied != 0) {
      storage_.CopyOut(offset_, destination.first(copied));
    }
    return copied;
  }
  [[nodiscard]] bool Equals(std::span<const std::byte> value) const noexcept {
    if (value.size() != size_) {
      return false;
    }
    for (std::size_t index = 0; index < size_; ++index) {
      if (storage_.ReadByte(offset_ + index) != value[index]) {
        return false;
      }
    }
    return true;
  }
  [[nodiscard]] bool Equals(std::string_view value) const noexcept {
    return Equals(
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(value.data()), value.size()});
  }
  [[nodiscard]] bool Equals(TextView value) const noexcept {
    if (value.size_ != size_) {
      return false;
    }
    for (std::size_t index = 0; index < size_; ++index) {
      if (storage_.ReadByte(offset_ + index) != value.storage_.ReadByte(value.offset_ + index)) {
        return false;
      }
    }
    return true;
  }

 private:
  friend class FieldView;
  friend class RecordView;
  TextView(impl::StorageReader storage, std::size_t offset, std::size_t size) noexcept
      : storage_(storage), offset_(offset), size_(size) {}
  impl::StorageReader storage_;
  std::size_t offset_{0};
  std::size_t size_{0};
};

class FieldView final {
 public:
  [[nodiscard]] TextView key() const noexcept {
    return TextView{storage_, field_.key_offset, field_.key_size};
  }
  [[nodiscard]] FieldKind kind() const noexcept { return static_cast<FieldKind>(field_.kind); }
  [[nodiscard]] std::optional<TextView> AsString() const noexcept {
    if (kind() != FieldKind::kString) {
      return std::nullopt;
    }
    return TextView{storage_, field_.value_low, field_.value_high};
  }
  [[nodiscard]] std::optional<std::int64_t> AsInt64() const noexcept {
    return ReadScalar<std::int64_t>(FieldKind::kInt64);
  }
  [[nodiscard]] std::optional<std::uint64_t> AsUInt64() const noexcept {
    return ReadScalar<std::uint64_t>(FieldKind::kUInt64);
  }
  [[nodiscard]] std::optional<double> AsDouble() const noexcept {
    return ReadScalar<double>(FieldKind::kDouble);
  }
  [[nodiscard]] std::optional<bool> AsBool() const noexcept {
    if (kind() != FieldKind::kBool) {
      return std::nullopt;
    }
    return field_.flags != 0U;
  }
  [[nodiscard]] bool IsNull() const noexcept { return kind() == FieldKind::kNull; }

 private:
  friend class RecordView;
  FieldView(impl::StorageReader storage, impl::SerializedField field) noexcept
      : storage_(storage), field_(field) {}

  template <typename Value>
  [[nodiscard]] std::optional<Value> ReadScalar(FieldKind expected_kind) const noexcept {
    if (kind() != expected_kind) {
      return std::nullopt;
    }
    const std::uint64_t bits = static_cast<std::uint64_t>(field_.value_low) |
                               (static_cast<std::uint64_t>(field_.value_high) << 32U);
    return std::bit_cast<Value>(bits);
  }
  impl::StorageReader storage_;
  impl::SerializedField field_;
};

class RecordView final {
 public:
  RecordView() noexcept = default;
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(storage_); }
  [[nodiscard]] TextView message() const noexcept {
    const auto header = Header();
    return TextView{storage_, header.message_offset, header.message_size};
  }
  [[nodiscard]] TextView source_path() const noexcept {
    const auto header = Header();
    return TextView{storage_, header.source_path_offset, header.source_path_size};
  }
  [[nodiscard]] TextView source_function() const noexcept {
    const auto header = Header();
    return TextView{storage_, header.source_function_offset, header.source_function_size};
  }
  [[nodiscard]] std::uint32_t source_line() const noexcept { return Header().source_line; }
  [[nodiscard]] std::int64_t event_timestamp() const noexcept { return Header().event_timestamp; }
  [[nodiscard]] std::size_t field_count() const noexcept { return Header().field_count; }
  [[nodiscard]] std::optional<FieldView> FieldAt(std::size_t index) const noexcept {
    if (!storage_) {
      return std::nullopt;
    }
    const auto header = Header();
    if (index >= header.field_count) {
      return std::nullopt;
    }
    std::uint32_t field_offset = header.first_field_offset;
    for (std::size_t current = 0; current < index; ++current) {
      field_offset = impl::ReadObject<impl::SerializedField>(storage_, field_offset).next_offset;
    }
    return FieldView{storage_, impl::ReadObject<impl::SerializedField>(storage_, field_offset)};
  }
  [[nodiscard]] const RecordFootprint& footprint() const noexcept { return footprint_; }

 private:
  template <typename Policy>
  friend class RecordSlot;
  RecordView(impl::StorageReader storage, RecordFootprint footprint) noexcept
      : storage_(storage), footprint_(footprint) {}
  [[nodiscard]] impl::SerializedRecordHeader Header() const noexcept {
    return storage_ ? impl::ReadObject<impl::SerializedRecordHeader>(storage_, 0)
                    : impl::SerializedRecordHeader{};
  }
  impl::StorageReader storage_;
  RecordFootprint footprint_{};
};

template <typename Policy>
class alignas(kRecordSlotAlignmentBytes) RecordSlot final {
 public:
  class Writer final {
   public:
    class FormatOutputIterator final {
     public:
      using value_type = void;
      using difference_type = std::ptrdiff_t;
      using pointer = void;
      using reference = void;
      using iterator_category = std::output_iterator_tag;

      FormatOutputIterator& operator=(char value) noexcept {
        writer_->AppendFormatByte(value);
        return *this;
      }
      [[nodiscard]] FormatOutputIterator& operator*() noexcept { return *this; }
      FormatOutputIterator& operator++() noexcept { return *this; }
      FormatOutputIterator operator++(int) noexcept { return *this; }

     private:
      friend class Writer;
      explicit FormatOutputIterator(Writer& writer) noexcept : writer_(&writer) {}
      Writer* writer_;
    };

    Writer(Writer&& other) noexcept { MoveFrom(other); }
    Writer& operator=(Writer&& other) noexcept {
      if (this != &other) {
        MoveFrom(other);
      }
      return *this;
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    [[nodiscard]] explicit operator bool() const noexcept { return slot_ != nullptr; }

    [[nodiscard]] WriteResult Append(std::span<const std::byte> value) noexcept {
      return AppendBytes(value);
    }
    [[nodiscard]] WriteResult Append(std::string_view value) noexcept {
      return AppendBytes(std::span<const std::byte>{
          reinterpret_cast<const std::byte*>(value.data()), value.size()});
    }
    template <std::size_t Size>
    [[nodiscard]] WriteResult Append(const char (&value)[Size]) noexcept {
      static_assert(Size != 0);
      return Append(std::string_view{value, Size - 1U});
    }
    [[nodiscard]] WriteResult Append(std::int64_t value) noexcept { return AppendNumber(value); }
    [[nodiscard]] WriteResult Append(std::uint64_t value) noexcept { return AppendNumber(value); }
    [[nodiscard]] WriteResult Append(double value) noexcept {
      std::array<char, 64> buffer{};
      const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                        std::chars_format::general);
      if (result.ec != std::errc{}) {
        return WriteResult{.requested_bytes = 0, .stored_bytes = 0, .truncated = true};
      }
      return Append(
          std::string_view{buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
    }
    [[nodiscard]] WriteResult Append(bool value) noexcept {
      return Append(value ? std::string_view{"true"} : std::string_view{"false"});
    }
    [[nodiscard]] FormatOutputIterator FormatOutput() noexcept {
      return FormatOutputIterator{*this};
    }

    [[nodiscard]] bool AddField(std::string_view key, std::string_view value) noexcept {
      return AddFieldBytes(key, FieldKind::kString,
                           std::span<const std::byte>{
                               reinterpret_cast<const std::byte*>(value.data()), value.size()},
                           0, false);
    }
    template <std::size_t Size>
    [[nodiscard]] bool AddField(std::string_view key, const char (&value)[Size]) noexcept {
      static_assert(Size != 0);
      return AddField(key, std::string_view{value, Size - 1U});
    }
    [[nodiscard]] bool AddField(std::string_view key, std::int64_t value) noexcept {
      return AddScalarField(key, FieldKind::kInt64, std::bit_cast<std::uint64_t>(value));
    }
    [[nodiscard]] bool AddField(std::string_view key, std::uint64_t value) noexcept {
      return AddScalarField(key, FieldKind::kUInt64, value);
    }
    [[nodiscard]] bool AddField(std::string_view key, double value) noexcept {
      return AddScalarField(key, FieldKind::kDouble, std::bit_cast<std::uint64_t>(value));
    }
    [[nodiscard]] bool AddField(std::string_view key, bool value) noexcept {
      return AddFieldBytes(key, FieldKind::kBool, {}, 0, value);
    }
    [[nodiscard]] bool AddField(std::string_view key, NullValue) noexcept {
      return AddFieldBytes(key, FieldKind::kNull, {}, 0, false);
    }

    [[nodiscard]] RecordView Publish() && noexcept {
      if (slot_ == nullptr || slot_->state_ != State::kBuilding) {
        return {};
      }
      const auto reader = impl::StorageReader::From(slot_->storage_);
      message_size_ = impl::ValidUtf8PrefixLength(reader, message_offset_, message_size_);
      auto header = impl::ReadObject<impl::SerializedRecordHeader>(reader, 0);
      header.message_size = static_cast<std::uint32_t>(message_size_);
      header.first_field_offset = first_field_offset_;
      header.field_count = field_count_;
      const bool truncated = requested_message_bytes_ != impl::ToUint64(message_size_);
      header.flags = truncated ? 1U : 0U;
      impl::WriteObject(slot_->storage_, 0, header);

      const std::uint64_t owned_payload_bytes =
          impl::ToUint64(source_payload_bytes_ + message_size_ + field_payload_bytes_);
      const std::uint64_t metadata_bytes =
          impl::ToUint64(kSerializedRecordMetadataBytes +
                         static_cast<std::size_t>(field_count_) * kSerializedFieldMetadataBytes);
      const std::uint64_t serialized_bytes = owned_payload_bytes + metadata_bytes;
      const std::uint64_t accounting_charge_bytes =
          Policy::AccountingCharge(static_cast<std::size_t>(serialized_bytes));
      if (accounting_charge_bytes > accounting_charge_limit_bytes_) {
        slot_->Reset();
        slot_ = nullptr;
        return {};
      }
      const RecordFootprint footprint{
          .requested_message_bytes = requested_message_bytes_,
          .stored_message_bytes = impl::ToUint64(message_size_),
          .owned_payload_bytes = owned_payload_bytes,
          .metadata_bytes = metadata_bytes,
          .fragmentation_bytes = accounting_charge_bytes - serialized_bytes,
          .accounting_charge_bytes = accounting_charge_bytes,
          .minimum_accounting_charge_bytes = Policy::kMinimumAccountingChargeBytes,
          .truncated = truncated};
      slot_->state_ = State::kPublished;
      slot_ = nullptr;
      return RecordView{reader, footprint};
    }

   private:
    friend class RecordSlot;
    Writer() noexcept = default;
    Writer(RecordSlot& slot, std::size_t serialized_limit, std::size_t message_offset,
           std::size_t source_payload_bytes, std::uint64_t accounting_charge_limit_bytes) noexcept
        : slot_(&slot),
          front_end_(message_offset),
          field_tail_(serialized_limit),
          message_offset_(message_offset),
          source_payload_bytes_(source_payload_bytes),
          accounting_charge_limit_bytes_(accounting_charge_limit_bytes) {}

    void MoveFrom(Writer& other) noexcept {
      slot_ = std::exchange(other.slot_, nullptr);
      front_end_ = other.front_end_;
      field_tail_ = other.field_tail_;
      message_offset_ = other.message_offset_;
      message_size_ = other.message_size_;
      requested_message_bytes_ = other.requested_message_bytes_;
      source_payload_bytes_ = other.source_payload_bytes_;
      field_payload_bytes_ = other.field_payload_bytes_;
      field_count_ = other.field_count_;
      first_field_offset_ = other.first_field_offset_;
      last_field_offset_ = other.last_field_offset_;
      accounting_charge_limit_bytes_ = other.accounting_charge_limit_bytes_;
    }

    [[nodiscard]] WriteResult AppendBytes(std::span<const std::byte> value) noexcept {
      requested_message_bytes_ = impl::SaturatingAdd(requested_message_bytes_, value.size());
      if (slot_ == nullptr) {
        return WriteResult{
            .requested_bytes = value.size(), .stored_bytes = 0, .truncated = !value.empty()};
      }
      const std::size_t stored = std::min(value.size(), field_tail_ - front_end_);
      impl::CopyIn(slot_->storage_, front_end_, value.first(stored));
      front_end_ += stored;
      message_size_ += stored;
      return WriteResult{.requested_bytes = value.size(),
                         .stored_bytes = stored,
                         .truncated = stored != value.size()};
    }

    void AppendFormatByte(char value) noexcept {
      const std::byte byte = static_cast<std::byte>(static_cast<unsigned char>(value));
      static_cast<void>(AppendBytes(std::span{&byte, std::size_t{1}}));
    }

    template <typename Value>
    [[nodiscard]] WriteResult AppendNumber(Value value) noexcept {
      std::array<char, std::numeric_limits<Value>::digits10 + 3U> buffer{};
      const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
      if (result.ec != std::errc{}) {
        return WriteResult{.requested_bytes = 0, .stored_bytes = 0, .truncated = true};
      }
      return Append(
          std::string_view{buffer.data(), static_cast<std::size_t>(result.ptr - buffer.data())});
    }

    [[nodiscard]] bool AddScalarField(std::string_view key, FieldKind kind,
                                      std::uint64_t bits) noexcept {
      return AddFieldBytes(key, kind, {}, bits, false);
    }

    [[nodiscard]] bool AddFieldBytes(std::string_view key, FieldKind kind,
                                     std::span<const std::byte> value, std::uint64_t scalar_bits,
                                     bool bool_value) noexcept {
      if (slot_ == nullptr || field_count_ == std::numeric_limits<std::uint32_t>::max() ||
          key.size() > std::numeric_limits<std::uint32_t>::max() ||
          value.size() > std::numeric_limits<std::uint32_t>::max() ||
          key.size() > std::numeric_limits<std::size_t>::max() - kSerializedFieldMetadataBytes ||
          value.size() > std::numeric_limits<std::size_t>::max() - kSerializedFieldMetadataBytes -
                             key.size()) {
        return false;
      }
      const std::size_t field_size = kSerializedFieldMetadataBytes + key.size() + value.size();
      if (field_size > field_tail_ - front_end_) {
        return false;
      }

      field_tail_ -= field_size;
      const auto field_offset = static_cast<std::uint32_t>(field_tail_);
      const auto key_offset =
          static_cast<std::uint32_t>(field_tail_ + kSerializedFieldMetadataBytes);
      const auto value_offset = static_cast<std::uint32_t>(key_offset + key.size());
      impl::CopyIn(
          slot_->storage_, key_offset,
          std::span<const std::byte>{reinterpret_cast<const std::byte*>(key.data()), key.size()});
      impl::CopyIn(slot_->storage_, value_offset, value);

      const impl::SerializedField field{
          .next_offset = 0,
          .key_offset = key_offset,
          .key_size = static_cast<std::uint32_t>(key.size()),
          .value_low = value.empty() ? static_cast<std::uint32_t>(scalar_bits) : value_offset,
          .value_high = value.empty() ? static_cast<std::uint32_t>(scalar_bits >> 32U)
                                      : static_cast<std::uint32_t>(value.size()),
          .kind = static_cast<std::uint8_t>(kind),
          .flags = bool_value ? std::uint8_t{1} : std::uint8_t{0},
          .reserved = 0};
      impl::WriteObject(slot_->storage_, field_offset, field);

      if (last_field_offset_ == 0U) {
        first_field_offset_ = field_offset;
      } else {
        const auto reader = impl::StorageReader::From(slot_->storage_);
        auto previous = impl::ReadObject<impl::SerializedField>(reader, last_field_offset_);
        previous.next_offset = field_offset;
        impl::WriteObject(slot_->storage_, last_field_offset_, previous);
      }
      last_field_offset_ = field_offset;
      ++field_count_;
      field_payload_bytes_ += key.size() + value.size();
      return true;
    }

    RecordSlot* slot_{nullptr};
    std::size_t front_end_{0};
    std::size_t field_tail_{0};
    std::size_t message_offset_{0};
    std::size_t message_size_{0};
    std::uint64_t requested_message_bytes_{0};
    std::size_t source_payload_bytes_{0};
    std::size_t field_payload_bytes_{0};
    std::uint32_t field_count_{0};
    std::uint32_t first_field_offset_{0};
    std::uint32_t last_field_offset_{0};
    std::uint64_t accounting_charge_limit_bytes_{0};
  };

  RecordSlot() noexcept = default;
  RecordSlot(const RecordSlot&) = delete;
  RecordSlot& operator=(const RecordSlot&) = delete;
  RecordSlot(RecordSlot&&) = delete;
  RecordSlot& operator=(RecordSlot&&) = delete;

  [[nodiscard]] Writer Begin(RecordSeed seed, const RecordFootprint& footprint) noexcept {
    if (state_ != State::kReset || !IsValidFootprint(footprint) ||
        seed.source_path.size() > std::numeric_limits<std::uint32_t>::max() ||
        seed.source_function.size() > std::numeric_limits<std::uint32_t>::max() ||
        seed.source_path.size() >
            std::numeric_limits<std::size_t>::max() - seed.source_function.size()) {
      return {};
    }
    const std::size_t serialized_limit = static_cast<std::size_t>(footprint.SerializedBytes());
    const std::size_t source_size = seed.source_path.size() + seed.source_function.size();
    if (source_size > serialized_limit - kSerializedRecordMetadataBytes) {
      return {};
    }

    const std::size_t path_offset = kSerializedRecordMetadataBytes;
    const std::size_t function_offset = path_offset + seed.source_path.size();
    const std::size_t message_offset = function_offset + seed.source_function.size();
    impl::CopyIn(
        storage_, path_offset,
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(seed.source_path.data()),
                                   seed.source_path.size()});
    impl::CopyIn(
        storage_, function_offset,
        std::span<const std::byte>{reinterpret_cast<const std::byte*>(seed.source_function.data()),
                                   seed.source_function.size()});

    const impl::SerializedRecordHeader header{
        .event_timestamp = seed.event_timestamp,
        .source_path_offset = static_cast<std::uint32_t>(path_offset),
        .source_path_size = static_cast<std::uint32_t>(seed.source_path.size()),
        .source_function_offset = static_cast<std::uint32_t>(function_offset),
        .source_function_size = static_cast<std::uint32_t>(seed.source_function.size()),
        .message_offset = static_cast<std::uint32_t>(message_offset),
        .message_size = 0,
        .source_line = seed.source_line,
        .first_field_offset = 0,
        .field_count = 0,
        .flags = 0};
    impl::WriteObject(storage_, 0, header);
    state_ = State::kBuilding;
    return Writer{*this, serialized_limit, message_offset, source_size,
                  footprint.accounting_charge_bytes};
  }

  void Reset() noexcept { state_ = State::kReset; }
  [[nodiscard]] bool is_published() const noexcept { return state_ == State::kPublished; }

 private:
  enum class State : std::uint8_t { kReset, kBuilding, kPublished };

  [[nodiscard]] static bool IsValidFootprint(const RecordFootprint& footprint) noexcept {
    if (footprint.owned_payload_bytes > kMaximumSerializedBytes ||
        footprint.metadata_bytes > kMaximumSerializedBytes ||
        footprint.owned_payload_bytes > kMaximumSerializedBytes - footprint.metadata_bytes) {
      return false;
    }
    const std::uint64_t serialized_bytes = footprint.owned_payload_bytes + footprint.metadata_bytes;
    if (footprint.accounting_charge_bytes > kMaximumSerializedBytes ||
        serialized_bytes < kSerializedRecordMetadataBytes ||
        serialized_bytes > footprint.accounting_charge_bytes ||
        footprint.minimum_accounting_charge_bytes != Policy::kMinimumAccountingChargeBytes) {
      return false;
    }
    const auto serialized_size = static_cast<std::size_t>(serialized_bytes);
    return footprint.accounting_charge_bytes == Policy::AccountingCharge(serialized_size) &&
           footprint.fragmentation_bytes == footprint.accounting_charge_bytes - serialized_bytes;
  }

  typename Policy::Storage storage_{};
  State state_{State::kReset};
  std::array<std::byte, kRecordSlotAlignmentBytes - sizeof(State)> state_cache_line_padding_{};
};

template <typename Writer>
[[nodiscard]] bool AddBenchmarkFields(
    Writer& writer, std::string_view string_key = kBenchmarkStringFieldKey,
    std::string_view string_value = kBenchmarkStringFieldValue) noexcept {
  bool fields_stored = writer.AddField(string_key, string_value);
  fields_stored = writer.AddField(kBenchmarkInt64FieldKey, std::int64_t{-7}) && fields_stored;
  fields_stored = writer.AddField(kBenchmarkUInt64FieldKey, std::uint64_t{42}) && fields_stored;
  fields_stored = writer.AddField(kBenchmarkDoubleFieldKey, 1.25) && fields_stored;
  fields_stored = writer.AddField(kBenchmarkBoolFieldKey, true) && fields_stored;
  fields_stored = writer.AddField(kBenchmarkNullFieldKey, kNull) && fields_stored;
  return fields_stored;
}

template <typename Policy>
[[nodiscard]] RecordView BuildBenchmarkRecord(typename RecordSlot<Policy>::Writer&& writer,
                                              std::span<const std::byte> message) noexcept {
  const bool fields_stored = AddBenchmarkFields(writer);

  const std::string_view format_argument{reinterpret_cast<const char*>(message.data()),
                                         message.size()};
  static_cast<void>(fmt::format_to(writer.FormatOutput(), "{}", format_argument));
  if (!fields_stored) {
    return {};
  }
  return std::move(writer).Publish();
}

using ContiguousRecordSlot = RecordSlot<ContiguousPolicy>;
using ChunkedRecordSlot = RecordSlot<ChunkedPolicy>;
using HybridRecordSlot = RecordSlot<HybridPolicy>;

static_assert(alignof(ContiguousRecordSlot) == kRecordSlotAlignmentBytes);
static_assert(alignof(ChunkedRecordSlot) == kRecordSlotAlignmentBytes);
static_assert(alignof(HybridRecordSlot) == kRecordSlotAlignmentBytes);
static_assert(sizeof(ContiguousRecordSlot) == kMaximumSerializedBytes + kRecordSlotAlignmentBytes);
static_assert(sizeof(ChunkedRecordSlot) == kMaximumSerializedBytes + kRecordSlotAlignmentBytes);
static_assert(sizeof(HybridRecordSlot) == kMaximumSerializedBytes + kRecordSlotAlignmentBytes);
static_assert(!std::is_copy_constructible_v<ContiguousRecordSlot::Writer>);
static_assert(std::is_nothrow_move_constructible_v<ContiguousRecordSlot::Writer>);
static_assert(std::is_nothrow_move_assignable_v<ContiguousRecordSlot::Writer>);

}  // namespace ulog::benchmark_support::record_storage
