#include "encoding/text_encoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <ulog/fields.hpp>
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

constexpr std::array kAllFormats{Format::kTskv, Format::kLtsv, Format::kRaw, Format::kJson,
                                 Format::kJsonYaDeploy};

TEST(TextEncoder, AFrameIsWrittenWhollyOrNotAtAll) {
  const auto source = SourceLocation::Custom("a.cpp", "F", 7);
  for (const Format format : kAllFormats) {
    const std::string frame =
        Encode(format, Level::kInfo, source, &BuildUnicode).value_or(std::string{});
    ASSERT_FALSE(frame.empty()) << ToString(format);
    EXPECT_EQ(Encode(format, Level::kInfo, source, &BuildUnicode, frame.size()), frame);
    EXPECT_EQ(Encode(format, Level::kInfo, source, &BuildUnicode, frame.size() - 1U), std::nullopt);
  }
}

TEST(TextEncoder, WorstCaseRecordFitsTheDerivedBound) {
  for (const Format format : kAllFormats) {
    const bool json = format == Format::kJson || format == Format::kJsonYaDeploy;
    // Every source, key, and message byte takes its longest escape: two bytes in the text formats
    // and `\u00XX` in JSON. Numbers take their longest form.
    const char text_byte = json ? '\x01' : '\n';
    const std::string path(200, json ? '\x01' : '\t');
    const std::string function(200, json ? '\x01' : '\\');
    const auto source = SourceLocation::Custom(path, function, 4'294'967'295U);
    const auto worst = [text_byte](RecordAppender& writer) {
      for (char suffix = 'a'; suffix < 'i'; ++suffix) {
        if (!writer.AddField(std::string{text_byte, suffix}, -2.2250738585072014e-308)) {
          return false;
        }
      }
      static_cast<void>(writer.Append(std::string(kRecordBytes, text_byte)));
      return true;
    };
    const std::size_t bound = MaximumEncodedBytes(format, kRecordBytes);
    const std::string frame =
        Encode(format, Level::kCritical, source, worst, bound).value_or(std::string{});
    ASSERT_FALSE(frame.empty()) << ToString(format);
    EXPECT_LE(frame.size(), bound);
    // The truncated message filled the Record, so the bound exceeds the frame only by the
    // expanded serialized bytes the frame does not print: the 56-byte Record header, the 24-byte
    // metadata of the eight fields and the truncation marker, and for Raw the 400 source bytes.
    const std::size_t unprinted = 56U + 9U * 24U + (format == Format::kRaw ? 400U : 0U);
    EXPECT_GE(frame.size() + (json ? 6U : 2U) * unprinted, bound) << ToString(format);
  }
}

TEST(TextEncoder, BoundsAreDerivedPerFormat) {
  EXPECT_EQ(MaximumEncodedBytes(Format::kRaw, 512U), 11U + 1'024U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kTskv, 512U), 87U + 1'024U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kLtsv, 512U), 82U + 1'024U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kJson, 512U), 101U + 3'072U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kJsonYaDeploy, 512U), 108U + 3'072U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kTskv, std::numeric_limits<std::size_t>::max()), 0U);
  EXPECT_EQ(MaximumEncodedBytes(Format::kJson, std::numeric_limits<std::size_t>::max() / 5U), 0U);
}

TEST(TextEncoder, ValuesOutsideTheFormatEnumerationNeverWrite) {
  for (const Format format : kAllFormats) {
    EXPECT_TRUE(IsEncodable(format)) << ToString(format);
  }
  const auto source = SourceLocation::Custom("a.cpp", "F", 7);
  for (const Format format :
       {std::bit_cast<Format>(std::uint8_t{5}), std::bit_cast<Format>(std::uint8_t{9})}) {
    EXPECT_FALSE(IsEncodable(format));
    EXPECT_EQ(MaximumEncodedBytes(format, 512U), 0U);
    EXPECT_EQ(Encode(format, Level::kInfo, source, &BuildScalars), std::nullopt);
  }
}

// 2000-01-02T03:04:05.123456Z, the UTC form of kCorpusInstant.
constexpr std::string_view kJsonTimestamp{"2000-01-02T03:04:05.123456Z"};

[[nodiscard]] std::string JsonPreamble(Format format, std::string_view level,
                                       std::string_view module) {
  const bool yadeploy = format == Format::kJsonYaDeploy;
  return std::string{yadeploy ? "{\"@timestamp\":\"" : "{\"timestamp\":\""} +
         std::string{kJsonTimestamp} + (yadeploy ? "\",\"levelStr\":\"" : "\",\"level\":\"") +
         std::string{level} + "\",\"module\":\"" + std::string{module} + "\"";
}

[[nodiscard]] std::string TextKey(Format format) {
  return format == Format::kJsonYaDeploy ? ",\"message\":" : ",\"text\":";
}

TEST(JsonEncoder, WritesTechnicalMembersTypedFieldsAndTextAsOneCompactObject) {
  const auto source = SourceLocation::Custom("src/probe.cpp", "BaselineJsonProbe", 321);
  for (const Format format : {Format::kJson, Format::kJsonYaDeploy}) {
    EXPECT_EQ(Encode(format, Level::kInfo, source, &BuildScalars),
              JsonPreamble(format, "INFO", "BaselineJsonProbe ( src/probe.cpp:321 )") +
                  ",\"string\":\"alpha\",\"signed\":-42,\"unsigned\":18446744073709551615"
                  ",\"float\":1.5,\"double\":-2.25,\"bool_true\":true,\"bool_false\":false"
                  ",\"optional\":null" +
                  TextKey(format) + "\"scalars\"}\n");
  }
}

TEST(JsonEncoder, YaDeployNeverWritesTheOrdinaryTechnicalNames) {
  const auto source = SourceLocation::Custom("a.cpp", "F", 7);
  const std::string frame =
      Encode(Format::kJsonYaDeploy, Level::kInfo, source, &BuildEmpty).value_or(std::string{});
  EXPECT_EQ(frame,
            JsonPreamble(Format::kJsonYaDeploy, "INFO", "F ( a.cpp:7 )") + ",\"message\":\"\"}\n");
  for (const std::string_view name : {"\"timestamp\"", "\"level\"", "\"text\""}) {
    EXPECT_EQ(frame.find(name), std::string::npos) << name;
  }
}

TEST(JsonEncoder, EveryLevelHasItsUppercaseName) {
  const auto source = SourceLocation::Custom("a.cpp", "F", 7);
  constexpr std::array kLevels{Level::kTrace,   Level::kDebug, Level::kInfo,
                               Level::kWarning, Level::kError, Level::kCritical};
  constexpr std::array<std::string_view, 6> kNames{"TRACE",   "DEBUG", "INFO",
                                                   "WARNING", "ERROR", "CRITICAL"};
  for (std::size_t index = 0; index < kLevels.size(); ++index) {
    for (const Format format : {Format::kJson, Format::kJsonYaDeploy}) {
      EXPECT_EQ(Encode(format, kLevels[index], source, &BuildEmpty),
                JsonPreamble(format, kNames[index], "F ( a.cpp:7 )") + TextKey(format) + "\"\"}\n");
    }
  }
}

TEST(JsonEncoder, EscapesKeysValuesMessageAndModuleWithoutChangingKeyCase) {
  const auto source = SourceLocation::Custom("dir\\\"x\ty.cpp", "f(\n)", 4'294'967'295U);
  EXPECT_EQ(Encode(Format::kJson, Level::kError, source, &BuildUnicode),
            JsonPreamble(Format::kJson, "ERROR", "f(\\n) ( dir\\\\\\\"x\\ty.cpp:4294967295 )") +
                ",\"A=.\\t\\r\\n\\u0000\\\\\":\"\xC3\xA9=ok:still|\\t\\r\\n\\u0000\\\\\""
                ",\"text\":\"\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|"
                "\\r\\n\\u0000\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\"}\n");

  const auto controls = [](RecordAppender& writer) {
    static_cast<void>(writer.Append("\"\b\f\x01\x1f\x7f/"sv));
    return true;
  };
  EXPECT_EQ(Encode(Format::kJson, Level::kInfo, SourceLocation::Custom("a.cpp", "F", 7), controls),
            JsonPreamble(Format::kJson, "INFO", "F ( a.cpp:7 )") +
                ",\"text\":\"\\\"\\b\\f\\u0001\\u001f\x7f/\"}\n");
}

TEST(JsonEncoder, ReplacesEachByteThatDoesNotStartWellFormedUtf8) {
  // A truncated sequence, an overlong form, a surrogate, a value above U+10FFFF, a byte that never
  // starts a sequence, and a stray continuation byte; the four-byte emoji is well formed.
  constexpr std::string_view kInvalid{
      "a\xC3|\xC0\xAF|\xED\xA0\x80|\xF4\x90\x80\x80|\xFF|\x80|\xF0\x9F\x8C\x8D"};
  // Each ill-formed byte (`#` below) becomes U+FFFD.
  std::string replaced{"a#|##|###|####|#|#|\xF0\x9F\x8C\x8D"};
  for (std::size_t position = replaced.find('#'); position != std::string::npos;
       position = replaced.find('#', position)) {
    replaced.replace(position, 1U, "\xEF\xBF\xBD");
  }
  // The message is already cut at its first ill-formed byte when the Record is published, so
  // only keys, string values, and the source location reach the encoder with such bytes.
  const auto build = [kInvalid](RecordAppender& writer) {
    return writer.AddField(kInvalid, kInvalid);
  };
  const auto source = SourceLocation::Custom(kInvalid, kInvalid, 7);
  EXPECT_EQ(Encode(Format::kJson, Level::kInfo, source, build),
            JsonPreamble(Format::kJson, "INFO", replaced + " ( " + replaced + ":7 )") + ",\"" +
                replaced + "\":\"" + replaced + "\",\"text\":\"\"}\n");
}

/// Encodes one raw-JSON field `v` as JSON and returns its written value.
[[nodiscard]] std::string RawJsonMember(std::string_view json) {
  const auto build = [json](RecordAppender& writer) {
    return writer.PutField("v", FieldValue::FromParts(FieldKind::kJson, json, 0U),
                           FieldMode::kReplaceable) == FieldStatus::kAdded;
  };
  const std::string frame =
      Encode(Format::kJson, Level::kInfo, SourceLocation::Custom("a.cpp", "F", 7), build)
          .value_or(std::string{});
  constexpr std::string_view kKey{",\"v\":"};
  const std::size_t begin = frame.find(kKey);
  const std::size_t end = frame.rfind(",\"text\":\"\"}\n");
  if (begin == std::string::npos || end == std::string::npos || end < begin + kKey.size()) {
    ADD_FAILURE() << frame;
    return {};
  }
  return frame.substr(begin + kKey.size(), end - begin - kKey.size());
}

TEST(JsonEncoder, InsertsValidRawJsonAsACompactNestedValue) {
  EXPECT_EQ(RawJsonMember("{ \"z\" : 1 ,\t\"a\":[ \"x y\\\" \", false , null ] ,\r\n\"o\":{}}"),
            "{\"z\":1,\"a\":[\"x y\\\" \",false,null],\"o\":{}}");
  EXPECT_EQ(RawJsonMember("  -0.5e+10 "), "-0.5e+10");
  EXPECT_EQ(RawJsonMember("\"\\u00e9\\/\xC3\xA9\""), "\"\\u00e9\\/\xC3\xA9\"");
  EXPECT_EQ(RawJsonMember("true"), "true");
  EXPECT_EQ(RawJsonMember("null"), "null");
  EXPECT_EQ(RawJsonMember("[]"), "[]");
  EXPECT_EQ(RawJsonMember(std::string(256, '[') + std::string(256, ']')),
            std::string(256, '[') + std::string(256, ']'));
}

TEST(JsonEncoder, WritesRawTextThatIsNotOneJsonValueAsAString) {
  EXPECT_EQ(RawJsonMember(""), "\"\"");
  EXPECT_EQ(RawJsonMember("{\"a\":1"), "\"{\\\"a\\\":1\"");
  EXPECT_EQ(RawJsonMember("{\"a\":1} x"), "\"{\\\"a\\\":1} x\"");
  EXPECT_EQ(RawJsonMember("1 2"), "\"1 2\"");
  EXPECT_EQ(RawJsonMember("01"), "\"01\"");
  EXPECT_EQ(RawJsonMember("1."), "\"1.\"");
  EXPECT_EQ(RawJsonMember("nan"), "\"nan\"");
  EXPECT_EQ(RawJsonMember("[1,]"), "\"[1,]\"");
  EXPECT_EQ(RawJsonMember("{\"a\" 1}"), "\"{\\\"a\\\" 1}\"");
  EXPECT_EQ(RawJsonMember("{1:2}"), "\"{1:2}\"");
  EXPECT_EQ(RawJsonMember("[}"), "\"[}\"");
  EXPECT_EQ(RawJsonMember("\"\\x\""), "\"\\\"\\\\x\\\"\"");
  EXPECT_EQ(RawJsonMember("\"\\u12\""), "\"\\\"\\\\u12\\\"\"");
  EXPECT_EQ(RawJsonMember("\"a\tb\""), "\"\\\"a\\tb\\\"\"");
  EXPECT_EQ(RawJsonMember("\"\xFF\""), "\"\\\"\xEF\xBF\xBD\\\"\"");
  EXPECT_EQ(RawJsonMember("tru"), "\"tru\"");
  // Nesting deeper than 256 is not inserted raw.
  const std::string deep = std::string(257, '[') + std::string(257, ']');
  EXPECT_EQ(RawJsonMember(deep), "\"" + deep + "\"");
}

TEST(JsonEncoder, WritesNonFiniteNumbersAsStrings) {
  const auto build = [](RecordAppender& writer) {
    return writer.AddField("nan", std::numeric_limits<double>::quiet_NaN()) &&
           writer.AddField("inf", std::numeric_limits<double>::infinity()) &&
           writer.AddField("minus_inf", -std::numeric_limits<double>::infinity()) &&
           writer.AddField("minus_zero", -0.0) && writer.AddField("large", 1e300);
  };
  EXPECT_EQ(Encode(Format::kJson, Level::kInfo, SourceLocation::Custom("a.cpp", "F", 7), build),
            JsonPreamble(Format::kJson, "INFO", "F ( a.cpp:7 )") +
                ",\"nan\":\"nan\",\"inf\":\"inf\",\"minus_inf\":\"-inf\",\"minus_zero\":-0"
                ",\"large\":1e+300,\"text\":\"\"}\n");
}

}  // namespace
}  // namespace ulog::detail::encoding
