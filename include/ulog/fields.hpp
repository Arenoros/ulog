#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ulog {

class FieldSet;
class FieldWriter;

namespace detail {
struct FieldSinkAccess;

// Character types are text units, not numbers; signed and unsigned char stay int8/uint8.
template <typename T>
concept CharacterType =
    std::same_as<T, char> || std::same_as<T, wchar_t> || std::same_as<T, char8_t> ||
    std::same_as<T, char16_t> || std::same_as<T, char32_t>;
}  // namespace detail

/// Type of one typed Record field value.
enum class FieldKind : std::uint8_t { kString, kInt64, kUInt64, kDouble, kBool, kNull, kJson };

/// Whether a later value for the same key may replace a field.
enum class FieldMode : std::uint8_t { kReplaceable, kFrozen };

/// Outcome of adding one field to a FieldSet or a Record.
enum class FieldStatus : std::uint8_t {
  /// The key was new and the field was appended.
  kAdded,
  /// A replaceable field with the same key took the new value at its original position.
  kReplaced,
  /// A frozen field with the same key kept its value.
  kFrozen,
  /// The key is a technical key; nothing changed.
  kReserved,
  /// The key is empty; nothing changed.
  kInvalidKey,
  /// The field did not fit in the Record; nothing changed and the Record is marked truncated.
  kTruncated,
};

/// Explicit JSON `null` field value.
struct Null final {};
inline constexpr Null kNull{};

/// Returns whether `key` names a technical field that user fields cannot use.
///
/// Reserved keys are `timestamp`, `level`, `module`, `task_id`, `thread_id`, `text`, and every key
/// starting with `ulog.`, such as `ulog.truncated`. They are compared ignoring ASCII case, because
/// text formats lowercase keys. Other keys collide only when they are byte-for-byte equal.
[[nodiscard]] constexpr bool IsReservedFieldKey(std::string_view key) noexcept {
  const auto equals = [key](std::string_view reserved, bool prefix) {
    if (prefix ? key.size() < reserved.size() : key.size() != reserved.size()) {
      return false;
    }
    for (std::size_t index = 0; index < reserved.size(); ++index) {
      const char byte = key[index];
      const char lower = byte >= 'A' && byte <= 'Z' ? static_cast<char>(byte - 'A' + 'a') : byte;
      if (lower != reserved[index]) {
        return false;
      }
    }
    return true;
  };
  return equals("timestamp", false) || equals("level", false) || equals("module", false) ||
         equals("task_id", false) || equals("thread_id", false) || equals("text", false) ||
         equals("ulog.", true);
}

/// One line of owned JSON text, inserted by JSON encoders as a raw nested value.
///
/// CR and LF are removed on construction, and empty text means JSON `null`. The text is not
/// validated. Text encoders write and escape it like a string.
class JsonString final {
 public:
  JsonString() = default;
  explicit JsonString(std::string_view json) {
    text_.reserve(json.size());
    for (const char byte : json) {
      if (byte != '\r' && byte != '\n') {
        text_.push_back(byte);
      }
    }
  }

  [[nodiscard]] std::string_view View() const noexcept {
    return text_.empty() ? std::string_view{"null"} : std::string_view{text_};
  }

 private:
  std::string text_;
};

/// Non-owning typed field value.
///
/// String and JSON values borrow their text; a Record or FieldSet copies it on insertion. Signed
/// integers are stored as `int64`, unsigned integers as `uint64`, and floating point as `double`.
class FieldValue final {
 public:
  constexpr FieldValue() noexcept = default;
  constexpr FieldValue(Null) noexcept {}
  constexpr FieldValue(std::nullptr_t) noexcept {}

  template <typename T>
    requires std::same_as<T, bool>
  constexpr FieldValue(T value) noexcept : kind_(FieldKind::kBool), bits_(value ? 1U : 0U) {}

  template <std::signed_integral T>
    requires(!detail::CharacterType<T>)
  constexpr FieldValue(T value) noexcept
      : kind_(FieldKind::kInt64), bits_(std::bit_cast<std::uint64_t>(std::int64_t{value})) {}

  template <std::unsigned_integral T>
    requires(!std::same_as<T, bool> && !detail::CharacterType<T>)
  constexpr FieldValue(T value) noexcept : kind_(FieldKind::kUInt64), bits_(std::uint64_t{value}) {}

  template <std::floating_point T>
  constexpr FieldValue(T value) noexcept
      : kind_(FieldKind::kDouble),
        bits_(std::bit_cast<std::uint64_t>(static_cast<double>(value))) {}

  template <typename T>
    requires(std::is_convertible_v<const T&, std::string_view> &&
             !std::same_as<std::remove_cvref_t<T>, std::nullptr_t>)
  constexpr FieldValue(const T& value) noexcept
      : kind_(FieldKind::kString), text_(std::string_view{value}) {}

  FieldValue(const JsonString& value) noexcept : kind_(FieldKind::kJson), text_(value.View()) {}

  [[nodiscard]] constexpr FieldKind Kind() const noexcept { return kind_; }
  /// String text for kString or JSON text for kJson; empty for other kinds.
  [[nodiscard]] constexpr std::string_view Text() const noexcept { return text_; }
  [[nodiscard]] constexpr std::int64_t Int64() const noexcept {
    return std::bit_cast<std::int64_t>(bits_);
  }
  [[nodiscard]] constexpr std::uint64_t UInt64() const noexcept { return bits_; }
  [[nodiscard]] constexpr double Double() const noexcept { return std::bit_cast<double>(bits_); }
  [[nodiscard]] constexpr bool Bool() const noexcept { return bits_ != 0U; }

  /// Rebuilds a value from its parts; `text` is used for kString and kJson, `bits` otherwise.
  [[nodiscard]] static constexpr FieldValue FromParts(FieldKind kind, std::string_view text,
                                                      std::uint64_t bits) noexcept {
    FieldValue value;
    value.kind_ = kind;
    if (kind == FieldKind::kString || kind == FieldKind::kJson) {
      value.text_ = text;
    } else if (kind != FieldKind::kNull) {
      value.bits_ = bits;
    }
    return value;
  }

 private:
  FieldKind kind_{FieldKind::kNull};
  std::string_view text_{};
  std::uint64_t bits_{0};
};

/// Owned ordered field set, built before a logging call and inserted into Records.
///
/// Adding a key that already exists replaces a replaceable field in place and leaves a frozen
/// field unchanged. Technical keys (see IsReservedFieldKey) and empty keys are rejected.
class FieldSet final {
 public:
  FieldSet() = default;

  FieldStatus Add(std::string_view key, FieldValue value,
                  FieldMode mode = FieldMode::kReplaceable) {
    if (key.empty()) {
      return FieldStatus::kInvalidKey;
    }
    if (IsReservedFieldKey(key)) {
      return FieldStatus::kReserved;
    }
    for (Entry& entry : entries_) {
      if (entry.key == key) {
        if (entry.frozen) {
          return FieldStatus::kFrozen;
        }
        entry.Assign(value, mode);
        return FieldStatus::kReplaced;
      }
    }
    Entry& entry = entries_.emplace_back();
    entry.key.assign(key);
    entry.Assign(value, mode);
    return FieldStatus::kAdded;
  }

  /// Freezes an existing field; returns false when the key is absent.
  bool Freeze(std::string_view key) noexcept {
    for (Entry& entry : entries_) {
      if (entry.key == key) {
        entry.frozen = true;
        return true;
      }
    }
    return false;
  }

  /// Adds every field of `other` in order, preserving each field's freeze state.
  void Merge(const FieldSet& other) {
    if (&other == this) {
      return;
    }
    for (const Entry& entry : other.entries_) {
      static_cast<void>(Add(entry.key, entry.Value(), entry.Mode()));
    }
  }

  void Merge(FieldSet&& other) {
    if (&other == this) {
      return;
    }
    if (entries_.empty()) {
      entries_ = std::move(other.entries_);
      other.entries_.clear();
      return;
    }
    Merge(static_cast<const FieldSet&>(other));
    other.entries_.clear();
  }

  [[nodiscard]] std::size_t Size() const noexcept { return entries_.size(); }
  [[nodiscard]] bool Empty() const noexcept { return entries_.empty(); }
  [[nodiscard]] std::string_view Key(std::size_t index) const noexcept {
    return entries_[index].key;
  }
  [[nodiscard]] FieldValue Value(std::size_t index) const noexcept {
    return entries_[index].Value();
  }
  [[nodiscard]] FieldMode Mode(std::size_t index) const noexcept { return entries_[index].Mode(); }

 private:
  struct Entry final {
    std::string key;
    std::string text;
    std::uint64_t bits{0};
    FieldKind kind{FieldKind::kNull};
    bool frozen{false};

    void Assign(const FieldValue& value, FieldMode mode) {
      kind = value.Kind();
      text.assign(value.Text());
      bits = value.UInt64();
      frozen = mode == FieldMode::kFrozen;
    }
    [[nodiscard]] FieldValue Value() const noexcept {
      return FieldValue::FromParts(kind, text, bits);
    }
    [[nodiscard]] FieldMode Mode() const noexcept {
      return frozen ? FieldMode::kFrozen : FieldMode::kReplaceable;
    }
  };

  std::vector<Entry> entries_;
};

/// One call-site field, or a whole FieldSet, for `ulog::Fields`.
class Field final {
 public:
  Field(std::string_view key, FieldValue value, FieldMode mode = FieldMode::kReplaceable) noexcept
      : key_(key), value_(value), mode_(mode) {}
  // Implicit so that a FieldSet can appear among call-site fields.
  Field(const FieldSet& fields) noexcept : set_(&fields) {}

 private:
  friend class FieldWriter;

  std::string_view key_{};
  FieldValue value_{};
  FieldMode mode_{FieldMode::kReplaceable};
  const FieldSet* set_{nullptr};
};

/// Ordered call-site fields, the first operand of a `LOG*` macro:
///
///   LOG_INFO(ulog::Fields{{"status", 200}, {"path", path}, extra}, "done in {}ms", elapsed);
///
/// It borrows its elements and must be used within the full expression that creates it.
class Fields final {
 public:
  Fields(std::initializer_list<Field> fields) noexcept : fields_(fields) {}

  [[nodiscard]] const Field* begin() const noexcept { return fields_.begin(); }
  [[nodiscard]] const Field* end() const noexcept { return fields_.end(); }

 private:
  std::initializer_list<Field> fields_;
};

namespace detail {

using FieldConsumer = FieldStatus (*)(void*, std::string_view, const FieldValue&,
                                      FieldMode) noexcept;

}  // namespace detail

/// Writes typed fields into the Record being built by a Logger call.
///
/// Fields follow the FieldSet collision policy within the Record. Values are copied into the
/// reserved Record; a field that does not fit returns kTruncated and marks the Record truncated.
class FieldWriter final {
 public:
  FieldWriter(const FieldWriter&) = delete;
  FieldWriter& operator=(const FieldWriter&) = delete;

  FieldStatus Add(std::string_view key, FieldValue value,
                  FieldMode mode = FieldMode::kReplaceable) noexcept {
    return consume_(context_, key, value, mode);
  }

  /// Adds every field of `fields` in order, preserving each field's freeze state.
  void Add(const FieldSet& fields) noexcept {
    for (std::size_t index = 0; index < fields.Size(); ++index) {
      static_cast<void>(Add(fields.Key(index), fields.Value(index), fields.Mode(index)));
    }
  }

  void Add(const Fields& fields) noexcept {
    for (const Field& field : fields) {
      if (field.set_ != nullptr) {
        Add(*field.set_);
      } else {
        static_cast<void>(Add(field.key_, field.value_, field.mode_));
      }
    }
  }

 private:
  friend struct detail::FieldSinkAccess;
  FieldWriter(void* context, detail::FieldConsumer consume) noexcept
      : context_(context), consume_(consume) {}

  void* context_;
  detail::FieldConsumer consume_;
};

}  // namespace ulog
