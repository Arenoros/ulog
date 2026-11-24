#pragma once

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <optional>
#include <ostream>
#include <ranges>
#include <streambuf>
#include <string_view>
#include <system_error>
#include <tuple>
#include <type_traits>
#include <typeinfo>
#include <ulog/export.hpp>
#include <ulog/fields.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>
#include <utility>

namespace ulog {

/// Fixed-width hexadecimal: `0x` and 16 uppercase digits. Pointers are written this way.
struct Hex final {
  template <std::unsigned_integral Unsigned>
  explicit constexpr Hex(Unsigned unsigned_value) noexcept : value(unsigned_value) {}
  template <typename Pointee>
  explicit Hex(Pointee* pointer) noexcept : value(reinterpret_cast<std::uintptr_t>(pointer)) {}

  std::uint64_t value;
};

/// Shortest uppercase hexadecimal without a prefix, such as `1F`.
struct HexShort final {
  template <std::unsigned_integral Unsigned>
  explicit constexpr HexShort(Unsigned unsigned_value) noexcept : value(unsigned_value) {}
  template <typename Pointee>
  explicit HexShort(Pointee* pointer) noexcept : value(reinterpret_cast<std::uintptr_t>(pointer)) {}

  std::uint64_t value;
};

/// Text in double quotes, with `"` and `\` escaped by a backslash.
struct Quoted final {
  std::string_view string;
};

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

/// Always false; a static_assert that names it fails only when instantiated.
template <typename>
inline constexpr bool kUnsupportedValue = false;

/// Maps have a key and a mapped type; their elements are written as `key: value`.
template <typename Range>
concept MapRange = requires {
  typename Range::key_type;
  typename Range::mapped_type;
};

template <typename Value>
concept OstreamWritable = requires(std::ostream& stream, const Value& value) { stream << value; };

/// The readable name of a dynamic type, such as `std::runtime_error`. GCC and Clang demangle into
/// a per-thread buffer that is reused once it is large enough.
[[nodiscard]] ULOG_API std::string_view TypeName(const std::type_info& type) noexcept;

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
/// Native forms never allocate once warmed up; ostream-writable values and `std::error_code`
/// may allocate. Writing never throws, except from a lazy writer invocable: a failure inside fmt
/// formatting, a string conversion, an ostream operator, or a range makes the writer abandon its
/// Record.
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
    return WriteText(text != nullptr ? std::string_view{text} : std::string_view{"(null)"});
  }

  LogWriter& operator<<(std::nullptr_t) noexcept { return WriteText("(null)"); }

  /// A null pointer writes `(null)`, a `char` pointer its text, and any other pointer its address
  /// as Hex.
  template <typename Pointee>
  LogWriter& operator<<(Pointee* pointer) noexcept {
    static_assert(!std::is_function_v<Pointee>,
                  "Ulog does not log function addresses or stream manipulators; cast the pointer "
                  "to const void* to log its address.");
    if (pointer == nullptr) {
      return WriteText("(null)");
    }
    if constexpr (std::is_same_v<std::remove_cv_t<Pointee>, char>) {
      return WriteText(std::string_view{pointer});
    } else {
      return *this << Hex{pointer};
    }
  }

  LogWriter& operator<<(bool value) noexcept {
    return WriteText(value ? std::string_view{"true"} : std::string_view{"false"});
  }

  /// Floating-point values use fmt's shortest round-trip form, such as `1.5`, `1e+300`, `nan`, and
  /// `-inf`.
  LogWriter& operator<<(float value) noexcept { return WriteFloatingPoint(value); }
  LogWriter& operator<<(double value) noexcept { return WriteFloatingPoint(value); }
  LogWriter& operator<<(long double value) noexcept { return WriteFloatingPoint(value); }

  /// Writes `what()` and the dynamic type, as in `boom (std::runtime_error)`.
  LogWriter& operator<<(const std::exception& exception) noexcept {
    if (CanWrite()) {
      const char* const text = exception.what();
      AppendText(text != nullptr ? std::string_view{text} : std::string_view{});
      AppendText(" (");
      AppendText(detail::TypeName(typeid(exception)));
      AppendText(")");
    }
    return *this;
  }

  LogWriter& operator<<(Hex hex) noexcept {
    std::array<char, 18> text{'0', 'x'};
    for (std::size_t index = 0; index < 16; ++index) {
      text[17 - index] = kHexDigits[(hex.value >> (4U * index)) & 0xFU];
    }
    return WriteText(std::string_view{text.data(), text.size()});
  }

  LogWriter& operator<<(HexShort hex) noexcept {
    std::array<char, 16> text{};
    std::size_t begin = text.size();
    std::uint64_t value = hex.value;
    do {
      --begin;
      text[begin] = kHexDigits[value & 0xFU];
      value >>= 4U;
    } while (value != 0);
    return WriteText(std::string_view{text.data() + begin, text.size() - begin});
  }

  LogWriter& operator<<(Quoted quoted) noexcept {
    if (CanWrite()) {
      AppendText("\"");
      std::size_t run_begin = 0;
      for (std::size_t index = 0; index < quoted.string.size(); ++index) {
        const char byte = quoted.string[index];
        if (byte == '"' || byte == '\\') {
          AppendText(quoted.string.substr(run_begin, index - run_begin));
          AppendText("\\");
          run_begin = index;
        }
      }
      AppendText(quoted.string.substr(run_begin));
      AppendText("\"");
    }
    return *this;
  }

  /// Writes `category:value (message)`; the message text may allocate.
  LogWriter& operator<<(const std::error_code& code) noexcept {
    if (CanWrite()) {
      try {
        *this << code.category().name() << ':' << code.value() << " (" << code.message() << ')';
      } catch (...) {
        failed_ = true;
      }
    }
    return *this;
  }

  LogWriter& operator<<(std::chrono::nanoseconds value) noexcept {
    return *this << value.count() << "ns";
  }
  LogWriter& operator<<(std::chrono::microseconds value) noexcept {
    return *this << value.count() << "us";
  }
  LogWriter& operator<<(std::chrono::milliseconds value) noexcept {
    return *this << value.count() << "ms";
  }
  LogWriter& operator<<(std::chrono::seconds value) noexcept {
    return *this << value.count() << "s";
  }
  LogWriter& operator<<(std::chrono::minutes value) noexcept {
    return *this << value.count() << "min";
  }
  LogWriter& operator<<(std::chrono::hours value) noexcept { return *this << value.count() << "h"; }

  /// A system-clock time point is written in UTC as `2000-01-02T03:04:05.123456+0000`, with
  /// trailing fractional zeros and an all-zero fraction omitted.
  template <typename Duration>
  LogWriter& operator<<(
      std::chrono::time_point<std::chrono::system_clock, Duration> time) noexcept {
    if (CanWrite()) {
      using namespace std::chrono;
      using Fine = std::common_type_t<Duration, seconds>;
      const Fine since_epoch = time.time_since_epoch();
      const auto day_count = floor<days>(since_epoch);
      const year_month_day date{sys_days{day_count}};
      const Fine in_day = since_epoch - day_count;
      const auto whole_seconds = floor<seconds>(in_day);
      const auto fraction = duration_cast<nanoseconds>(in_day - whole_seconds).count();
      const auto second_of_day = whole_seconds.count();
      Format("{:04}-{:02}-{:02}T{:02}:{:02}:{:02}", static_cast<int>(date.year()),
             static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
             second_of_day / 3'600, second_of_day / 60 % 60, second_of_day % 60);
      if (fraction != 0) {
        std::array<char, 10> digits{'.'};
        auto remaining = fraction;
        for (std::size_t index = 9; index > 0; --index) {
          digits[index] = static_cast<char>('0' + remaining % 10);
          remaining /= 10;
        }
        std::size_t size = digits.size();
        while (digits[size - 1] == '0') {
          --size;
        }
        AppendText(std::string_view{digits.data(), size});
      }
      AppendText("+0000");
    }
    return *this;
  }

  /// An atomic writes one load of its value.
  template <typename Value>
  LogWriter& operator<<(const std::atomic<Value>& value) {
    return *this << value.load();
  }

  /// An empty optional writes `(none)`.
  template <typename Value>
  LogWriter& operator<<(const std::optional<Value>& value) {
    return value ? (*this << *value) : WriteText("(none)");
  }

  /// Tuples and pairs write `(first, second, ...)`.
  template <typename... Values>
  LogWriter& operator<<(const std::tuple<Values...>& values) {
    WriteText("(");
    std::apply(
        [this](const auto&... elements) {
          bool first = true;
          ((first ? static_cast<void>(first = false) : static_cast<void>(WriteText(", ")),
            *this << elements),
           ...);
        },
        values);
    return WriteText(")");
  }

  template <typename First, typename Second>
  LogWriter& operator<<(const std::pair<First, Second>& values) {
    return WriteText("(") << values.first << ", " << values.second << ")";
  }

  /// A lazy writer: an invocable that takes the writer runs only while the writer holds an
  /// admitted Record. Its exceptions propagate and abandon the Record.
  template <typename Writer>
    requires std::is_invocable_v<Writer&, LogWriter&>
  LogWriter& operator<<(Writer&& write) {
    if (CanWrite()) {
      std::invoke(write, *this);
    }
    return *this;
  }

  /// Every other value, in this order: text usable as std::string_view; integers in decimal; an
  /// exception's message and type; an ostream-writable value through its stream operator; a range
  /// as `[a, b]` (maps as `[k: v]`, string elements quoted); and an fmt-formattable value. A type
  /// can take precedence by providing `ulog::LogWriter& operator<<(ulog::LogWriter&, const T&)`.
  template <typename Value>
    requires(!std::is_invocable_v<Value&, LogWriter&>)
  LogWriter& operator<<(const Value& value) noexcept {
    if constexpr (detail::StreamText<Value>) {
      if (CanWrite()) {
        try {
          AppendText(std::string_view{value});
        } catch (...) {
          failed_ = true;
        }
      }
    } else if constexpr (std::integral<Value>) {
      static_assert(!detail::TextUnit<Value>,
                    "Ulog streams only char text; convert wide and UTF character types to UTF-8 "
                    "text, or cast them to an integer to log their code.");
      std::array<char, 24> digits{};
      const auto written = std::to_chars(digits.data(), digits.data() + digits.size(), value);
      WriteText(
          std::string_view{digits.data(), static_cast<std::size_t>(written.ptr - digits.data())});
    } else if constexpr (std::is_base_of_v<std::exception, Value>) {
      *this << static_cast<const std::exception&>(value);
    } else if constexpr (detail::OstreamWritable<Value>) {
      WriteStreamed(value);
    } else if constexpr (std::ranges::range<const Value>) {
      WriteRange(value);
    } else if constexpr (fmt::is_formattable<Value>::value) {
      Format("{}", value);
    } else {
      static_assert(detail::kUnsupportedValue<Value>,
                    "Ulog cannot write this type; provide "
                    "ulog::LogWriter& operator<<(ulog::LogWriter&, const T&), an ostream "
                    "operator<<, or an fmt::formatter for it.");
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

  static constexpr std::string_view kHexDigits{"0123456789ABCDEF"};

  /// Writes fmt-free text into a std::ostream in chunks.
  class StreamBuffer final : public std::streambuf {
   public:
    explicit StreamBuffer(LogWriter& writer) noexcept : writer_(&writer) {
      setp(bytes_.data(), bytes_.data() + bytes_.size());
    }

    void Flush() noexcept {
      writer_->AppendText(std::string_view{pbase(), static_cast<std::size_t>(pptr() - pbase())});
      setp(bytes_.data(), bytes_.data() + bytes_.size());
    }

   protected:
    int_type overflow(int_type character) override {
      Flush();
      if (!traits_type::eq_int_type(character, traits_type::eof())) {
        *pptr() = traits_type::to_char_type(character);
        pbump(1);
      }
      return traits_type::not_eof(character);
    }

    int sync() override {
      Flush();
      return 0;
    }

   private:
    LogWriter* writer_;
    std::array<char, 128> bytes_{};
  };

  [[nodiscard]] bool CanWrite() const noexcept { return record_.finish != nullptr && !failed_; }

  void AppendText(std::string_view text) noexcept {
    if (!text.empty() && !record_.append_text(record_.context, text)) {
      text_full_ = true;
    }
  }

  LogWriter& WriteText(std::string_view text) noexcept {
    if (CanWrite()) {
      AppendText(text);
    }
    return *this;
  }

  template <typename Value>
  void WriteStreamed(const Value& value) noexcept {
    if (CanWrite()) {
      try {
        StreamBuffer buffer{*this};
        std::ostream stream{&buffer};
        stream << value;
        buffer.Flush();
      } catch (...) {
        failed_ = true;
      }
    }
  }

  /// Writes `[a, b]`; it stops at the Record bound, after which nothing more fits.
  template <typename Range>
  void WriteRange(const Range& range) noexcept {
    using Element = std::remove_cv_t<std::ranges::range_value_t<const Range>>;
    static_assert(!std::is_same_v<Element, char>,
                  "Convert a range of char to std::string_view, or provide "
                  "ulog::LogWriter& operator<<(ulog::LogWriter&, const T&) for the type.");
    if (!CanWrite()) {
      return;
    }
    WriteText("[");
    try {
      bool first = true;
      for (const auto& element : range) {
        if (!CanWrite() || text_full_) {
          break;
        }
        if (!first) {
          WriteText(", ");
        }
        first = false;
        if constexpr (detail::MapRange<Range>) {
          WriteElement(element.first);
          WriteText(": ");
          WriteElement(element.second);
        } else {
          WriteElement(element);
        }
      }
    } catch (...) {
      failed_ = true;
    }
    WriteText("]");
  }

  template <typename Element>
  void WriteElement(const Element& element) {
    if constexpr (detail::StreamText<Element>) {
      *this << Quoted{std::string_view{element}};
    } else {
      *this << element;
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
  /// Set once the Record bound cut the text; later text cannot fit.
  bool text_full_{false};
};

}  // namespace ulog
