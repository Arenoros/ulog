#include "encoding/text_encoder.hpp"

#include <array>
#include <charconv>
#include <cmath>
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

/// Names of the JSON technical members; YaDeploy renames timestamp, level, and text.
struct JsonNames final {
  std::string_view timestamp;
  std::string_view level;
  std::string_view text;
};

[[nodiscard]] constexpr bool IsJson(Format format) noexcept {
  return format == Format::kJson || format == Format::kJsonYaDeploy;
}

[[nodiscard]] constexpr JsonNames JsonNamesOf(Format format) noexcept {
  if (format == Format::kJsonYaDeploy) {
    return {.timestamp = "@timestamp", .level = "levelStr", .text = "message"};
  }
  return {.timestamp = "timestamp", .level = "level", .text = "text"};
}

// JSON output bytes that do not depend on Record content: the closing brace, the newline, and the
// four technical members. A string member costs its key plus six bytes: the brace or comma before
// it, two key quotes, the colon, and two value quotes. Every other output byte comes from at most
// a sixth of a serialized Record byte; see MaximumEncodedBytes.
[[nodiscard]] constexpr std::size_t JsonFixedBytes(const JsonNames& names) noexcept {
  constexpr auto kMember = [](std::string_view key) { return key.size() + 6U; };
  // The UTC timestamp ends in `Z`.
  return kMember(names.timestamp) + kTimestampTextBytes + 1U + kMember(names.level) +
         kMaximumLevelBytes + kMember("module") + kModuleOpen.size() + 1U + kMaximumLineDigits +
         kModuleClose.size() + kMember(names.text) + 2U;
}

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

/// Length of the well-formed UTF-8 sequence starting at `index`, or zero when the byte there does
/// not start one: overlong forms, surrogates, and values above U+10FFFF are ill-formed.
[[nodiscard]] std::size_t Utf8SequenceBytes(std::string_view text, std::size_t index) noexcept {
  const auto byte_at = [text](std::size_t position) {
    return static_cast<unsigned char>(text[position]);
  };
  const unsigned char lead = byte_at(index);
  std::size_t length = 0;
  unsigned char second_minimum = 0x80;
  unsigned char second_maximum = 0xBF;
  if (lead >= 0xC2 && lead <= 0xDF) {
    length = 2;
  } else if (lead >= 0xE0 && lead <= 0xEF) {
    length = 3;
    second_minimum = lead == 0xE0 ? 0xA0 : 0x80;
    second_maximum = lead == 0xED ? 0x9F : 0xBF;
  } else if (lead >= 0xF0 && lead <= 0xF4) {
    length = 4;
    second_minimum = lead == 0xF0 ? 0x90 : 0x80;
    second_maximum = lead == 0xF4 ? 0x8F : 0xBF;
  } else {
    return 0;
  }
  if (text.size() - index < length) {
    return 0;
  }
  const unsigned char second = byte_at(index + 1U);
  if (second < second_minimum || second > second_maximum) {
    return 0;
  }
  for (std::size_t offset = 2; offset < length; ++offset) {
    const unsigned char next = byte_at(index + offset);
    if (next < 0x80 || next > 0xBF) {
      return 0;
    }
  }
  return length;
}

constexpr std::string_view kReplacementCharacter{"\xEF\xBF\xBD"};

/// Writes the content of a JSON string: quote, backslash, and control characters are escaped,
/// well-formed UTF-8 is copied, and each byte that does not start a well-formed UTF-8 sequence
/// becomes U+FFFD. Every input byte becomes at most six output bytes.
template <typename Out>
void EmitJsonText(Out& out, std::string_view text) noexcept {
  constexpr std::string_view kHex{"0123456789abcdef"};
  std::size_t run_begin = 0;
  std::size_t index = 0;
  const auto flush = [&] { out.Literal(text.substr(run_begin, index - run_begin)); };
  while (index < text.size()) {
    const auto byte = static_cast<unsigned char>(text[index]);
    if (byte >= 0x80) {
      const std::size_t length = Utf8SequenceBytes(text, index);
      if (length != 0) {
        index += length;
        continue;
      }
      flush();
      out.Literal(kReplacementCharacter);
    } else if (byte >= 0x20 && byte != '"' && byte != '\\') {
      ++index;
      continue;
    } else {
      flush();
      switch (byte) {
        case '"':
          out.Literal("\\\"");
          break;
        case '\\':
          out.Literal("\\\\");
          break;
        case '\b':
          out.Literal("\\b");
          break;
        case '\f':
          out.Literal("\\f");
          break;
        case '\n':
          out.Literal("\\n");
          break;
        case '\r':
          out.Literal("\\r");
          break;
        case '\t':
          out.Literal("\\t");
          break;
        default: {
          const std::array<char, 6> escape{
              '\\', 'u', '0', '0', kHex[byte >> 4U], kHex[byte & 0xFU]};
          out.Literal({escape.data(), escape.size()});
          break;
        }
      }
    }
    ++index;
    run_begin = index;
  }
  flush();
}

[[nodiscard]] constexpr bool IsJsonWhitespace(char byte) noexcept {
  return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

/// Strict RFC 8259 check that `text` is exactly one JSON value, optionally surrounded by
/// whitespace, with well-formed UTF-8 in its strings. Nesting deeper than kMaximumDepth is
/// rejected, so the check needs no heap.
class JsonValidator final {
 public:
  static constexpr std::size_t kMaximumDepth = 256;

  [[nodiscard]] static bool IsValid(std::string_view text) noexcept {
    JsonValidator validator{text};
    return validator.Validate();
  }

 private:
  enum class Expect : std::uint8_t { kValue, kKey, kAfterValue };

  explicit JsonValidator(std::string_view text) noexcept : text_(text) {}

  [[nodiscard]] bool Validate() noexcept {
    Expect expect = Expect::kValue;
    while (true) {
      SkipWhitespace();
      switch (expect) {
        case Expect::kValue:
          if (!ScanValue(expect)) {
            return false;
          }
          break;
        case Expect::kKey:
          if (!ScanString()) {
            return false;
          }
          SkipWhitespace();
          if (!Consume(':')) {
            return false;
          }
          expect = Expect::kValue;
          break;
        case Expect::kAfterValue:
          if (depth_ == 0) {
            return index_ == text_.size();
          }
          if (Consume(',')) {
            expect = InObject() ? Expect::kKey : Expect::kValue;
          } else if (Consume(InObject() ? '}' : ']')) {
            --depth_;
          } else {
            return false;
          }
          break;
      }
    }
  }

  /// Scans a scalar or opens a container, and sets what the next token must be.
  [[nodiscard]] bool ScanValue(Expect& expect) noexcept {
    if (index_ == text_.size()) {
      return false;
    }
    const char byte = text_[index_];
    if (byte == '{' || byte == '[') {
      if (depth_ == kMaximumDepth) {
        return false;
      }
      ++index_;
      const bool object = byte == '{';
      SkipWhitespace();
      if (Consume(object ? '}' : ']')) {
        expect = Expect::kAfterValue;
      } else {
        objects_[depth_] = object;
        ++depth_;
        expect = object ? Expect::kKey : Expect::kValue;
      }
      return true;
    }
    expect = Expect::kAfterValue;
    if (byte == '"') {
      return ScanString();
    }
    if (byte == '-' || (byte >= '0' && byte <= '9')) {
      return ScanNumber();
    }
    return ScanWord("true") || ScanWord("false") || ScanWord("null");
  }

  [[nodiscard]] bool ScanString() noexcept {
    if (!Consume('"')) {
      return false;
    }
    while (index_ < text_.size()) {
      const auto byte = static_cast<unsigned char>(text_[index_]);
      if (byte == '"') {
        ++index_;
        return true;
      }
      if (byte < 0x20) {
        return false;
      }
      if (byte == '\\') {
        if (!ScanEscape()) {
          return false;
        }
      } else if (byte >= 0x80) {
        const std::size_t length = Utf8SequenceBytes(text_, index_);
        if (length == 0) {
          return false;
        }
        index_ += length;
      } else {
        ++index_;
      }
    }
    return false;
  }

  [[nodiscard]] bool ScanEscape() noexcept {
    ++index_;
    if (index_ == text_.size()) {
      return false;
    }
    const char code = text_[index_];
    ++index_;
    if (code == 'u') {
      for (int digit = 0; digit < 4; ++digit) {
        if (index_ == text_.size() || !IsHexDigit(text_[index_])) {
          return false;
        }
        ++index_;
      }
      return true;
    }
    return std::string_view{"\"\\/bfnrt"}.find(code) != std::string_view::npos;
  }

  [[nodiscard]] bool ScanNumber() noexcept {
    static_cast<void>(Consume('-'));
    // A leading zero is the whole integer part.
    if (!Consume('0') && !ScanDigits()) {
      return false;
    }
    if (Consume('.') && !ScanDigits()) {
      return false;
    }
    if (Consume('e') || Consume('E')) {
      if (!Consume('+')) {
        static_cast<void>(Consume('-'));
      }
      return ScanDigits();
    }
    return true;
  }

  [[nodiscard]] bool ScanDigits() noexcept {
    const std::size_t begin = index_;
    while (index_ < text_.size() && text_[index_] >= '0' && text_[index_] <= '9') {
      ++index_;
    }
    return index_ != begin;
  }

  [[nodiscard]] bool ScanWord(std::string_view word) noexcept {
    if (text_.substr(index_, word.size()) != word) {
      return false;
    }
    index_ += word.size();
    return true;
  }

  [[nodiscard]] static constexpr bool IsHexDigit(char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') ||
           (byte >= 'A' && byte <= 'F');
  }

  [[nodiscard]] bool Consume(char expected) noexcept {
    if (index_ < text_.size() && text_[index_] == expected) {
      ++index_;
      return true;
    }
    return false;
  }

  void SkipWhitespace() noexcept {
    while (index_ < text_.size() && IsJsonWhitespace(text_[index_])) {
      ++index_;
    }
  }

  [[nodiscard]] bool InObject() const noexcept { return objects_[depth_ - 1U]; }

  std::string_view text_;
  std::size_t index_{0};
  std::size_t depth_{0};
  std::array<bool, kMaximumDepth> objects_{};
};

/// Writes valid JSON text without its insignificant whitespace.
template <typename Out>
void EmitCompactJson(Out& out, std::string_view json) noexcept {
  bool in_string = false;
  bool escaped = false;
  std::size_t run_begin = 0;
  for (std::size_t index = 0; index < json.size(); ++index) {
    const char byte = json[index];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        in_string = false;
      }
    } else if (byte == '"') {
      in_string = true;
    } else if (IsJsonWhitespace(byte)) {
      out.Literal(json.substr(run_begin, index - run_begin));
      run_begin = index + 1U;
    }
  }
  out.Literal(json.substr(run_begin));
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

template <typename Out>
void EmitJsonString(Out& out, std::string_view text) noexcept {
  out.Byte('"');
  EmitJsonText(out, text);
  out.Byte('"');
}

template <typename Out>
[[nodiscard]] bool EmitJsonFieldValue(Out& out, const FieldView& field) noexcept {
  switch (field.kind()) {
    case FieldKind::kString: {
      const auto value = field.AsString();
      if (value) {
        EmitJsonString(out, *value);
      }
      return value.has_value();
    }
    case FieldKind::kJson: {
      const auto value = field.AsJson();
      if (value) {
        if (JsonValidator::IsValid(*value)) {
          EmitCompactJson(out, *value);
        } else {
          EmitJsonString(out, *value);
        }
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
        // JSON has no NaN or infinity; those are written as strings such as "nan" and "-inf".
        const bool finite = std::isfinite(*value);
        if (!finite) {
          out.Byte('"');
        }
        out.Number(*value);
        if (!finite) {
          out.Byte('"');
        }
      }
      return value.has_value();
    }
    case FieldKind::kBool: {
      const auto value = field.AsBool();
      if (value) {
        out.Literal(*value ? std::string_view{"true"} : std::string_view{"false"});
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

/// Emits one compact JSON object and LF through `out`; the same traversal sizes and writes it.
template <typename Out>
[[nodiscard]] bool EmitJson(Out& out, const JsonNames& names, const producer::RecordView& record,
                            std::string_view timestamp) noexcept {
  const auto member = [&out](char opening, std::string_view key) {
    out.Byte(opening);
    out.Byte('"');
    out.Literal(key);
    out.Literal("\":");
  };

  member('{', names.timestamp);
  out.Byte('"');
  out.Literal(timestamp);
  if (!timestamp.empty()) {
    out.Byte('Z');
  }
  out.Byte('"');
  member(',', names.level);
  out.Byte('"');
  out.Literal(ToUpperCaseString(record.level()));
  out.Byte('"');
  member(',', "module");
  out.Byte('"');
  EmitJsonText(out, record.source_function());
  out.Literal(kModuleOpen);
  EmitJsonText(out, record.source_path());
  out.Byte(':');
  out.Number(record.source_line());
  out.Literal(kModuleClose);
  out.Byte('"');

  auto fields = record.Fields();
  const std::size_t field_count = record.field_count();
  for (std::size_t index = 0; index < field_count; ++index) {
    const auto field = fields.Next();
    if (!field) {
      return false;
    }
    out.Byte(',');
    EmitJsonString(out, field->key());
    out.Byte(':');
    if (!EmitJsonFieldValue(out, *field)) {
      return false;
    }
  }

  member(',', names.text);
  EmitJsonString(out, record.message());
  out.Literal("}\n");
  return true;
}

/// Sizes the frame `emit` produces and writes it only if it fits `output`.
template <typename EmitFrame>
[[nodiscard]] EncodeResult EncodeFrame(std::span<char> output, const EmitFrame& emit) noexcept {
  SizeCounter counter;
  if (!emit(counter) || counter.Size() > output.size()) {
    return {};
  }
  FrameWriter writer{output};
  static_cast<void>(emit(writer));
  return {.encoded_bytes = writer.Size(), .complete = true};
}

}  // namespace

bool IsEncodable(Format format) noexcept {
  switch (format) {
    case Format::kTskv:
    case Format::kLtsv:
    case Format::kRaw:
    case Format::kJson:
    case Format::kJsonYaDeploy:
      return true;
  }
  return false;
}

// Text formats: each serialized byte of the message, source path, function name, field keys, and
// string values becomes at most two escaped bytes. A field's 24 serialized metadata bytes cover
// its two separators and a formatted number of at most 24 bytes, so the variable part never
// exceeds twice the serialized Record size.
//
// JSON formats: each of those bytes becomes at most six bytes (`\u00XX`), raw JSON is copied
// without whitespace or written as an escaped string, and a field's 24 metadata bytes cover its
// comma, key quotes, colon, value quotes, and a number of at most 24 bytes, so the variable part
// never exceeds six times the serialized Record size.
std::size_t MaximumEncodedBytes(Format format, std::size_t maximum_record_bytes) noexcept {
  if (!IsEncodable(format)) {
    return 0U;
  }
  const bool json = IsJson(format);
  const std::size_t fixed_bytes =
      json ? JsonFixedBytes(JsonNamesOf(format)) : FixedBytes(LayoutOf(format));
  const std::size_t expansion = json ? 6U : 2U;
  if (maximum_record_bytes > (std::numeric_limits<std::size_t>::max() - fixed_bytes) / expansion) {
    return 0U;
  }
  return fixed_bytes + maximum_record_bytes * expansion;
}

EncodeResult EncodeRecord(Format format, const producer::RecordView& record,
                          std::span<char> output) noexcept {
  if (!IsEncodable(format)) {
    return {};
  }
  if (IsJson(format)) {
    // An instant outside years 0000 through 9999 leaves the timestamp value empty.
    const TimestampText timestamp = FormatUtcTimestamp(record.event_timestamp());
    const JsonNames names = JsonNamesOf(format);
    return EncodeFrame(output,
                       [&](auto& out) { return EmitJson(out, names, record, timestamp.View()); });
  }
  const Layout layout = LayoutOf(format);
  // An instant the platform cannot convert to local time leaves the timestamp value empty.
  const TimestampText timestamp =
      layout.technical_fields ? FormatLocalTimestamp(record.event_timestamp()) : TimestampText{};
  return EncodeFrame(output,
                     [&](auto& out) { return Emit(out, layout, record, timestamp.View()); });
}

}  // namespace ulog::detail::encoding
