#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <locale>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <ulog/fields.hpp>
#include <ulog/log.hpp>
#include <ulog/log_writer.hpp>
#include <vector>

#include "producer/producer_kernel.hpp"

namespace ulog::test {
namespace {

using namespace std::string_view_literals;
using detail::producer::ConsumeStatus;
using detail::producer::FieldView;
using detail::producer::KernelConfig;
using detail::producer::ProducerKernel;
using detail::producer::RecordView;

constexpr SourceLocation kSource = SourceLocation::Custom("writer.cpp", "Write", 7);

static_assert(!std::is_copy_constructible_v<LogWriter>);
static_assert(!std::is_copy_assignable_v<LogWriter>);
static_assert(!std::is_move_constructible_v<LogWriter>);
static_assert(!std::is_move_assignable_v<LogWriter>);

[[nodiscard]] KernelConfig WriterConfig(Level threshold = Level::kTrace,
                                        std::size_t maximum_record_bytes = 4'096,
                                        std::size_t ingress_cells = 4) noexcept {
  return KernelConfig{
      .threshold = threshold,
      .payload_capacity_bytes = maximum_record_bytes * ingress_cells,
      .maximum_record_bytes = maximum_record_bytes,
      .producer_slots = 1,
      .ingress_cells = ingress_cells,
  };
}

struct ObservedField final {
  std::string key;
  std::string text;
};

struct Observed final {
  bool present{false};
  Level level{Level::kNone};
  std::string message;
  std::string source_path;
  std::string source_function;
  std::uint32_t source_line{0};
  bool truncated{false};
  std::size_t serialized_bytes{0};
  std::vector<ObservedField> fields;
};

[[nodiscard]] std::string FieldText(const FieldView& field) {
  if (const auto text = field.AsString()) return std::string{*text};
  if (const auto value = field.AsInt64()) return std::to_string(*value);
  if (const auto value = field.AsUInt64()) return std::to_string(*value);
  if (const auto value = field.AsBool()) return *value ? "true" : "false";
  return "?";
}

void Observe(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& observed = *static_cast<Observed*>(context);
  try {
    observed.present = true;
    observed.level = record.level();
    observed.message = record.message();
    observed.source_path = record.source_path();
    observed.source_function = record.source_function();
    observed.source_line = record.source_line();
    observed.truncated = record.truncated();
    observed.serialized_bytes = record.serialized_bytes();
    auto cursor = record.Fields();
    for (std::size_t index = 0; index < record.field_count(); ++index) {
      const auto field = cursor.Next();
      if (field) {
        observed.fields.push_back({std::string{field->key()}, FieldText(*field)});
      }
    }
  } catch (...) {
    observed.present = false;
  }
}

[[nodiscard]] Observed Take(ProducerKernel& kernel) {
  Observed observed;
  if (kernel.TryConsume(&observed, &Observe) != ConsumeStatus::kRecord) {
    observed.present = false;
  }
  return observed;
}

[[nodiscard]] std::string TakeMessage(ProducerKernel& kernel) {
  const Observed observed = Take(kernel);
  EXPECT_TRUE(observed.present);
  return observed.message;
}

class ScopedDefaultLogger final {
 public:
  explicit ScopedDefaultLogger(Logger logger) noexcept : previous_(ExchangeDefaultLogger(logger)) {}
  ScopedDefaultLogger(const ScopedDefaultLogger&) = delete;
  ScopedDefaultLogger& operator=(const ScopedDefaultLogger&) = delete;
  ~ScopedDefaultLogger() { static_cast<void>(ExchangeDefaultLogger(previous_)); }

 private:
  Logger previous_;
};

/// The Default Logger targets must stay alive until the process ends.
ProducerKernel& DefaultWriterKernel() {
  static ProducerKernel kernel{WriterConfig(Level::kTrace, 4'096, 16)};
  return kernel;
}

TEST(LogWriter, SubmitsOneRecordWithTextFieldsAndExplicitSourceWhenTheScopeEnds) {
  ProducerKernel kernel{WriterConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  {
    LogWriter writer{kernel.GetLogger(), Level::kWarning, kSource};
    ASSERT_TRUE(writer.IsActive());
    ASSERT_TRUE(static_cast<bool>(writer));
    writer("count={}", 3) << ' ' << "items";
    EXPECT_EQ(writer.AddField("id", 7), FieldStatus::kAdded);
    EXPECT_EQ(writer.AddField("text", "reserved"), FieldStatus::kReserved);
    writer << Fields{{"ok", true}};
    EXPECT_EQ(kernel.TryConsume(nullptr, nullptr), ConsumeStatus::kEmpty);
  }
  const Observed observed = Take(kernel);
  ASSERT_TRUE(observed.present);
  EXPECT_EQ(observed.level, Level::kWarning);
  EXPECT_EQ(observed.message, "count=3 items");
  EXPECT_EQ(observed.source_path, "writer.cpp");
  EXPECT_EQ(observed.source_function, "Write");
  EXPECT_EQ(observed.source_line, 7U);
  ASSERT_EQ(observed.fields.size(), 2U);
  EXPECT_EQ(observed.fields[0].key, "id");
  EXPECT_EQ(observed.fields[0].text, "7");
  EXPECT_EQ(observed.fields[1].key, "ok");
  EXPECT_EQ(observed.fields[1].text, "true");
  EXPECT_FALSE(Take(kernel).present);
  EXPECT_EQ(kernel.GetSnapshot().accepted_records, 1U);
}

TEST(LogWriter, SubmitPublishesOnceAndLeavesTheWriterInactive) {
  ProducerKernel kernel{WriterConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  {
    LogWriter writer{kernel.GetLogger(), Level::kInfo, kSource};
    writer << "first";
    writer.Submit();
    EXPECT_FALSE(writer.IsActive());
    EXPECT_EQ(TakeMessage(kernel), "first");
    writer << "ignored";
    EXPECT_EQ(writer.AddField("late", 1), FieldStatus::kTruncated);
    writer.Submit();
  }
  EXPECT_FALSE(Take(kernel).present);
  EXPECT_EQ(kernel.GetSnapshot().accepted_records, 1U);
}

TEST(LogWriter, DisabledWritersSubmitNothing) {
  ProducerKernel kernel{WriterConfig(Level::kInfo)};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  {
    LogWriter filtered{kernel.GetLogger(), Level::kDebug, kSource};
    LogWriter null_logger{GetNullLogger(), Level::kCritical, kSource};
    EXPECT_FALSE(filtered.IsActive());
    EXPECT_FALSE(null_logger.IsActive());
    filtered << "filtered";
    null_logger("{}", "null");
  }
  ProducerKernel unregistered{WriterConfig()};
  {
    LogWriter writer{unregistered.GetLogger(), Level::kInfo, kSource};
    EXPECT_FALSE(writer.IsActive());
  }
  EXPECT_FALSE(Take(kernel).present);
  EXPECT_FALSE(Take(unregistered).present);
  EXPECT_EQ(unregistered.GetSnapshot().rejected_no_producer, 1U);
}

TEST(LogWriter, StreamedOperandsAreEvaluatedOnlyAfterAdmission) {
  ProducerKernel kernel{WriterConfig(Level::kInfo, 4'096, 1)};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  ProducerKernel unregistered{WriterConfig()};
  int evaluations = 0;
  const auto operand = [&evaluations] {
    ++evaluations;
    return "evaluated";
  };

  LOG_DEBUG_TO(kernel.GetLogger()) << operand();
  LOG_INFO_TO(GetNullLogger()) << operand();
  LOG_INFO_TO(unregistered.GetLogger()) << operand();
  LOG_TO(kernel.GetLogger(), Level::kTrace, "{}", operand()) << operand();
  EXPECT_EQ(evaluations, 0);

  LOG_INFO_TO(kernel.GetLogger()) << operand();
  EXPECT_EQ(evaluations, 1);
  // The only ingress cell is occupied, so the next call is rejected before its operands run.
  LOG_ERROR_TO(kernel.GetLogger(), "{}", operand()) << operand();
  EXPECT_EQ(evaluations, 1);
  EXPECT_EQ(kernel.GetSnapshot().rejected_lane_full, 1U);
  EXPECT_EQ(TakeMessage(kernel), "evaluated");
}

TEST(LogWriter, EveryMacroFamilySupportsTheStreamForm) {
  ProducerKernel kernel{WriterConfig(Level::kTrace, 4'096, 16)};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const Logger logger = kernel.GetLogger();

  LOG_TRACE_TO(logger) << "trace";
  LOG_DEBUG_TO(logger) << "debug";
  LOG_INFO_TO(logger) << "info";
  LOG_WARNING_TO(logger) << "warning";
  LOG_ERROR_TO(logger) << "error";
  LOG_CRITICAL_TO(logger) << "critical";
  LOG_TO(logger, Level::kWarning) << "generic";
  const struct {
    Level level;
    std::string_view message;
  } expected[] = {
      {Level::kTrace, "trace"},     {Level::kDebug, "debug"}, {Level::kInfo, "info"},
      {Level::kWarning, "warning"}, {Level::kError, "error"}, {Level::kCritical, "critical"},
      {Level::kWarning, "generic"},
  };
  for (const auto& record : expected) {
    const Observed observed = Take(kernel);
    ASSERT_TRUE(observed.present) << record.message;
    EXPECT_EQ(observed.level, record.level);
    EXPECT_EQ(observed.message, record.message);
    EXPECT_NE(observed.source_path.find("log_writer_test.cpp"), std::string::npos);
  }

  ProducerKernel& default_kernel = DefaultWriterKernel();
  auto default_producer = default_kernel.TryRegisterProducer();
  ASSERT_TRUE(default_producer);
  const ScopedDefaultLogger scoped{default_kernel.GetLogger()};
  LOG_TRACE() << "trace";
  LOG_DEBUG() << "debug";
  LOG_INFO() << "info";
  LOG_WARNING() << "warning";
  LOG_ERROR() << "error";
  LOG_CRITICAL() << "critical";
  LOG(Level::kWarning) << "generic";
  for (const auto& record : expected) {
    const Observed observed = Take(default_kernel);
    ASSERT_TRUE(observed.present) << record.message;
    EXPECT_EQ(observed.level, record.level);
    EXPECT_EQ(observed.message, record.message);
  }
}

TEST(LogWriter, StreamFormsAreSafeInUnbracedControlFlow) {
  ProducerKernel kernel{WriterConfig(Level::kTrace, 4'096, 8)};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  int else_branches = 0;

  if (true)
    LOG_INFO_TO(kernel.GetLogger()) << "taken";
  else
    ++else_branches;
  if (false)
    LOG_INFO_TO(kernel.GetLogger()) << "skipped";
  else
    ++else_branches;
  for (int iteration = 0; iteration < 2; ++iteration)
    LOG_INFO_TO(kernel.GetLogger(), "loop {}", iteration) << '!';

  EXPECT_EQ(else_branches, 1);
  EXPECT_EQ(TakeMessage(kernel), "taken");
  EXPECT_EQ(TakeMessage(kernel), "loop 0!");
  EXPECT_EQ(TakeMessage(kernel), "loop 1!");
  EXPECT_FALSE(Take(kernel).present);
}

TEST(LogWriter, PrimitiveValuesUseBaselineTextForms) {
  ProducerKernel kernel{WriterConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const auto log_all = [&kernel] {
    const char* const null_text = nullptr;
    char array[] = "array";
    LOG_INFO_TO(kernel.GetLogger())
        << 'c' << '|' << "literal" << '|' << std::string{"string"} << '|' << "view"sv << '|'
        << array << '|' << null_text << '|' << std::numeric_limits<std::int64_t>::min() << '|'
        << std::numeric_limits<std::uint64_t>::max() << '|' << std::int8_t{-5} << '|'
        << std::uint8_t{200} << '|' << short{-7} << '|' << 0U << '|' << 1.5F << '|' << 0.1F << '|'
        << -2.25 << '|' << 1e300 << '|' << 1e-7 << '|' << 100.0 << '|'
        << std::numeric_limits<double>::quiet_NaN() << '|'
        << std::numeric_limits<double>::infinity() << '|' << -std::numeric_limits<float>::infinity()
        << '|' << 0.5L << '|' << true << '|' << false << '|' << std::runtime_error{"boom"} << '|'
        << std::string_view{"a\0b\n\t", 5};
  };
  constexpr std::string_view kExpected =
      "c|literal|string|view|array|(null)|-9223372036854775808|18446744073709551615|-5|200|-7|0|"
      "1.5|0.1|-2.25|1e+300|1e-07|100|nan|inf|-inf|0.5|true|false|boom (std::runtime_error)|a\0b\n\t"sv;

  log_all();
  EXPECT_EQ(TakeMessage(kernel), kExpected);

  // The text forms do not depend on the global locale. A host without a comma-decimal locale
  // repeats the check under the classic one.
  const std::locale previous = std::locale::global(std::locale::classic());
  std::string selected_locale = "C";
  for (const char* const name : {"de_DE.UTF-8", "de_DE", "German_Germany.1252", "ru_RU.UTF-8"}) {
    try {
      std::locale::global(std::locale{name});
      selected_locale = name;
      break;
    } catch (const std::runtime_error&) {
      selected_locale = "C";
    }
  }
  RecordProperty("global_locale", selected_locale);
  log_all();
  std::locale::global(previous);
  EXPECT_EQ(TakeMessage(kernel), kExpected);
}

TEST(LogWriter, FmtTextStartsAndContinuesWithOwnedCopies) {
  ProducerKernel kernel{WriterConfig(Level::kTrace, 4'096, 8)};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  std::string owned = "first";

  LOG_INFO_TO(kernel.GetLogger(), "{}-{}", owned, 2) << '+' << owned;
  LOG_INFO_TO(kernel.GetLogger(), "{braces}") << " stay literal";
  LOG_INFO_TO(kernel.GetLogger(), Fields{{"k", 1}}, "fields {}", 1) << " tail";
  LOG_INFO_TO(kernel.GetLogger(), Fields{{"k", 2}}) << "stream only";
  {
    LogWriter writer{kernel.GetLogger(), Level::kInfo, kSource};
    writer << "a=" << 1;
    writer.Format(", b={}", 2) << ", c=" << 3;
  }
  owned.assign("changed");

  EXPECT_EQ(TakeMessage(kernel), "first-2+first");
  EXPECT_EQ(TakeMessage(kernel), "{braces} stay literal");
  Observed with_fields = Take(kernel);
  EXPECT_EQ(with_fields.message, "fields 1 tail");
  ASSERT_EQ(with_fields.fields.size(), 1U);
  EXPECT_EQ(with_fields.fields[0].text, "1");
  with_fields = Take(kernel);
  EXPECT_EQ(with_fields.message, "stream only");
  ASSERT_EQ(with_fields.fields.size(), 1U);
  EXPECT_EQ(with_fields.fields[0].text, "2");
  EXPECT_EQ(TakeMessage(kernel), "a=1, b=2, c=3");

  // Formatting a long argument fills the chunk buffer more than once.
  const std::string long_text(1'000, 'x');
  LOG_INFO_TO(kernel.GetLogger(), "<{}>", long_text) << '!';
  EXPECT_EQ(TakeMessage(kernel), "<" + long_text + ">!");
}

TEST(LogWriter, FieldsAreInsertedInOrderUnderTheRecordPolicy) {
  ProducerKernel kernel{WriterConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  FieldSet extra;
  extra.Add("region", "eu", FieldMode::kFrozen);
  LOG_INFO_TO(kernel.GetLogger()) << "text" << extra << Fields{{"region", "us"}, {"n", 1}}
                                  << " more";
  const Observed observed = Take(kernel);
  EXPECT_EQ(observed.message, "text more");
  ASSERT_EQ(observed.fields.size(), 2U);
  EXPECT_EQ(observed.fields[0].key, "region");
  EXPECT_EQ(observed.fields[0].text, "eu");
  EXPECT_EQ(observed.fields[1].key, "n");
}

TEST(LogWriter, FormattingFailuresAbandonTheRecordWithoutThrowing) {
  ProducerKernel kernel{WriterConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  EXPECT_NO_THROW({ LOG_INFO_TO(kernel.GetLogger(), "value={:{}}", 42, -1) << "more"; });
  {
    LogWriter writer{kernel.GetLogger(), Level::kInfo, kSource};
    writer << "before";
    writer.Format("{:{}}", 1, -1);
    writer << "after";
    EXPECT_TRUE(writer.IsActive());
  }
  EXPECT_FALSE(Take(kernel).present);
  EXPECT_EQ(kernel.GetSnapshot().abandoned_builds, 2U);
  LOG_INFO_TO(kernel.GetLogger()) << "slot is free";
  EXPECT_EQ(TakeMessage(kernel), "slot is free");
}

TEST(LogWriter, AnOperandExceptionAbandonsTheRecordAndPropagates) {
  ProducerKernel kernel{WriterConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  volatile bool fail = true;
  const auto throwing = [&fail]() -> int {
    if (fail) {
      throw std::runtime_error{"operand"};
    }
    return 0;
  };
  EXPECT_THROW({ LOG_INFO_TO(kernel.GetLogger()) << "partial" << throwing(); }, std::runtime_error);
  EXPECT_FALSE(Take(kernel).present);
  EXPECT_EQ(kernel.GetSnapshot().abandoned_builds, 1U);
  LOG_INFO_TO(kernel.GetLogger()) << "after";
  EXPECT_EQ(TakeMessage(kernel), "after");
}

TEST(LogWriter, ANestedStatementInAnOperandIsRejectedWhileTheOuterRecordIsOpen) {
  ProducerKernel kernel{WriterConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const auto nested = [&kernel] {
    LOG_INFO_TO(kernel.GetLogger()) << "inner";
    return "outer";
  };
  LOG_INFO_TO(kernel.GetLogger()) << nested();
  EXPECT_EQ(TakeMessage(kernel), "outer");
  EXPECT_FALSE(Take(kernel).present);
  EXPECT_EQ(kernel.GetSnapshot().rejected_lane_full, 1U);
}

// --- DEF-015: truncation at valid UTF-8 boundaries -------------------------------------------

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

[[nodiscard]] bool IsCodePointBoundary(std::string_view text, std::size_t position) {
  return position == text.size() || (static_cast<unsigned char>(text[position]) & 0xC0U) != 0x80U;
}

enum class WritingForm : std::uint8_t { kString, kCharacters, kFormat, kStartText, kThenField };

TEST(LogWriter, EveryWritingFormTruncatesAtAValidUtf8BoundaryWithOneMarker) {
  constexpr std::size_t kRecordBytes = 256;
  // Two-, three-, and four-byte code points, repeated past any Record bound.
  std::string payload;
  for (int repeat = 0; repeat < 40; ++repeat) {
    payload += "\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80";
  }
  for (const WritingForm form :
       {WritingForm::kString, WritingForm::kCharacters, WritingForm::kFormat,
        WritingForm::kStartText, WritingForm::kThenField}) {
    for (std::size_t prefix_bytes = 0; prefix_bytes < 200; ++prefix_bytes) {
      SCOPED_TRACE(static_cast<int>(form));
      SCOPED_TRACE(prefix_bytes);
      ProducerKernel kernel{WriterConfig(Level::kTrace, kRecordBytes, 1)};
      auto producer = kernel.TryRegisterProducer();
      ASSERT_TRUE(producer);
      const std::string prefix(prefix_bytes, 'a');
      const std::string full = prefix + payload;
      {
        LogWriter writer{kernel.GetLogger(), Level::kInfo, kSource};
        switch (form) {
          case WritingForm::kString:
            writer << prefix << payload;
            break;
          case WritingForm::kCharacters:
            for (const char byte : full) {
              writer << byte;
            }
            break;
          case WritingForm::kFormat:
            writer.Format("{}{}", prefix, payload);
            break;
          case WritingForm::kStartText:
            writer(full);
            break;
          case WritingForm::kThenField:
            writer << full << Fields{{"after", 1}};
            break;
        }
      }
      const Observed observed = Take(kernel);
      ASSERT_TRUE(observed.present);
      EXPECT_LE(observed.serialized_bytes, kRecordBytes);
      EXPECT_TRUE(IsValidUtf8(observed.message));
      ASSERT_LT(observed.message.size(), full.size());
      EXPECT_EQ(full.substr(0, observed.message.size()), observed.message);
      EXPECT_TRUE(IsCodePointBoundary(full, observed.message.size()));
      EXPECT_TRUE(observed.truncated);
      ASSERT_FALSE(observed.fields.empty());
      std::size_t markers = 0;
      for (const ObservedField& field : observed.fields) {
        markers += field.key == "ulog.truncated" ? 1U : 0U;
      }
      EXPECT_EQ(markers, 1U);
      EXPECT_EQ(observed.fields.back().key, "ulog.truncated");
      EXPECT_EQ(observed.fields.back().text, "true");
    }
  }
}

TEST(LogWriter, TextThatFitsIsNeverMarkedTruncated) {
  ProducerKernel kernel{WriterConfig(Level::kTrace, 256, 1)};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  { LogWriter{kernel.GetLogger(), Level::kInfo, kSource} << "\xC3\xA9\xF0\x9F\x98\x80" << 'x'; }
  const Observed observed = Take(kernel);
  EXPECT_EQ(observed.message, "\xC3\xA9\xF0\x9F\x98\x80x");
  EXPECT_FALSE(observed.truncated);
  EXPECT_TRUE(observed.fields.empty());
}

}  // namespace
}  // namespace ulog::test
