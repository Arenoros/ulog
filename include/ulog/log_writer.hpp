#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>
#include <type_traits>
#include <ulog/fields.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>
#include <utility>

namespace ulog {

namespace detail {

template <typename FieldList>
concept MacroFieldList = std::same_as<std::remove_cvref_t<FieldList>, Fields> ||
                         std::same_as<std::remove_cvref_t<FieldList>, FieldSet>;

/// Types streamed as text: everything usable as std::string_view except C strings, which have
/// their own overload.
template <typename Text>
concept StreamText = std::is_constructible_v<std::string_view, const Text&> &&
                     !std::is_pointer_v<std::decay_t<Text>> && !MacroFieldList<Text>;

/// Character types other than `char` are text units in an unknown encoding.
template <typename Value>
concept TextUnit = std::same_as<Value, wchar_t> || std::same_as<Value, char8_t> ||
                   std::same_as<Value, char16_t> || std::same_as<Value, char32_t>;

/// Forces the call site's SourceLocation, including path trimming, to be a constant.
[[nodiscard]] consteval SourceLocation CallSite(SourceLocation source) noexcept { return source; }

}  // namespace detail

/// Scope writer for one Record.
///
/// The constructor filters `level` against the Logger and reserves the Record before any operand is
/// written, so a disabled writer evaluates nothing it is not given. Text and typed fields are
/// written straight into the reservation; `Submit()` or the destructor publishes the Record once.
/// A writer whose Logger filtered or rejected the call, whose formatting failed, or that is
/// destroyed by an exception publishes nothing. Text beyond the Record bound is cut at a valid
/// UTF-8 boundary and the Record gets one `ulog.truncated=true` field.
///
/// Writing never allocates and never throws: a failure inside fmt formatting or a string
/// conversion makes the writer abandon its Record.
class LogWriter final {
 public:
  LogWriter(Logger logger, Level level, const SourceLocation& source) noexcept
      : record_(logger.BeginRecord(level, source)),
        uncaught_exceptions_(record_.finish != nullptr ? std::uncaught_exceptions() : 0) {}

  LogWriter(const LogWriter&) = delete;
  LogWriter& operator=(const LogWriter&) = delete;
  LogWriter(LogWriter&&) = delete;
  LogWriter& operator=(LogWriter&&) = delete;

  /// Publishes the Record, unless the writer is inactive or an exception is unwinding through it.
  ~LogWriter() {
    if (record_.finish != nullptr) {
      Finish(!failed_ && std::uncaught_exceptions() == uncaught_exceptions_);
    }
  }

  /// True while the writer holds an admitted Record that it has not submitted.
  [[nodiscard]] bool IsActive() const noexcept { return record_.finish != nullptr; }
  [[nodiscard]] explicit operator bool() const noexcept { return IsActive(); }

  /// Publishes the Record now, or abandons it after a failure; the writer becomes inactive.
  void Submit() noexcept {
    if (record_.finish != nullptr) {
      Finish(!failed_);
    }
  }

  /// Starts the text like the logging macros: no operand, one text operand whose braces are
  /// literal, or a format string and values, optionally after a leading Fields or FieldSet.
  LogWriter& operator()() noexcept { return *this; }

  template <typename Text>
    requires(!detail::MacroFieldList<Text>)
  LogWriter& operator()(Text&& text) noexcept {
    static_assert(
        requires { std::string_view{std::forward<Text>(text)}; },
        "A one-operand Ulog message must be usable as std::string_view.");
    if (CanWrite()) {
      try {
        AppendText(std::string_view{std::forward<Text>(text)});
      } catch (...) {
        failed_ = true;
      }
    }
    return *this;
  }

  template <typename... Args>
    requires(sizeof...(Args) > 0)
  LogWriter& operator()(fmt::format_string<Args...> format, Args&&... args) noexcept {
    return Format(format, std::forward<Args>(args)...);
  }

  template <detail::MacroFieldList FieldList>
  LogWriter& operator()(const FieldList& fields) noexcept {
    return *this << fields;
  }

  template <detail::MacroFieldList FieldList, typename Text>
  LogWriter& operator()(const FieldList& fields, Text&& text) noexcept {
    *this << fields;
    return (*this)(std::forward<Text>(text));
  }

  template <detail::MacroFieldList FieldList, typename... Args>
    requires(sizeof...(Args) > 0)
  LogWriter& operator()(const FieldList& fields, fmt::format_string<Args...> format,
                        Args&&... args) noexcept {
    *this << fields;
    return Format(format, std::forward<Args>(args)...);
  }

  /// Appends fmt-formatted text; a formatting failure abandons the Record.
  template <typename... Args>
  LogWriter& Format(fmt::format_string<Args...> format, Args&&... args) noexcept {
    if (CanWrite()) {
      try {
        BufferedText buffer{*this};
        fmt::format_to(buffer.Output(), format, std::forward<Args>(args)...);
        buffer.Flush();
      } catch (...) {
        failed_ = true;
      }
    }
    return *this;
  }

  /// Adds one typed field under the Record's collision and reserved-key policy.
  FieldStatus AddField(std::string_view key, const FieldValue& value,
                       FieldMode mode = FieldMode::kReplaceable) noexcept {
    if (!CanWrite()) {
      return FieldStatus::kTruncated;
    }
    return record_.put_field(record_.context, key, value, mode);
  }

  LogWriter& operator<<(const Fields& fields) noexcept {
    if (CanWrite()) {
      FieldSink().Add(fields);
    }
    return *this;
  }

  LogWriter& operator<<(const FieldSet& fields) noexcept {
    if (CanWrite()) {
      FieldSink().Add(fields);
    }
    return *this;
  }

  LogWriter& operator<<(char value) noexcept {
    if (CanWrite()) {
      AppendText(std::string_view{&value, 1});
    }
    return *this;
  }

  /// A null C string writes `(null)`.
  LogWriter& operator<<(const char* text) noexcept {
    if (CanWrite()) {
      AppendText(text != nullptr ? std::string_view{text} : std::string_view{"(null)"});
    }
    return *this;
  }

  template <detail::StreamText Text>
  LogWriter& operator<<(const Text& text) noexcept {
    if (CanWrite()) {
      try {
        AppendText(std::string_view{text});
      } catch (...) {
        failed_ = true;
      }
    }
    return *this;
  }

  LogWriter& operator<<(bool value) noexcept {
    if (CanWrite()) {
      AppendText(value ? std::string_view{"true"} : std::string_view{"false"});
    }
    return *this;
  }

  /// Integers are written in decimal; `signed char` and `unsigned char` are numbers too.
  template <std::integral Value>
    requires(!std::same_as<Value, bool> && !std::same_as<Value, char>)
  LogWriter& operator<<(Value value) noexcept {
    static_assert(!detail::TextUnit<Value>,
                  "Ulog streams only char text; convert wide and UTF character types to UTF-8 "
                  "text, or cast them to an integer to log their code.");
    if (CanWrite()) {
      std::array<char, 24> digits{};
      const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), value);
      AppendText(
          std::string_view{digits.data(), static_cast<std::size_t>(written.ptr - digits.data())});
    }
    return *this;
  }

  /// Floating-point values use fmt's shortest round-trip form, such as `1.5`, `1e+300`, `nan`, and
  /// `-inf`.
  LogWriter& operator<<(float value) noexcept { return WriteFloatingPoint(value); }
  LogWriter& operator<<(double value) noexcept { return WriteFloatingPoint(value); }
  LogWriter& operator<<(long double value) noexcept { return WriteFloatingPoint(value); }

  /// Writes the exception's `what()` text.
  LogWriter& operator<<(const std::exception& exception) noexcept {
    if (CanWrite()) {
      const char* const text = exception.what();
      AppendText(text != nullptr ? std::string_view{text} : std::string_view{});
    }
    return *this;
  }

 private:
  /// Collects fmt output in a small buffer and appends it in chunks.
  class BufferedText final {
   public:
    class Iterator final {
     public:
      using value_type = void;
      using difference_type = std::ptrdiff_t;
      using pointer = void;
      using reference = void;
      using iterator_category = std::output_iterator_tag;

      explicit Iterator(BufferedText& buffer) noexcept : buffer_(&buffer) {}
      Iterator& operator=(char value) noexcept {
        buffer_->Put(value);
        return *this;
      }
      [[nodiscard]] Iterator& operator*() noexcept { return *this; }
      Iterator& operator++() noexcept { return *this; }
      Iterator operator++(int) noexcept { return *this; }

     private:
      BufferedText* buffer_;
    };

    explicit BufferedText(LogWriter& writer) noexcept : writer_(&writer) {}

    [[nodiscard]] Iterator Output() noexcept { return Iterator{*this}; }

    void Put(char value) noexcept {
      if (size_ == bytes_.size()) {
        Flush();
      }
      bytes_[size_] = value;
      ++size_;
    }

    void Flush() noexcept {
      writer_->AppendText(std::string_view{bytes_.data(), size_});
      size_ = 0;
    }

   private:
    LogWriter* writer_;
    std::array<char, 128> bytes_{};
    std::size_t size_{0};
  };

  [[nodiscard]] bool CanWrite() const noexcept { return record_.finish != nullptr && !failed_; }

  void AppendText(std::string_view text) noexcept {
    if (!text.empty()) {
      record_.append_text(record_.context, text);
    }
  }

  [[nodiscard]] FieldWriter FieldSink() noexcept {
    return detail::FieldSinkAccess::Make(record_.context, record_.put_field);
  }

  template <typename Value>
  LogWriter& WriteFloatingPoint(Value value) noexcept {
    if (CanWrite()) {
      try {
        std::array<char, 64> text{};
        const auto written = fmt::format_to_n(text.data(), text.size(), "{}", value);
        AppendText(std::string_view{text.data(), std::min(written.size, text.size())});
      } catch (...) {
        failed_ = true;
      }
    }
    return *this;
  }

  void Finish(bool publish) noexcept {
    const detail::OpenRecord record = std::exchange(record_, detail::OpenRecord{});
    record.finish(record.context, publish);
  }

  detail::OpenRecord record_;
  int uncaught_exceptions_;
  bool failed_{false};
};

}  // namespace ulog
