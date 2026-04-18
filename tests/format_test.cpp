#include <ulog/format.hpp>

#include <gtest/gtest.h>

using ulog::Format;

TEST(FormatTest, Parse) {
    EXPECT_EQ(ulog::FormatFromString("tskv"), Format::kTskv);
    EXPECT_EQ(ulog::FormatFromString("ltsv"), Format::kLtsv);
    EXPECT_EQ(ulog::FormatFromString("raw"),  Format::kRaw);
    EXPECT_EQ(ulog::FormatFromString("json"), Format::kJson);
    EXPECT_EQ(ulog::FormatFromString("json_yadeploy"), Format::kJsonYaDeploy);
}

TEST(FormatTest, UnknownThrows) {
    EXPECT_THROW(ulog::FormatFromString("xml"), std::runtime_error);
}

TEST(FormatTest, ToString) {
    EXPECT_EQ(ulog::ToString(Format::kTskv), "tskv");
    EXPECT_EQ(ulog::ToString(Format::kJson), "json");
}
