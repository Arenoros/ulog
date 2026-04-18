#include <ulog/level.hpp>

#include <gtest/gtest.h>

using ulog::Level;

TEST(LevelTest, RoundTripLower) {
    EXPECT_EQ(ulog::LevelFromString("trace"),    Level::kTrace);
    EXPECT_EQ(ulog::LevelFromString("debug"),    Level::kDebug);
    EXPECT_EQ(ulog::LevelFromString("info"),     Level::kInfo);
    EXPECT_EQ(ulog::LevelFromString("warning"),  Level::kWarning);
    EXPECT_EQ(ulog::LevelFromString("error"),    Level::kError);
    EXPECT_EQ(ulog::LevelFromString("critical"), Level::kCritical);
    EXPECT_EQ(ulog::LevelFromString("none"),     Level::kNone);
}

TEST(LevelTest, CaseInsensitive) {
    EXPECT_EQ(ulog::LevelFromString("INFO"), Level::kInfo);
    EXPECT_EQ(ulog::LevelFromString("Warning"), Level::kWarning);
}

TEST(LevelTest, UnknownThrows) {
    EXPECT_THROW(ulog::LevelFromString("garbage"), std::runtime_error);
}

TEST(LevelTest, ToStringRoundtrip) {
    EXPECT_EQ(ulog::ToString(Level::kTrace), "trace");
    EXPECT_EQ(ulog::ToString(Level::kCritical), "critical");
    EXPECT_EQ(ulog::ToUpperCaseString(Level::kTrace), "TRACE");
    EXPECT_EQ(ulog::ToUpperCaseString(Level::kInfo), "INFO");
}

TEST(LevelTest, OptionalParse) {
    EXPECT_EQ(ulog::OptionalLevelFromString({}), std::nullopt);
    EXPECT_EQ(ulog::OptionalLevelFromString(std::optional<std::string>{"debug"}), Level::kDebug);
}
