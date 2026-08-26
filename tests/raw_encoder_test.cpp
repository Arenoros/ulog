#include "encoding/raw_encoder.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>

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
  RawEncodeResult complete_result{};
  RawEncodeResult too_small_result{};
};

void EncodeWithExactBoundary(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& capture = *static_cast<EncodeCapture*>(context);
  capture.too_small.fill('#');
  capture.complete_result = EncodeRawRecord(record, capture.complete);
  capture.too_small_result = EncodeRawRecord(record, capture.too_small);
}

void EncodeComplete(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& capture = *static_cast<EncodeCapture*>(context);
  capture.complete_result = EncodeRawRecord(record, capture.complete);
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
  EXPECT_EQ(MaximumRawEncodedBytes(512U), 1'035U);
  EXPECT_EQ(MaximumRawEncodedBytes(std::numeric_limits<std::size_t>::max()), 0U);
}

}  // namespace
}  // namespace ulog::detail::encoding
