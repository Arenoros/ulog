#include "encoding/raw_encoder.hpp"

#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>

namespace ulog::detail::encoding {
namespace {

using producer::FieldKind;
using producer::FieldView;

constexpr std::string_view kRawPrefix{"tskv"};
constexpr std::string_view kTextPrefix{"\ttext="};
constexpr std::string_view kNullValue{"null"};
constexpr char kFieldSeparator = '\t';
constexpr char kKeyValueSeparator = '=';
constexpr char kRecordTerminator = '\n';

[[nodiscard]] char EscapeCode(char value, bool is_key) noexcept {
  switch (value) {
    case '\0':
      return '0';
    case '\t':
      return 't';
    case '\n':
      return 'n';
    case '\r':
      return 'r';
    case '\\':
      return '\\';
    case '=':
      return is_key ? '=' : '\0';
    default:
      return '\0';
  }
}

[[nodiscard]] char NormalizeKeyByte(char value) noexcept {
  return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

[[nodiscard]] bool AddSize(std::size_t& total, std::size_t increment) noexcept {
  if (increment > std::numeric_limits<std::size_t>::max() - total) {
    return false;
  }
  total += increment;
  return true;
}

[[nodiscard]] bool AddEscapedSize(std::size_t& total, std::string_view value,
                                  bool is_key) noexcept {
  for (const char byte : value) {
    if (!AddSize(total, EscapeCode(byte, is_key) == '\0' ? 1U : 2U)) {
      return false;
    }
  }
  return true;
}

template <typename Value>
[[nodiscard]] std::optional<std::size_t> FormattedNumberSize(Value value) noexcept {
  std::array<char, 64> buffer{};
  const auto formatted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
  if (formatted.ec != std::errc{}) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(formatted.ptr - buffer.data());
}

[[nodiscard]] bool AddFieldValueSize(std::size_t& total, const FieldView& field) noexcept {
  switch (field.kind()) {
    case FieldKind::kString: {
      const auto value = field.AsString();
      return value && AddEscapedSize(total, *value, false);
    }
    case FieldKind::kInt64: {
      const auto value = field.AsInt64();
      const auto size = value ? FormattedNumberSize(*value) : std::nullopt;
      return size && AddSize(total, *size);
    }
    case FieldKind::kUInt64: {
      const auto value = field.AsUInt64();
      const auto size = value ? FormattedNumberSize(*value) : std::nullopt;
      return size && AddSize(total, *size);
    }
    case FieldKind::kDouble: {
      const auto value = field.AsDouble();
      const auto size = value ? FormattedNumberSize(*value) : std::nullopt;
      return size && AddSize(total, *size);
    }
    case FieldKind::kBool:
      return field.AsBool().has_value() && AddSize(total, 1U);
    case FieldKind::kNull:
      return field.IsNull() && AddSize(total, kNullValue.size());
  }
  return false;
}

[[nodiscard]] bool RequiredSize(const producer::RecordView& record, std::size_t& result) noexcept {
  result = kRawPrefix.size();
  auto fields = record.Fields();
  const std::size_t field_count = record.field_count();
  for (std::size_t index = 0; index < field_count; ++index) {
    const auto field = fields.Next();
    if (!field || !AddSize(result, 1U) || !AddEscapedSize(result, field->key(), true) ||
        !AddSize(result, 1U) || !AddFieldValueSize(result, *field)) {
      return false;
    }
  }
  return AddSize(result, kTextPrefix.size()) && AddEscapedSize(result, record.message(), false) &&
         AddSize(result, 1U);
}

void WriteLiteral(std::span<char> output, std::size_t& cursor, std::string_view value) noexcept {
  if (!value.empty()) {
    std::memcpy(output.data() + cursor, value.data(), value.size());
    cursor += value.size();
  }
}

void WriteEscaped(std::span<char> output, std::size_t& cursor, std::string_view value,
                  bool is_key) noexcept {
  for (char byte : value) {
    const char escape = EscapeCode(byte, is_key);
    if (escape != '\0') {
      output[cursor++] = '\\';
      output[cursor++] = escape;
    } else {
      output[cursor++] = is_key ? NormalizeKeyByte(byte) : byte;
    }
  }
}

template <typename Value>
void WriteNumber(std::span<char> output, std::size_t& cursor, Value value) noexcept {
  const auto formatted =
      std::to_chars(output.data() + cursor, output.data() + output.size(), value);
  cursor = static_cast<std::size_t>(formatted.ptr - output.data());
}

void WriteFieldValue(std::span<char> output, std::size_t& cursor, const FieldView& field) noexcept {
  switch (field.kind()) {
    case FieldKind::kString:
      WriteEscaped(output, cursor, *field.AsString(), false);
      return;
    case FieldKind::kInt64:
      WriteNumber(output, cursor, *field.AsInt64());
      return;
    case FieldKind::kUInt64:
      WriteNumber(output, cursor, *field.AsUInt64());
      return;
    case FieldKind::kDouble:
      WriteNumber(output, cursor, *field.AsDouble());
      return;
    case FieldKind::kBool:
      output[cursor++] = *field.AsBool() ? '1' : '0';
      return;
    case FieldKind::kNull:
      WriteLiteral(output, cursor, kNullValue);
      return;
  }
}

}  // namespace

std::size_t MaximumRawEncodedBytes(std::size_t maximum_record_bytes) noexcept {
  constexpr std::size_t kFixedBytes = kRawPrefix.size() + kTextPrefix.size() + 1U;
  if (maximum_record_bytes > (std::numeric_limits<std::size_t>::max() - kFixedBytes) / 2U) {
    return 0U;
  }
  return kFixedBytes + maximum_record_bytes * 2U;
}

RawEncodeResult EncodeRawRecord(const producer::RecordView& record,
                                std::span<char> output) noexcept {
  std::size_t required_size = 0;
  if (!RequiredSize(record, required_size) || required_size > output.size()) {
    return {};
  }

  std::size_t cursor = 0;
  WriteLiteral(output, cursor, kRawPrefix);
  auto fields = record.Fields();
  const std::size_t field_count = record.field_count();
  for (std::size_t index = 0; index < field_count; ++index) {
    const auto field = fields.Next();
    output[cursor++] = kFieldSeparator;
    WriteEscaped(output, cursor, field->key(), true);
    output[cursor++] = kKeyValueSeparator;
    WriteFieldValue(output, cursor, *field);
  }
  WriteLiteral(output, cursor, kTextPrefix);
  WriteEscaped(output, cursor, record.message(), false);
  output[cursor++] = kRecordTerminator;
  return {.encoded_bytes = cursor, .complete = true};
}

}  // namespace ulog::detail::encoding
