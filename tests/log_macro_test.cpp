#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ulog/log.hpp>

#include "producer/producer_kernel.hpp"

namespace ulog::test {

struct ThrowingFormatValue final {};

}  // namespace ulog::test

template <>
struct fmt::formatter<ulog::test::ThrowingFormatValue> final {
  constexpr auto parse(fmt::format_parse_context& context) { return context.begin(); }

  template <typename FormatContext>
  auto format(const ulog::test::ThrowingFormatValue&, FormatContext&) const ->
      typename FormatContext::iterator {
    throw std::runtime_error{"formatter failure"};
  }
};

namespace ulog::test {
namespace {

using detail::producer::ConsumeStatus;
using detail::producer::KernelConfig;
using detail::producer::ProducerKernel;
using detail::producer::RecordView;

class PoisonAfterConversion final {
 public:
  explicit PoisonAfterConversion(std::string& storage) noexcept : storage_(&storage) {}

  PoisonAfterConversion(const PoisonAfterConversion&) = delete;
  PoisonAfterConversion& operator=(const PoisonAfterConversion&) = delete;

  ~PoisonAfterConversion() {
    for (char& character : *storage_) {
      character = '#';
    }
  }

  operator std::string_view() const noexcept { return *storage_; }

 private:
  std::string* storage_;
};

[[nodiscard]] constexpr KernelConfig MacroTestConfig() noexcept {
  return KernelConfig{
      .threshold = Level::kTrace,
      .payload_capacity_bytes = 512,
      .maximum_record_bytes = 256,
      .producer_slots = 1,
      .ingress_cells = 2,
  };
}

[[nodiscard]] constexpr KernelConfig MacroTestConfig(Level threshold,
                                                     std::size_t payload_capacity_bytes,
                                                     std::size_t ingress_cells) noexcept {
  return KernelConfig{
      .threshold = threshold,
      .payload_capacity_bytes = payload_capacity_bytes,
      .maximum_record_bytes = 256,
      .producer_slots = 1,
      .ingress_cells = ingress_cells,
  };
}

ProducerKernel& DefaultMacroKernel() {
  static ProducerKernel kernel{MacroTestConfig()};
  return kernel;
}

ProducerKernel& ReplacementMacroKernel() {
  static ProducerKernel kernel{MacroTestConfig()};
  return kernel;
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

struct ObservedRecord final {
  bool failed{false};
  Level level{Level::kNone};
  std::string message;
  std::string source_path;
  std::string source_function;
  std::uint32_t source_line{0};
};

void ObserveRecord(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& observed = *static_cast<ObservedRecord*>(context);
  try {
    observed.level = record.level();
    observed.message = record.message();
    observed.source_path = record.source_path();
    observed.source_function = record.source_function();
    observed.source_line = record.source_line();
  } catch (...) {
    observed.failed = true;
  }
}

void ExpectNextRecord(ProducerKernel& kernel, Level level, std::string_view message) {
  ObservedRecord observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &ObserveRecord), ConsumeStatus::kRecord);
  EXPECT_FALSE(observed.failed);
  EXPECT_EQ(observed.level, level);
  EXPECT_EQ(observed.message, message);
  EXPECT_NE(observed.source_path.find("log_macro_test.cpp"), std::string::npos);
  EXPECT_FALSE(observed.source_function.empty());
  EXPECT_NE(observed.source_line, 0U);
}

TEST(LogMacro, UnnamedInfoPublishesTextAtTheCallSite) {
  ProducerKernel& kernel = DefaultMacroKernel();
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const ScopedDefaultLogger default_logger{kernel.GetLogger()};

  const std::uint32_t expected_line = __LINE__ + 1;
  LOG_INFO("macro message");

  ObservedRecord observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &ObserveRecord), ConsumeStatus::kRecord);
  EXPECT_FALSE(observed.failed);
  EXPECT_EQ(observed.level, Level::kInfo);
  EXPECT_EQ(observed.message, "macro message");
  EXPECT_NE(observed.source_path.find("log_macro_test.cpp"), std::string::npos);
  EXPECT_EQ(observed.source_line, expected_line);
}

TEST(LogMacro, ExplicitNullTargetIsEvaluatedOnceAndDoesNotEvaluateText) {
  std::size_t target_evaluations = 0;
  std::size_t message_evaluations = 0;

  const auto select_target = [&] {
    ++target_evaluations;
    return GetNullLogger();
  };

  LOG_INFO_TO(select_target(), ([&]() -> std::string_view {
                ++message_evaluations;
                return "must stay lazy";
              })());

  EXPECT_EQ(target_evaluations, 1U);
  EXPECT_EQ(message_evaluations, 0U);
}

TEST(LogMacro, ExplicitInfoFormatsOnlyAfterAdmission) {
  ProducerKernel kernel{MacroTestConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  std::size_t integer_evaluations = 0;
  std::size_t bool_evaluations = 0;

  LOG_INFO_TO(kernel.GetLogger(), "answer={}, ok={}", ([&] {
                ++integer_evaluations;
                return 42;
              })(),
              ([&] {
                ++bool_evaluations;
                return true;
              })());

  ObservedRecord observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &ObserveRecord), ConsumeStatus::kRecord);
  EXPECT_FALSE(observed.failed);
  EXPECT_EQ(observed.message, "answer=42, ok=true");
  EXPECT_EQ(integer_evaluations, 1U);
  EXPECT_EQ(bool_evaluations, 1U);
}

TEST(LogMacro, InvalidBuiltinFormatDoesNotEscapeOrPublish) {
  ProducerKernel kernel{MacroTestConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);

  EXPECT_NO_THROW({ LOG_INFO_TO(kernel.GetLogger(), "value={:{}}", 42, -1); });

  EXPECT_EQ(kernel.TryConsume(nullptr, nullptr), ConsumeStatus::kEmpty);
  EXPECT_EQ(kernel.GetSnapshot().abandoned_builds, 1U);
}

TEST(LogMacro, FormatterFailuresDoNotEscapeButOperandFailuresDo) {
  ProducerKernel kernel{MacroTestConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  volatile bool throw_operand = true;

  EXPECT_NO_THROW({ LOG_INFO_TO(kernel.GetLogger(), "value={}", ThrowingFormatValue{}); });
  EXPECT_THROW(
      {
        LOG_INFO_TO(kernel.GetLogger(), "value={}", ([&]() -> int {
                      if (throw_operand) {
                        throw std::runtime_error{"operand failure"};
                      }
                      return 0;
                    })());
      },
      std::runtime_error);

  EXPECT_EQ(kernel.TryConsume(nullptr, nullptr), ConsumeStatus::kEmpty);
  EXPECT_EQ(kernel.GetSnapshot().abandoned_builds, 2U);
}

TEST(LogMacro, CallerIdentifiersDoNotBindToMacroInternals) {
  ProducerKernel kernel{MacroTestConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const Logger ulog_macro_logger = kernel.GetLogger();
  const Level ulog_macro_level = Level::kWarning;
  const std::string_view ulog_macro_sink = "caller text";

  LOG_INFO_TO(ulog_macro_logger, ulog_macro_sink);
  ExpectNextRecord(kernel, Level::kInfo, "caller text");
  LOG_TO(ulog_macro_logger, ulog_macro_level, "generic text");
  ExpectNextRecord(kernel, Level::kWarning, "generic text");
}

TEST(LogMacro, MessageOperandsAreConsumedBeforeNestedTemporariesExpire) {
  auto config = MacroTestConfig(Level::kTrace, 1024, 2);
  config.maximum_record_bytes = 512;
  ProducerKernel kernel{config};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  std::string native_storage = "native temporary";
  std::string formatted_storage = "formatted temporary";

  LOG_INFO_TO(kernel.GetLogger(), std::string_view{PoisonAfterConversion{native_storage}});
  LOG_INFO_TO(kernel.GetLogger(), "value={}",
              std::string_view{PoisonAfterConversion{formatted_storage}});

  const auto snapshot = kernel.GetSnapshot();
  EXPECT_EQ(snapshot.accepted_records, 2U);
  EXPECT_EQ(snapshot.abandoned_builds, 0U);
  EXPECT_EQ(snapshot.rejected_lane_full, 0U);
  EXPECT_EQ(snapshot.rejected_budget, 0U);
  ExpectNextRecord(kernel, Level::kInfo, "native temporary");
  ExpectNextRecord(kernel, Level::kInfo, "value=formatted temporary");
}

TEST(LogMacro, CompleteExplicitNamedFamilyUsesItsDeclaredLevels) {
  ProducerKernel kernel{MacroTestConfig()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const Logger logger = kernel.GetLogger();

  LOG_TRACE_TO(logger, "trace");
  ExpectNextRecord(kernel, Level::kTrace, "trace");
  LOG_DEBUG_TO(logger, "debug={}", 1);
  ExpectNextRecord(kernel, Level::kDebug, "debug=1");
  LOG_INFO_TO(logger, "info");
  ExpectNextRecord(kernel, Level::kInfo, "info");
  LOG_WARNING_TO(logger, "warning={}", 2);
  ExpectNextRecord(kernel, Level::kWarning, "warning=2");
  LOG_ERROR_TO(logger, "error");
  ExpectNextRecord(kernel, Level::kError, "error");
  LOG_CRITICAL_TO(logger, "critical={}", 3);
  ExpectNextRecord(kernel, Level::kCritical, "critical=3");
}

TEST(LogMacro, CompleteUnnamedNamedFamilyUsesItsDeclaredLevels) {
  ProducerKernel& kernel = DefaultMacroKernel();
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const ScopedDefaultLogger default_logger{kernel.GetLogger()};

  LOG_TRACE("trace");
  ExpectNextRecord(kernel, Level::kTrace, "trace");
  LOG_DEBUG("debug={}", 1);
  ExpectNextRecord(kernel, Level::kDebug, "debug=1");
  LOG_INFO("info");
  ExpectNextRecord(kernel, Level::kInfo, "info");
  LOG_WARNING("warning={}", 2);
  ExpectNextRecord(kernel, Level::kWarning, "warning=2");
  LOG_ERROR("error");
  ExpectNextRecord(kernel, Level::kError, "error");
  LOG_CRITICAL("critical={}", 3);
  ExpectNextRecord(kernel, Level::kCritical, "critical=3");
}

TEST(LogMacro, GenericFormsEvaluateDynamicSelectorsOnce) {
  ProducerKernel explicit_kernel{MacroTestConfig()};
  auto explicit_producer = explicit_kernel.TryRegisterProducer();
  ASSERT_TRUE(explicit_producer);
  std::size_t target_evaluations = 0;
  std::size_t level_evaluations = 0;
  std::size_t message_evaluations = 0;

  LOG_TO(([&] {
           ++target_evaluations;
           return explicit_kernel.GetLogger();
         })(),
         ([&] {
           ++level_evaluations;
           return Level::kError;
         })(),
         "generic={}", ([&] {
           ++message_evaluations;
           return 7;
         })());

  ExpectNextRecord(explicit_kernel, Level::kError, "generic=7");
  EXPECT_EQ(target_evaluations, 1U);
  EXPECT_EQ(level_evaluations, 1U);
  EXPECT_EQ(message_evaluations, 1U);

  ProducerKernel& default_kernel = DefaultMacroKernel();
  auto default_producer = default_kernel.TryRegisterProducer();
  ASSERT_TRUE(default_producer);
  const ScopedDefaultLogger default_logger{default_kernel.GetLogger()};
  LOG(Level::kDebug, "generic default");
  ExpectNextRecord(default_kernel, Level::kDebug, "generic default");
}

TEST(LogMacro, FilteredAndUnregisteredCallsDoNotEvaluateFormatOperands) {
  ProducerKernel filtered_kernel{MacroTestConfig(Level::kWarning, 512, 2)};
  auto filtered_producer = filtered_kernel.TryRegisterProducer();
  ASSERT_TRUE(filtered_producer);
  std::size_t filtered_evaluations = 0;
  LOG_INFO_TO(filtered_kernel.GetLogger(), "filtered={}", ([&] {
                ++filtered_evaluations;
                return 1;
              })());
  EXPECT_EQ(filtered_evaluations, 0U);
  EXPECT_EQ(filtered_kernel.TryConsume(nullptr, nullptr), ConsumeStatus::kEmpty);

  ProducerKernel unregistered_kernel{MacroTestConfig()};
  std::size_t unregistered_evaluations = 0;
  LOG_INFO_TO(unregistered_kernel.GetLogger(), "unregistered={}", ([&] {
                ++unregistered_evaluations;
                return 2;
              })());
  EXPECT_EQ(unregistered_evaluations, 0U);
  EXPECT_EQ(unregistered_kernel.GetSnapshot().rejected_no_producer, 1U);
}

TEST(LogMacro, AdmissionRejectionDoesNotEvaluateFormatOperands) {
  ProducerKernel budget_kernel{MacroTestConfig(Level::kTrace, 256, 2)};
  auto budget_producer = budget_kernel.TryRegisterProducer();
  ASSERT_TRUE(budget_producer);
  LOG_INFO_TO(budget_kernel.GetLogger(), "retained");
  std::size_t budget_evaluations = 0;
  LOG_INFO_TO(budget_kernel.GetLogger(), "budget={}", ([&] {
                ++budget_evaluations;
                return 1;
              })());
  EXPECT_EQ(budget_evaluations, 0U);
  EXPECT_EQ(budget_kernel.GetSnapshot().rejected_budget, 1U);
  ExpectNextRecord(budget_kernel, Level::kInfo, "retained");

  ProducerKernel lane_kernel{MacroTestConfig(Level::kTrace, 512, 1)};
  auto lane_producer = lane_kernel.TryRegisterProducer();
  ASSERT_TRUE(lane_producer);
  LOG_INFO_TO(lane_kernel.GetLogger(), "retained");
  std::size_t lane_evaluations = 0;
  LOG_INFO_TO(lane_kernel.GetLogger(), "lane={}", ([&] {
                ++lane_evaluations;
                return 2;
              })());
  EXPECT_EQ(lane_evaluations, 0U);
  EXPECT_EQ(lane_kernel.GetSnapshot().rejected_lane_full, 1U);
  ExpectNextRecord(lane_kernel, Level::kInfo, "retained");
}

TEST(LogMacro, ExpansionIsSafeInNestedControlFlow) {
  std::size_t target_evaluations = 0;
  std::size_t message_evaluations = 0;
  std::size_t selected_branches = 0;
  const auto select_target = [&] {
    ++target_evaluations;
    return GetNullLogger();
  };
  const auto message = [&]() -> std::string_view {
    ++message_evaluations;
    return "suppressed";
  };

  if (true)
    LOG_INFO_TO(select_target(), message());
  else
    selected_branches += 100;

  if (false)
    LOG_INFO_TO(select_target(), message());
  else
    ++selected_branches;

  for (std::size_t iteration = 0; iteration < 2; ++iteration)
    LOG_INFO_TO(select_target(), message());

  EXPECT_EQ(selected_branches, 1U);
  EXPECT_EQ(target_evaluations, 3U);
  EXPECT_EQ(message_evaluations, 0U);
}

TEST(LogMacro, UnnamedCallFinishesAgainstTheTargetLoadedBeforeMessageEvaluation) {
  ProducerKernel& first = DefaultMacroKernel();
  ProducerKernel& replacement = ReplacementMacroKernel();
  auto first_producer = first.TryRegisterProducer();
  auto replacement_producer = replacement.TryRegisterProducer();
  ASSERT_TRUE(first_producer);
  ASSERT_TRUE(replacement_producer);
  const ScopedDefaultLogger default_logger{first.GetLogger()};

  LOG_INFO(([&]() -> std::string_view {
    static_cast<void>(ExchangeDefaultLogger(replacement.GetLogger()));
    return "stable target";
  })());

  ExpectNextRecord(first, Level::kInfo, "stable target");
  EXPECT_EQ(replacement.TryConsume(nullptr, nullptr), ConsumeStatus::kEmpty);
}

}  // namespace
}  // namespace ulog::test
