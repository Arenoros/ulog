#include "encoding/text_encoder.hpp"

#include <array>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/timestamp.hpp>

namespace ulog::detail::encoding {
namespace {

using producer::FieldKind;
using producer::FieldView;

constexpr std::string_view kTskvPrefix{"tskv"};
constexpr std::string_view kNullValue{"null"};
constexpr std::string_view kModuleOpen{" ( "};
constexpr std::string_view kModuleClose{" )"};
constexpr char kPairSeparator = '\t';
constexpr char kRecordTerminator = '\n';
// The longest uppercase level name and std::uint32_t line number.
constexpr std::size_t kMaximumLevelBytes = 8;
constexpr std::size_t kMaximumLineDigits = 10;

struct Layout final {
  std::string_view prefix;
  char key_value_separator;
  /// Writes timestamp, level, and module before the fields.
  bool technical_fields;
};

[[nodiscard]] constexpr Layout LayoutOf(Format format) noexcept {
  switch (format) {
    case Format::kTskv:
      return {.prefix = kTskvPrefix, .key_value_separator = '=', .technical_fields = true};
    case Format::kLtsv:
      return {.prefix = {}, .key_value_separator = ':', .technical_fields = true};
    case Format::kRaw:
    case Format::kJson:
    case Format::kJsonYaDeploy:
      break;
  }
  return {.prefix = kTskvPrefix, .key_value_separator = '=', .technical_fields = false};
}

// Output bytes that do not depend on Record content. Every other output byte comes from at most
// half a serialized Record byte; see MaximumEncodedBytes.
[[nodiscard]] constexpr std::size_t FixedBytes(const Layout& layout) noexcept {
  // A pair is a separator, key, and key/value separator; the first pair of LTSV has no separator.
  constexpr auto kPair = [](std::string_view key) { return 1U + key.size() + 1U; };
  std::size_t bytes = layout.prefix.size() + kPair("text") + 1U;
  if (layout.technical_fields) {
    bytes += kPair("timestamp") + kTimestampTextBytes + kPair("level") + kMaximumLevelBytes +
             kPair("module") + kModuleOpen.size() + 1U + kMaximumLineDigits + kModuleClose.size();
    if (layout.prefix.empty()) {
      --bytes;
    }
  }
  return bytes;
}

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

/// Counts the frame bytes without writing them.
class SizeCounter final {
 public:
  void Literal(std::string_view value) noexcept { size_ += value.size(); }
  void Byte(char) noexcept { ++size_; }
  void Escaped(std::string_view value, bool is_key) noexcept {
    for (const char byte : value) {
      size_ += EscapeCode(byte, is_key) == '\0' ? 1U : 2U;
    }
  }
  template <typename Value>
  void Number(Value value) noexcept {
    // Wide enough for every std::int64_t, std::uint64_t, and shortest double representation.
    std::array<char, 64> buffer{};
    const auto formatted = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
    size_ += static_cast<std::size_t>(formatted.ptr - buffer.data());
  }

  [[nodiscard]] std::size_t Size() const noexcept { return size_; }

 private:
  std::size_t size_{0};
};

/// Writes into an output already checked to hold the counted frame.
class FrameWriter final {
 public:
  explicit FrameWriter(std::span<char> output) noexcept : output_(output) {}

  void Literal(std::string_view value) noexcept {
    if (!value.empty()) {
      std::memcpy(output_.data() + cursor_, value.data(), value.size());
      cursor_ += value.size();
    }
  }
  void Byte(char value) noexcept { output_[cursor_++] = value; }
  void Escaped(std::string_view value, bool is_key) noexcept {
    for (const char byte : value) {
      const char escape = EscapeCode(byte, is_key);
      if (escape != '\0') {
        output_[cursor_++] = '\\';
        output_[cursor_++] = escape;
      } else {
        output_[cursor_++] = is_key ? NormalizeKeyByte(byte) : byte;
      }
    }
  }
  template <typename Value>
  void Number(Value value) noexcept {
    const auto formatted =
        std::to_chars(output_.data() + cursor_, output_.data() + output_.size(), value);
    cursor_ = static_cast<std::size_t>(formatted.ptr - output_.data());
  }

  [[nodiscard]] std::size_t Size() const noexcept { return cursor_; }

 private:
  std::span<char> output_;
  std::size_t cursor_{0};
};

template <typename Out>
[[nodiscard]] bool EmitFieldValue(Out& out, const FieldView& field) noexcept {
  switch (field.kind()) {
    case FieldKind::kString: {
      const auto value = field.AsString();
      if (value) {
        out.Escaped(*value, false);
      }
      return value.has_value();
    }
    case FieldKind::kJson: {
      const auto value = field.AsJson();
      if (value) {
        out.Escaped(*value, false);
      }
      return value.has_value();
    }
    case FieldKind::kInt64: {
      const auto value = field.AsInt64();
      if (value) {
        out.Number(*value);
      }
      return value.has_value();
    }
    case FieldKind::kUInt64: {
      const auto value = field.AsUInt64();
      if (value) {
        out.Number(*value);
      }
      return value.has_value();
    }
    case FieldKind::kDouble: {
      const auto value = field.AsDouble();
      if (value) {
        out.Number(*value);
      }
      return value.has_value();
    }
    case FieldKind::kBool: {
      const auto value = field.AsBool();
      if (value) {
        out.Byte(*value ? '1' : '0');
      }
      return value.has_value();
    }
    case FieldKind::kNull:
      if (field.IsNull()) {
        out.Literal(kNullValue);
      }
      return field.IsNull();
  }
  return false;
}

/// Emits the whole frame through `out`; the same traversal sizes and writes it.
template <typename Out>
[[nodiscard]] bool Emit(Out& out, const Layout& layout, const producer::RecordView& record,
                        std::string_view timestamp) noexcept {
  out.Literal(layout.prefix);
  bool first_pair = layout.prefix.empty();
  const auto begin_pair = [&] {
    if (!first_pair) {
      out.Byte(kPairSeparator);
    }
    first_pair = false;
  };
  const auto technical_key = [&](std::string_view key) {
    begin_pair();
    out.Literal(key);
    out.Byte(layout.key_value_separator);
  };

  if (layout.technical_fields) {
    technical_key("timestamp");
    out.Literal(timestamp);
    technical_key("level");
    out.Literal(ToUpperCaseString(record.level()));
    technical_key("module");
    out.Escaped(record.source_function(), false);
    out.Literal(kModuleOpen);
    out.Escaped(record.source_path(), false);
    out.Byte(':');
    out.Number(record.source_line());
    out.Literal(kModuleClose);
  }

  auto fields = record.Fields();
  const std::size_t field_count = record.field_count();
  for (std::size_t index = 0; index < field_count; ++index) {
    const auto field = fields.Next();
    if (!field) {
      return false;
    }
    begin_pair();
    out.Escaped(field->key(), true);
    out.Byte(layout.key_value_separator);
    if (!EmitFieldValue(out, *field)) {
      return false;
    }
  }

  technical_key("text");
  out.Escaped(record.message(), false);
  out.Byte(kRecordTerminator);
  return true;
}

}  // namespace

bool IsEncodable(Format format) noexcept {
  return format == Format::kTskv || format == Format::kLtsv || format == Format::kRaw;
}

// Each serialized byte of the message, source path, function name, field keys, and string
// values becomes at most two escaped bytes. A field's 24 serialized metadata bytes cover its two
// separators and a formatted number of at most 24 bytes, so the variable part never exceeds
// twice the serialized Record size.
std::size_t MaximumEncodedBytes(Format format, std::size_t maximum_record_bytes) noexcept {
  if (!IsEncodable(format)) {
    return 0U;
  }
  const std::size_t fixed_bytes = FixedBytes(LayoutOf(format));
  if (maximum_record_bytes > (std::numeric_limits<std::size_t>::max() - fixed_bytes) / 2U) {
    return 0U;
  }
  return fixed_bytes + maximum_record_bytes * 2U;
}

EncodeResult EncodeRecord(Format format, const producer::RecordView& record,
                          std::span<char> output) noexcept {
  if (!IsEncodable(format)) {
    return {};
  }
  const Layout layout = LayoutOf(format);
  // An instant the platform cannot convert to local time leaves the timestamp value empty.
  const TimestampText timestamp =
      layout.technical_fields ? FormatLocalTimestamp(record.event_timestamp()) : TimestampText{};
  SizeCounter counter;
  if (!Emit(counter, layout, record, timestamp.View()) || counter.Size() > output.size()) {
    return {};
  }
  FrameWriter writer{output};
  static_cast<void>(Emit(writer, layout, record, timestamp.View()));
  return {.encoded_bytes = writer.Size(), .complete = true};
}

}  // namespace ulog::detail::encoding
