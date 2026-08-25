#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

namespace ulog::test {

Logger LoadDefaultLoggerFromAnotherTranslationUnit() noexcept;

namespace {

constexpr std::array kLevels{
    Level::kTrace, Level::kDebug,    Level::kInfo, Level::kWarning,
    Level::kError, Level::kCritical, Level::kNone,
};

static_assert(static_cast<std::uint8_t>(Level::kTrace) == 0);
static_assert(static_cast<std::uint8_t>(Level::kDebug) == 1);
static_assert(static_cast<std::uint8_t>(Level::kInfo) == 2);
static_assert(static_cast<std::uint8_t>(Level::kWarning) == 3);
static_assert(static_cast<std::uint8_t>(Level::kError) == 4);
static_assert(static_cast<std::uint8_t>(Level::kCritical) == 5);
static_assert(static_cast<std::uint8_t>(Level::kNone) == 6);

static_assert(std::is_trivially_copyable_v<Logger>);
static_assert(std::is_trivially_destructible_v<Logger>);
static_assert(sizeof(Logger) == sizeof(void*));

TEST(SourceLocation, CurrentCapturesTheCaller) {
  const std::uint_least32_t expected_line = __LINE__ + 1;
  const SourceLocation location = SourceLocation::Current();

  EXPECT_EQ(location.GetLine(), expected_line);
  EXPECT_NE(location.GetFileName().find("frontend_test.cpp"), std::string_view::npos);
  EXPECT_FALSE(location.GetFunctionName().empty());
}

TEST(SourceLocation, CustomPreservesAllFields) {
  constexpr SourceLocation location = SourceLocation::Custom("adapter.cpp", "Capture", 42, 7);

  EXPECT_EQ(location.GetFileName(), "adapter.cpp");
  EXPECT_EQ(location.GetFunctionName(), "Capture");
  EXPECT_EQ(location.GetLine(), 42U);
  EXPECT_EQ(location.GetColumn(), 7U);
}

TEST(Level, ThresholdOrderingIsExplicit) {
  EXPECT_TRUE(IsLevelEnabled(Level::kInfo, Level::kTrace));
  EXPECT_TRUE(IsLevelEnabled(Level::kInfo, Level::kInfo));
  EXPECT_FALSE(IsLevelEnabled(Level::kInfo, Level::kWarning));
  EXPECT_TRUE(IsLevelEnabled(Level::kCritical, Level::kCritical));
  EXPECT_FALSE(IsLevelEnabled(Level::kNone, Level::kTrace));
  EXPECT_FALSE(IsLevelEnabled(Level::kCritical, Level::kNone));
  EXPECT_FALSE(IsLevelEnabled(static_cast<Level>(255), Level::kTrace));
  EXPECT_FALSE(IsLevelEnabled(Level::kCritical, static_cast<Level>(255)));
}

TEST(Logger, InitialDefaultIsTheProcessWideNullLogger) {
  const Logger default_logger = GetDefaultLogger();
  const Logger null_logger = GetNullLogger();

  EXPECT_EQ(Logger{}, null_logger);
  EXPECT_EQ(default_logger, null_logger);
  EXPECT_EQ(LoadDefaultLoggerFromAnotherTranslationUnit(), default_logger);
  EXPECT_EQ(default_logger.GetLevel(), Level::kNone);

  for (const Level level : kLevels) {
    EXPECT_FALSE(default_logger.ShouldLog(level));
  }
}

TEST(Logger, NullLoggerDoesNotEvaluateMessageFactories) {
  const Logger logger = GetNullLogger();
  const SourceLocation location = SourceLocation::Current();
  std::size_t evaluations = 0;
  const auto message = [&]() -> std::string_view {
    ++evaluations;
    return "must not be evaluated";
  };

  logger.Log<Level::kTrace>(location, message);
  logger.Log<Level::kDebug>(location, message);
  logger.Log<Level::kInfo>(location, message);
  logger.Log<Level::kWarning>(location, message);
  logger.Log<Level::kError>(location, message);
  logger.Log<Level::kCritical>(location, message);
  logger.Log<Level::kNone>(location, message);

  EXPECT_EQ(evaluations, 0U);
}

}  // namespace
}  // namespace ulog::test
