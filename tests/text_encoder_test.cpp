#include "encoding/text_encoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <ulog/format.hpp>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>
#include <ulog/timestamp.hpp>
#include <vector>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::encoding {
namespace {

using namespace std::string_view_literals;
using producer::BuildOperation;
using producer::BuildStatus;
using producer::ConsumeStatus;
using producer::KernelConfig;
using producer::kNull;
using producer::ProducerKernel;
using producer::PublishOutcome;
using producer::RecordAppender;
using producer::RecordView;

constexpr std::string_view kUnicodeMessage{
    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|\r\n\0\t\\|literal:\\r\\n\\0\\t\\\\"sv};
constexpr char kEscapedKey[] = "A=.\t\r\n\0\\";
constexpr char kEscapedValue[] = "\xC3\xA9=ok:still|\t\r\n\0\\";
constexpr std::string_view kUnicodeFrame{
    "tskv\ta\\=."
    "\\t\\r\\n\\0\\\\=\xC3\xA9=ok:still|\\t\\r\\n\\0\\\\\ttext="
    "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|"
    "\\r\\n\\0\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\n"};

[[nodiscard]] KernelConfig TestKernelConfig() noexcept {
  return KernelConfig{
      .threshold = Level::kTrace,
      .payload_capacity_bytes = 1'024,
      .maximum_record_bytes = 1'024,
      .producer_slots = 1,
      .ingress_cells = 1,
  };
}

struct UnicodeBuild final {
  static BuildStatus Invoke(void*, RecordAppender& writer) {
    const auto message = writer.Append(kUnicodeMessage);
    const bool field = writer.AddField(std::string_view{kEscapedKey, sizeof(kEscapedKey) - 1U},
                                       std::string_view{kEscapedValue, sizeof(kEscapedValue) - 1U});
    return !message.truncated && field ? BuildStatus::kComplete : BuildStatus::kInvalid;
  }
};

struct ScalarBuild final {
  static BuildStatus Invoke(void*, RecordAppender& writer) {
    const auto message = writer.Append("scalars");
    const bool fields = writer.AddField("string", "alpha") &&
                        writer.AddField("signed", std::int64_t{-42}) &&
                        writer.AddField("unsigned", std::numeric_limits<std::uint64_t>::max()) &&
                        writer.AddField("float", 1.5) && writer.AddField("double", -2.25) &&
                        writer.AddField("bool_true", true) &&
                        writer.AddField("bool_false", false) && writer.AddField("optional", kNull);
    return !message.truncated && fields ? BuildStatus::kComplete : BuildStatus::kInvalid;
  }
};

struct EncodeCapture final {
  std::array<char, 256> complete{};
  std::array<char, 100> too_small{};
  EncodeResult complete_result{};
  EncodeResult too_small_result{};
};

void EncodeWithExactBoundary(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& capture = *static_cast<EncodeCapture*>(context);
  capture.too_small.fill('#');
  capture.complete_result = EncodeRecord(Format::kRaw, record, capture.complete);
  capture.too_small_result = EncodeRecord(Format::kRaw, record, capture.too_small);
}

void EncodeComplete(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& capture = *static_cast<EncodeCapture*>(context);
  capture.complete_result = EncodeRecord(Format::kRaw, record, capture.complete);
}

TEST(RawEncoder, UnicodeFieldAndMessageMatchTheCommittedCorpusLiteralAtomically) {
  ProducerKernel kernel{TestKernelConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const auto published =
      kernel.TryPublish(producer, Level::kInfo, SourceLocation::Custom("raw.cpp", "Build", 1),
                        BuildOperation{nullptr, &UnicodeBuild::Invoke});
  ASSERT_EQ(published.outcome, PublishOutcome::kAccepted);

  EncodeCapture capture;
  ASSERT_EQ(kernel.TryConsume(&capture, &EncodeWithExactBoundary), ConsumeStatus::kRecord);
  ASSERT_TRUE(capture.complete_result.complete);
  EXPECT_EQ(capture.complete_result.encoded_bytes, 101U);
  const std::string_view complete_frame{capture.complete.data(),
                                        capture.complete_result.encoded_bytes};
  EXPECT_EQ(complete_frame, kUnicodeFrame);
  EXPECT_FALSE(capture.too_small_result.complete);
  EXPECT_EQ(capture.too_small_result.encoded_bytes, 0U);
  EXPECT_TRUE(std::ranges::all_of(capture.too_small, [](char byte) { return byte == '#'; }));
}

TEST(RawEncoder, OrderedScalarFieldsUseBaselineTextRepresentations) {
  ProducerKernel kernel{TestKernelConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const auto published =
      kernel.TryPublish(producer, Level::kInfo, SourceLocation::Custom("raw.cpp", "Build", 2),
                        BuildOperation{nullptr, &ScalarBuild::Invoke});
  ASSERT_EQ(published.outcome, PublishOutcome::kAccepted);

  EncodeCapture capture;
  ASSERT_EQ(kernel.TryConsume(&capture, &EncodeComplete), ConsumeStatus::kRecord);
  ASSERT_TRUE(capture.complete_result.complete);
  const std::string_view complete_frame{capture.complete.data(),
                                        capture.complete_result.encoded_bytes};
  EXPECT_EQ(complete_frame,
            "tskv\tstring=alpha\tsigned=-42\tunsigned=18446744073709551615\tfloat=1.5"
            "\tdouble=-2.25\tbool_true=1\tbool_false=0\toptional=null\ttext=scalars\n");
}

TEST(RawEncoder, MaximumOutputBoundIncludesFullEscapingExpansionAndFraming) {
  EXPECT_EQ(MaximumEncodedBytes(Format::kRaw, 512U), 1'035U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kRaw, std::numeric_limits<std::size_t>::max()), 0U);
}

// 2000-01-02T03:04:05.123456 UTC, the instant of the committed TSKV and LTSV corpus.
constexpr std::int64_t kCorpusInstant = 946'782'245'123'456;

[[nodiscard]] std::int64_t CorpusClock(void*) noexcept { return kCorpusInstant; }

template <typename Build>
BuildStatus InvokeBuild(void* context, RecordAppender& writer) {
  return (*static_cast<Build*>(context))(writer) ? BuildStatus::kComplete : BuildStatus::kInvalid;
}

struct EncodeRequest final {
  Format format{Format::kRaw};
  std::span<char> output;
  EncodeResult result{};
};

void EncodeInto(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& request = *static_cast<EncodeRequest*>(context);
  request.result = EncodeRecord(request.format, record, request.output);
}

constexpr std::size_t kRecordBytes = 1'024;

/// Publishes one Record of at most kRecordBytes at the corpus instant and encodes it into
/// `output_bytes`. An incomplete encoding must leave the output untouched.
template <typename Build>
[[nodiscard]] std::optional<std::string> Encode(Format format, Level level,
                                                const SourceLocation& source, Build build,
                                                std::size_t output_bytes = 4'096) {
  ProducerKernel kernel{KernelConfig{
                            .threshold = Level::kTrace,
                            .payload_capacity_bytes = kRecordBytes,
                            .maximum_record_bytes = kRecordBytes,
                            .producer_slots = 1,
                            .ingress_cells = 1,
                        },
                        EventClock{.context = nullptr, .now = &CorpusClock}};
  auto producer = kernel.TryRegisterProducer();
  // The context points at one callable object, even when `build` is a function pointer.
  auto call = [&build](RecordAppender& writer) { return build(writer); };
  const auto published = kernel.TryPublish(producer, level, source,
                                           BuildOperation{&call, &InvokeBuild<decltype(call)>});
  EXPECT_EQ(published.outcome, PublishOutcome::kAccepted);
  std::vector<char> output(output_bytes, '#');
  EncodeRequest request{.format = format, .output = output};
  EXPECT_EQ(kernel.TryConsume(&request, &EncodeInto), ConsumeStatus::kRecord);
  if (!request.result.complete) {
    EXPECT_EQ(request.result.encoded_bytes, 0U);
    EXPECT_TRUE(std::ranges::all_of(output, [](char byte) { return byte == '#'; }));
    return std::nullopt;
  }
  return std::string{output.data(), request.result.encoded_bytes};
}

[[nodiscard]] std::string Timestamp() {
  return std::string{FormatLocalTimestamp(kCorpusInstant).View()};
}

[[nodiscard]] bool BuildScalars(RecordAppender& writer) {
  return ScalarBuild::Invoke(nullptr, writer) == BuildStatus::kComplete;
}

[[nodiscard]] bool BuildUnicode(RecordAppender& writer) {
  return UnicodeBuild::Invoke(nullptr, writer) == BuildStatus::kComplete;
}

[[nodiscard]] bool BuildEmpty(RecordAppender&) { return true; }

TEST(TextEncoder, TskvWritesThePreambleBeforeFieldsAndText) {
  const auto source = SourceLocation::Custom("src/probe.cpp", "BaselineTextProbe", 321);
  EXPECT_EQ(Encode(Format::kTskv, Level::kInfo, source, &BuildScalars),
            "tskv\ttimestamp=" + Timestamp() +
                "\tlevel=INFO\tmodule=BaselineTextProbe ( src/probe.cpp:321 )\tstring=alpha"
                "\tsigned=-42\tunsigned=18446744073709551615\tfloat=1.5\tdouble=-2.25"
                "\tbool_true=1\tbool_false=0\toptional=null\ttext=scalars\n");
  EXPECT_EQ(Encode(Format::kTskv, Level::kInfo, source, &BuildUnicode),
            "tskv\ttimestamp=" + Timestamp() +
                "\tlevel=INFO\tmodule=BaselineTextProbe ( src/probe.cpp:321 )" +
                std::string{kUnicodeFrame.substr(4)});
}

TEST(TextEncoder, LtsvOmitsThePrefixAndSeparatesKeysWithAColon) {
  const auto source = SourceLocation::Custom("src/probe.cpp", "BaselineTextProbe", 321);
  EXPECT_EQ(Encode(Format::kLtsv, Level::kInfo, source, &BuildScalars),
            "timestamp:" + Timestamp() +
                "\tlevel:INFO\tmodule:BaselineTextProbe ( src/probe.cpp:321 )\tstring:alpha"
                "\tsigned:-42\tunsigned:18446744073709551615\tfloat:1.5\tdouble:-2.25"
                "\tbool_true:1\tbool_false:0\toptional:null\ttext:scalars\n");
  // LTSV uses the TSKV escaping: `=` stays escaped in keys and `:` is never escaped.
  EXPECT_EQ(Encode(Format::kLtsv, Level::kInfo, source, &BuildUnicode),
            "timestamp:" + Timestamp() +
                "\tlevel:INFO\tmodule:BaselineTextProbe ( src/probe.cpp:321 )"
                "\ta\\=.\\t\\r\\n\\0\\\\:\xC3\xA9=ok:still|\\t\\r\\n\\0\\\\\ttext:"
                "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|"
                "\\r\\n\\0\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\n");
}

TEST(TextEncoder, EveryLevelHasItsUppercaseNameAndEmptyTextKeepsItsKey) {
  const auto source = SourceLocation::Custom("a.cpp", "F", 7);
  constexpr std::array kLevels{Level::kTrace,   Level::kDebug, Level::kInfo,
                               Level::kWarning, Level::kError, Level::kCritical};
  constexpr std::array<std::string_view, 6> kNames{"TRACE",   "DEBUG", "INFO",
                                                   "WARNING", "ERROR", "CRITICAL"};
  for (std::size_t index = 0; index < kLevels.size(); ++index) {
    const std::string name{kNames[index]};
    EXPECT_EQ(
        Encode(Format::kTskv, kLevels[index], source, &BuildEmpty),
        "tskv\ttimestamp=" + Timestamp() + "\tlevel=" + name + "\tmodule=F ( a.cpp:7 )\ttext=\n");
    EXPECT_EQ(Encode(Format::kLtsv, kLevels[index], source, &BuildEmpty),
              "timestamp:" + Timestamp() + "\tlevel:" + name + "\tmodule:F ( a.cpp:7 )\ttext:\n");
  }
}

TEST(TextEncoder, ModuleEscapesTheFunctionAndPathAsAValue) {
  const auto source = SourceLocation::Custom("dir\\x=\ty.cpp", "f(\n)", 4'294'967'295U);
  EXPECT_EQ(Encode(Format::kTskv, Level::kError, source, &BuildEmpty),
            "tskv\ttimestamp=" + Timestamp() +
                "\tlevel=ERROR\tmodule=f(\\n) ( dir\\\\x=\\ty.cpp:4294967295 )\ttext=\n");
}

TEST(TextEncoder, AFrameIsWrittenWhollyOrNotAtAll) {
  const auto source = SourceLocation::Custom("a.cpp", "F", 7);
  for (const Format format : {Format::kTskv, Format::kLtsv, Format::kRaw}) {
    const std::string frame =
        Encode(format, Level::kInfo, source, &BuildUnicode).value_or(std::string{});
    ASSERT_FALSE(frame.empty()) << ToString(format);
    EXPECT_EQ(Encode(format, Level::kInfo, source, &BuildUnicode, frame.size()), frame);
    EXPECT_EQ(Encode(format, Level::kInfo, source, &BuildUnicode, frame.size() - 1U), std::nullopt);
  }
}

TEST(TextEncoder, WorstCaseRecordFitsTheDerivedBound) {
  // Every source, key, and message byte needs escaping, and numbers take their longest form.
  const std::string path(200, '\t');
  const std::string function(200, '\\');
  const auto source = SourceLocation::Custom(path, function, 4'294'967'295U);
  const auto worst = [](RecordAppender& writer) {
    for (char suffix = 'a'; suffix < 'i'; ++suffix) {
      if (!writer.AddField(std::string{'\n', suffix}, -2.2250738585072014e-308)) {
        return false;
      }
    }
    static_cast<void>(writer.Append(std::string(kRecordBytes, '\n')));
    return true;
  };
  for (const Format format : {Format::kTskv, Format::kLtsv, Format::kRaw}) {
    const std::size_t bound = MaximumEncodedBytes(format, kRecordBytes);
    const std::string frame =
        Encode(format, Level::kCritical, source, worst, bound).value_or(std::string{});
    ASSERT_FALSE(frame.empty()) << ToString(format);
    EXPECT_LE(frame.size(), bound);
    // The truncated message filled the Record, so the bound exceeds the frame only by the
    // doubled serialized bytes the frame does not print: the 56-byte Record header, the 24-byte
    // metadata of the eight fields and the truncation marker, and for Raw the 400 source bytes.
    const std::size_t unprinted = 56U + 9U * 24U + (format == Format::kRaw ? 400U : 0U);
    EXPECT_GE(frame.size() + 2U * unprinted, bound) << ToString(format);
  }
}

TEST(TextEncoder, BoundsAreDerivedPerFormat) {
  EXPECT_EQ(MaximumEncodedBytes(Format::kRaw, 512U), 11U + 1'024U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kTskv, 512U), 87U + 1'024U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kLtsv, 512U), 82U + 1'024U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kTskv, std::numeric_limits<std::size_t>::max()), 0U);
}

TEST(TextEncoder, FormatsWithoutAnEncoderNeverWrite) {
  const auto source = SourceLocation::Custom("a.cpp", "F", 7);
  for (const Format format :
       {Format::kJson, Format::kJsonYaDeploy, std::bit_cast<Format>(std::uint8_t{9})}) {
    EXPECT_FALSE(IsEncodable(format));
    EXPECT_EQ(MaximumEncodedBytes(format, 512U), 0U);
    EXPECT_EQ(Encode(format, Level::kInfo, source, &BuildScalars), std::nullopt);
  }
}

}  // namespace
}  // namespace ulog::detail::encoding
