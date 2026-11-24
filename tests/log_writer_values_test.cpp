#include <fmt/format.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <ulog/log.hpp>
#include <ulog/log_writer.hpp>
#include <utility>
#include <vector>

#include "producer/producer_kernel.hpp"

namespace ulog::test {

struct CustomError final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

/// Writes itself through an ostream operator.
struct Streamed final {
  int value;
  friend std::ostream& operator<<(std::ostream& stream, const Streamed& streamed) {
    return stream << "streamed<" << streamed.value << '>';
  }
};

/// Both a range and ostream-writable: the stream operator wins.
struct StreamedRange final {
  std::vector<int> values{1, 2};
  [[nodiscard]] auto begin() const { return values.begin(); }
  [[nodiscard]] auto end() const { return values.end(); }
  friend std::ostream& operator<<(std::ostream& stream, const StreamedRange&) {
    return stream << "stream wins";
  }
};

/// Has a native writer overload and an ostream operator: the native overload wins.
struct Point final {
  int x;
  int y;
  friend std::ostream& operator<<(std::ostream& stream, const Point&) {
    return stream << "ostream";
  }
};

LogWriter& operator<<(LogWriter& writer, const Point& point) {
  return writer << '(' << point.x << ';' << point.y << ')';
}

/// Writes a borrowed string through an ostream operator.
struct LongStreamed final {
  const std::string* text;
  friend std::ostream& operator<<(std::ostream& stream, const LongStreamed& value) {
    return stream << *value.text;
  }
};

/// Formattable only through fmt.
struct Formatted final {
  int value;
};

}  // namespace ulog::test

template <>
struct fmt::formatter<ulog::test::Formatted> : fmt::formatter<int> {
  auto format(const ulog::test::Formatted& formatted, fmt::format_context& context) const {
    return fmt::format_to(context.out(), "fmt<{}>", formatted.value);
  }
};

namespace ulog::test {
namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using detail::producer::ConsumeStatus;
using detail::producer::KernelConfig;
using detail::producer::ProducerKernel;
using detail::producer::RecordView;

constexpr SourceLocation kSource = SourceLocation::Custom("values.cpp", "Write", 1);

[[nodiscard]] KernelConfig ValuesConfig(Level threshold = Level::kTrace,
                                        std::size_t maximum_record_bytes = 4'096) noexcept {
  return KernelConfig{
      .threshold = threshold,
      .payload_capacity_bytes = maximum_record_bytes * 2,
      .maximum_record_bytes = maximum_record_bytes,
      .producer_slots = 1,
      .ingress_cells = 2,
  };
}

struct Observed final {
  bool present{false};
  std::string message;
  bool truncated{false};
  std::size_t serialized_bytes{0};
  std::size_t truncation_markers{0};
};

void Observe(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& observed = *static_cast<Observed*>(context);
  try {
    observed.present = true;
    observed.message = record.message();
    observed.truncated = record.truncated();
    observed.serialized_bytes = record.serialized_bytes();
    auto cursor = record.Fields();
    for (std::size_t index = 0; index < record.field_count(); ++index) {
      const auto field = cursor.Next();
      if (field && field->key() == "ulog.truncated") {
        ++observed.truncation_markers;
      }
    }
  } catch (...) {
    observed.present = false;
  }
}

/// Writes `values` through one LogWriter and returns the Record's message.
class Harness final {
 public:
  explicit Harness(std::size_t maximum_record_bytes = 4'096)
      : kernel_(ValuesConfig(Level::kTrace, maximum_record_bytes)),
        producer_(kernel_.TryRegisterProducer()) {}

  template <typename... Values>
  [[nodiscard]] std::string Write(const Values&... values) {
    {
      LogWriter writer{kernel_.GetLogger(), Level::kInfo, kSource};
      (writer << ... << values);
    }
    return TakeRecord().message;
  }

  [[nodiscard]] Observed TakeRecord() {
    Observed observed;
    if (kernel_.TryConsume(&observed, &Observe) != ConsumeStatus::kRecord) {
      ADD_FAILURE() << "no Record";
    }
    return observed;
  }

  [[nodiscard]] Logger GetLogger() noexcept { return kernel_.GetLogger(); }

 private:
  ProducerKernel kernel_;
  ProducerKernel::ProducerRegistration producer_;
};

TEST(LogWriterValues, PointersHexAndQuotedHelpersUseBaselineForms) {
  Harness harness;
  int value = 0;
  int* const pointer = &value;
  const int* const null_pointer = nullptr;
  char text[] = "mutable";
  char* const mutable_text = text;
  const char* const null_text = nullptr;
  const void* const address = pointer;
  const std::string expected_address =
      fmt::format("0x{:016X}", reinterpret_cast<std::uintptr_t>(pointer));

  EXPECT_EQ(harness.Write(null_pointer, '|', nullptr, '|', null_text), "(null)|(null)|(null)");
  EXPECT_EQ(harness.Write(mutable_text), "mutable");
  EXPECT_EQ(harness.Write(pointer), expected_address);
  EXPECT_EQ(harness.Write(address), expected_address);
  EXPECT_EQ(harness.Write(Hex{0x1FU}, '|', Hex{std::uint64_t{0xFFFFFFFFFFFFFFFFU}}),
            "0x000000000000001F|0xFFFFFFFFFFFFFFFF");
  EXPECT_EQ(harness.Write(HexShort{0x1FU}, '|', HexShort{0U}, '|', HexShort{pointer}),
            fmt::format("1F|0|{:X}", reinterpret_cast<std::uintptr_t>(pointer)));
  EXPECT_EQ(harness.Write(Quoted{"a\"b\\c"}, '|', Quoted{""}, '|', Quoted{"\xC3\xA9"}),
            "\"a\\\"b\\\\c\"|\"\"|\"\xC3\xA9\"");
}

TEST(LogWriterValues, AtomicsOptionalsAndErrorCodesUseBaselineForms) {
  Harness harness;
  const std::atomic<int> counter{5};
  const std::atomic<bool> flag{true};
  const std::optional<int> empty;
  const std::optional<std::string> text{"text"};
  const std::error_code code = std::make_error_code(std::errc::invalid_argument);

  EXPECT_EQ(harness.Write(counter, '|', flag), "5|true");
  EXPECT_EQ(harness.Write(empty, '|', text, '|', std::optional<int>{7}), "(none)|text|7");
  EXPECT_EQ(harness.Write(code), std::string{code.category().name()} + ':' +
                                     std::to_string(code.value()) + " (" + code.message() + ')');
}

TEST(LogWriterValues, DurationsUseUnitSuffixesAndTimePointsUseUtc) {
  Harness harness;
  EXPECT_EQ(harness.Write(1ns, ' ', 2us, ' ', 3ms, ' ', 4s, ' ', 5min, ' ', 6h, ' ', -7s),
            "1ns 2us 3ms 4s 5min 6h -7s");

  using namespace std::chrono;
  const sys_days day = sys_days{year{2000} / January / 2};
  const auto time = time_point_cast<system_clock::duration>(day + 3h + 4min + 5s + 123'456us);
  EXPECT_EQ(harness.Write(time), "2000-01-02T03:04:05.123456+0000");
  EXPECT_EQ(harness.Write(time_point_cast<system_clock::duration>(day + 3h + 4min + 5s)),
            "2000-01-02T03:04:05+0000");
  EXPECT_EQ(harness.Write(sys_seconds{day + 59s}), "2000-01-02T00:00:59+0000");
  EXPECT_EQ(harness.Write(sys_time<nanoseconds>{day + 1'234'567'000ns}),
            "2000-01-02T00:00:01.234567+0000");
  EXPECT_EQ(
      harness.Write(sys_time<milliseconds>{sys_days{year{1969} / December / 31} + 86'399'500ms}),
      "1969-12-31T23:59:59.5+0000");
}

TEST(LogWriterValues, RangesMapsTuplesAndPairsUseBaselineForms) {
  Harness harness;
  const std::vector<int> numbers{1, 2, 3};
  const std::vector<int> none;
  const std::vector<std::string> strings{"a", "b\"c"};
  const std::map<std::string, int> map{{"a", 1}, {"b", 2}};
  const std::map<int, std::vector<std::string_view>> nested_map{{1, {"x"}}, {2, {}}};
  const std::vector<std::vector<int>> nested{{1, 2}, {}};
  const std::set<int> set{3, 1};
  const std::array<double, 2> array{0.5, -1.0};

  EXPECT_EQ(harness.Write(numbers, ' ', none), "[1, 2, 3] []");
  EXPECT_EQ(harness.Write(strings), "[\"a\", \"b\\\"c\"]");
  EXPECT_EQ(harness.Write(map), "[\"a\": 1, \"b\": 2]");
  EXPECT_EQ(harness.Write(nested_map), "[1: [\"x\"], 2: []]");
  EXPECT_EQ(harness.Write(nested, ' ', set, ' ', array), "[[1, 2], []] [1, 3] [0.5, -1]");
  EXPECT_EQ(harness.Write(std::tuple<int, std::string, double>{1, "x", 2.5}, ' ', std::tuple<>{}),
            "(1, x, 2.5) ()");
  EXPECT_EQ(harness.Write(std::pair<int, const char*>{1, "y"}), "(1, y)");
}

TEST(LogWriterValues, OstreamFmtAndNativeOverloadsFollowThePrecedence) {
  Harness harness;
  EXPECT_EQ(harness.Write(Streamed{4}), "streamed<4>");
  EXPECT_EQ(harness.Write(StreamedRange{}), "stream wins");
  EXPECT_EQ(harness.Write(Point{1, 2}), "(1;2)");
  EXPECT_EQ(harness.Write(std::vector<Point>{{1, 2}, {3, 4}}), "[(1;2), (3;4)]");
  EXPECT_EQ(harness.Write(Formatted{9}), "fmt<9>");
  // Longer than the ostream adapter's 128-byte chunk.
  const std::string long_text(300, 'z');
  EXPECT_EQ(harness.Write(LongStreamed{&long_text}, '!'), long_text + "!");
}

TEST(LogWriterValues, LazyWritersRunOnlyAfterAdmission) {
  ProducerKernel kernel{ValuesConfig(Level::kInfo)};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  int calls = 0;
  const auto lazy = [&calls](LogWriter& writer) {
    ++calls;
    writer << "lazy " << calls;
  };
  LOG_DEBUG_TO(kernel.GetLogger()) << lazy;
  LOG_INFO_TO(GetNullLogger()) << lazy;
  {
    LogWriter filtered{kernel.GetLogger(), Level::kTrace, kSource};
    filtered << lazy;
  }
  EXPECT_EQ(calls, 0);
  LOG_INFO_TO(kernel.GetLogger()) << "value: " << lazy;
  EXPECT_EQ(calls, 1);
  Observed observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &Observe), ConsumeStatus::kRecord);
  EXPECT_EQ(observed.message, "value: lazy 1");
}

TEST(LogWriterValues, ExceptionsWriteTheirMessageAndDynamicType) {
  Harness harness;
  EXPECT_EQ(harness.Write(std::runtime_error{"boom"}), "boom (std::runtime_error)");
  EXPECT_EQ(harness.Write(CustomError{"custom"}), "custom (ulog::test::CustomError)");
  try {
    throw CustomError{"thrown"};
  } catch (const std::exception& error) {
    EXPECT_EQ(harness.Write(error), "thrown (ulog::test::CustomError)");
  }
}

[[nodiscard]] bool IsValidUtf8(std::string_view text) {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto lead = static_cast<unsigned char>(text[index]);
    std::size_t length = 1;
    if (lead >= 0xF0) {
      length = 4;
    } else if (lead >= 0xE0) {
      length = 3;
    } else if (lead >= 0xC0) {
      length = 2;
    } else if (lead >= 0x80) {
      return false;
    }
    if (index + length > text.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset < length; ++offset) {
      if ((static_cast<unsigned char>(text[index + offset]) & 0xC0U) != 0x80U) {
        return false;
      }
    }
    index += length;
  }
  return true;
}

TEST(LogWriterValues, NestedRangesStopAtTheRecordBoundWithValidUtf8AndOneMarker) {
  // Strings of two-, three-, and four-byte code points nested in maps and vectors.
  std::map<std::string, std::vector<std::string>> nested;
  for (int key = 0; key < 20; ++key) {
    nested.emplace("k\xC3\xA9" + std::to_string(key),
                   std::vector<std::string>{"\xE2\x82\xAC\xF0\x9F\x98\x80", "q\"\\"});
  }
  Harness large{16'384};
  const std::string full = large.Write(nested);
  ASSERT_FALSE(full.empty());
  constexpr std::size_t kRecordBytes = 256;
  for (std::size_t prefix_bytes = 0; prefix_bytes < 180; ++prefix_bytes) {
    SCOPED_TRACE(prefix_bytes);
    Harness small{kRecordBytes};
    const std::string prefix(prefix_bytes, 'a');
    { LogWriter{small.GetLogger(), Level::kInfo, kSource} << prefix << nested << "tail"; }
    const Observed observed = small.TakeRecord();
    ASSERT_TRUE(observed.present);
    EXPECT_LE(observed.serialized_bytes, kRecordBytes);
    EXPECT_TRUE(IsValidUtf8(observed.message));
    EXPECT_EQ((prefix + full).substr(0, observed.message.size()), observed.message);
    EXPECT_TRUE(observed.truncated);
    EXPECT_EQ(observed.truncation_markers, 1U);
  }
}

}  // namespace
}  // namespace ulog::test
