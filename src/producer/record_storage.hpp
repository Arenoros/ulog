#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::producer::record {

inline constexpr std::size_t kSerializedRecordMetadataBytes = 56;
inline constexpr std::size_t kSerializedFieldMetadataBytes = 24;
inline constexpr std::string_view kTruncatedFieldKey = "ulog.truncated";

struct SerializedRecordHeader final {
  EventTimestamp event_timestamp{0};
  std::uint32_t source_path_offset{0};
  std::uint32_t source_path_size{0};
  std::uint32_t source_function_offset{0};
  std::uint32_t source_function_size{0};
  std::uint32_t message_offset{0};
  std::uint32_t message_size{0};
  std::uint32_t source_line{0};
  std::uint32_t source_column{0};
  std::uint32_t first_field_offset{0};
  std::uint32_t field_count{0};
  std::uint32_t flags{0};
  std::uint8_t level{0};
  std::array<std::byte, 3> reserved{};
};

struct SerializedField final {
  std::uint32_t next_offset{0};
  std::uint32_t key_offset{0};
  std::uint32_t key_size{0};
  std::uint32_t value_low{0};
  std::uint32_t value_high{0};
  std::uint8_t kind{0};
  std::uint8_t flags{0};
  std::uint16_t reserved{0};
};

static_assert(sizeof(SerializedRecordHeader) == kSerializedRecordMetadataBytes);
static_assert(sizeof(SerializedField) == kSerializedFieldMetadataBytes);

struct StoredRecord final {
  const std::byte* storage{nullptr};
  std::size_t serialized_bytes{0};
  std::size_t accounting_charge_bytes{0};
  bool truncated{false};

  [[nodiscard]] explicit operator bool() const noexcept { return storage != nullptr; }
};

class RecordSlot;

class RecordWriter final {
 public:
  RecordWriter() noexcept = default;
  RecordWriter(RecordWriter&& other) noexcept;
  RecordWriter& operator=(RecordWriter&& other) noexcept;
  RecordWriter(const RecordWriter&) = delete;
  RecordWriter& operator=(const RecordWriter&) = delete;
  ~RecordWriter();

  [[nodiscard]] explicit operator bool() const noexcept { return slot_ != nullptr; }
  [[nodiscard]] WriteResult Append(std::string_view value) noexcept;
  template <std::size_t Size>
  [[nodiscard]] WriteResult Append(const char (&value)[Size]) noexcept {
    static_assert(Size != 0);
    return Append(std::string_view{value, Size - 1U});
  }
  [[nodiscard]] WriteResult Append(std::int64_t value) noexcept;
  [[nodiscard]] WriteResult Append(std::uint64_t value) noexcept;
  [[nodiscard]] WriteResult Append(double value) noexcept;
  [[nodiscard]] WriteResult Append(bool value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, std::string_view value) noexcept;
  template <std::size_t Size>
  [[nodiscard]] bool AddField(std::string_view key, const char (&value)[Size]) noexcept {
    static_assert(Size != 0);
    return AddField(key, std::string_view{value, Size - 1U});
  }
  [[nodiscard]] bool AddField(std::string_view key, std::int64_t value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, std::uint64_t value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, double value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, bool value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, NullValue) noexcept;

  [[nodiscard]] StoredRecord Publish() && noexcept;
  void Abandon() noexcept;

 private:
  friend class RecordSlot;
  struct InitialState final {
    std::size_t serialized_limit{0};
    std::size_t message_offset{0};
    std::size_t source_payload_bytes{0};
  };

  RecordWriter(RecordSlot& slot, InitialState initial_state) noexcept;

  void MoveFrom(RecordWriter& other) noexcept;
  [[nodiscard]] WriteResult AppendBytes(std::span<const std::byte> value) noexcept;
  template <typename Value>
  [[nodiscard]] WriteResult AppendNumber(Value value) noexcept;
  [[nodiscard]] bool AddScalarField(std::string_view key, FieldKind kind,
                                    std::uint64_t bits) noexcept;
  [[nodiscard]] bool AddFieldBytes(std::string_view key, FieldKind kind,
                                   std::span<const std::byte> value, std::uint64_t scalar_bits,
                                   bool bool_value) noexcept;
  [[nodiscard]] bool AddTruncatedField() noexcept;

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
};

class alignas(kAccountingQuantumBytes) RecordSlot final {
 public:
  RecordSlot() noexcept = default;
  RecordSlot(const RecordSlot&) = delete;
  RecordSlot& operator=(const RecordSlot&) = delete;
  RecordSlot(RecordSlot&&) = delete;
  RecordSlot& operator=(RecordSlot&&) = delete;

  [[nodiscard]] RecordWriter Begin(std::size_t serialized_limit, Level level,
                                   const SourceLocation& source, EventTimestamp timestamp) noexcept;
  void Reset() noexcept;

  [[nodiscard]] std::byte* data() noexcept { return storage_.data(); }
  [[nodiscard]] const std::byte* data() const noexcept { return storage_.data(); }

 private:
  friend class RecordWriter;
  enum class State : std::uint8_t { kReset, kBuilding, kPublished };

  std::array<std::byte, kMaximumRecordBytes> storage_{};
  State state_{State::kReset};
  std::array<std::byte, kAccountingQuantumBytes - sizeof(State)> padding_{};
};

[[nodiscard]] SerializedRecordHeader Header(const void* storage) noexcept;
[[nodiscard]] SerializedField Field(const void* storage, std::uint32_t offset) noexcept;
[[nodiscard]] std::string_view Text(const void* storage, std::uint32_t offset,
                                    std::uint32_t size) noexcept;
[[nodiscard]] std::size_t AccountingCharge(std::size_t serialized_bytes) noexcept;

}  // namespace ulog::detail::producer::record
