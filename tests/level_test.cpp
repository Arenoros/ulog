#include <gtest/gtest.h>

#include <bit>
#include <cstdint>
#include <string_view>
#include <ulog/level.hpp>

namespace ulog::test {
namespace {

struct LevelName final {
  Level level;
  std::string_view lower;
  std::string_view upper;
};

constexpr LevelName kLevels[] = {
    {Level::kTrace, "trace", "TRACE"}, {Level::kDebug, "debug", "DEBUG"},
    {Level::kInfo, "info", "INFO"},    {Level::kWarning, "warning", "WARNING"},
    {Level::kError, "error", "ERROR"}, {Level::kCritical, "critical", "CRITICAL"},
    {Level::kNone, "none", "NONE"},
};

static_assert(ParseLevel("Warning").level == Level::kWarning);
static_assert(ToString(Level::kCritical) == "critical");

TEST(LevelNames, SerializationRoundTripsInBothCases) {
  for (const auto& name : kLevels) {
    SCOPED_TRACE(name.lower);
    EXPECT_EQ(ToString(name.level), name.lower);
    EXPECT_EQ(ToUpperCaseString(name.level), name.upper);
    EXPECT_EQ(ParseLevel(ToString(name.level)).level, name.level);
    EXPECT_EQ(ParseLevel(ToUpperCaseString(name.level)).level, name.level);
    EXPECT_TRUE(ParseLevel(name.lower));
    EXPECT_TRUE(ParseLevel(name.lower).HowToFix().empty());
  }
}

TEST(LevelNames, ParsingIgnoresAsciiCase) {
  EXPECT_EQ(ParseLevel("WaRnInG").level, Level::kWarning);
  EXPECT_EQ(ParseLevel("Info").level, Level::kInfo);
  EXPECT_EQ(ParseLevel("nOnE").level, Level::kNone);
}

TEST(LevelNames, RejectsEverythingElseWithTheAcceptedSet) {
  using namespace std::string_view_literals;
  constexpr std::string_view kInvalid[] = {""sv,      "warn"sv, "infos"sv, " info"sv,  "info "sv,
                                           "fatal"sv, "err"sv,  "6"sv,     "trace\0"sv};
  for (const std::string_view name : kInvalid) {
    SCOPED_TRACE(name);
    const LevelParseResult parsed = ParseLevel(name);
    EXPECT_FALSE(parsed);
    for (const auto& level : kLevels) {
      EXPECT_NE(parsed.HowToFix().find(level.lower), std::string_view::npos);
    }
  }
}

TEST(LevelNames, NonEnumeratorValuesHaveNoName) {
  const auto invalid = std::bit_cast<Level>(std::uint8_t{7});
  EXPECT_TRUE(ToString(invalid).empty());
  EXPECT_TRUE(ToUpperCaseString(invalid).empty());
}

}  // namespace
}  // namespace ulog::test
