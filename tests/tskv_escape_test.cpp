#include <ulog/detail/tskv_escape.hpp>

#include <string>
#include <gtest/gtest.h>

using ulog::detail::EncodeTskv;
using ulog::detail::TskvMode;

TEST(TskvEscape, Plain) {
    std::string out;
    EncodeTskv(out, std::string_view("hello"), TskvMode::kValue);
    EXPECT_EQ(out, "hello");
}

TEST(TskvEscape, Tab) {
    std::string out;
    EncodeTskv(out, std::string_view("a\tb"), TskvMode::kValue);
    EXPECT_EQ(out, "a\\tb");
}

TEST(TskvEscape, Newline) {
    std::string out;
    EncodeTskv(out, std::string_view("a\nb"), TskvMode::kValue);
    EXPECT_EQ(out, "a\\nb");
}

TEST(TskvEscape, EqualsInKey) {
    std::string out;
    EncodeTskv(out, std::string_view("a=b"), TskvMode::kKey);
    EXPECT_EQ(out, "a\\=b");
}

TEST(TskvEscape, EqualsInValuePassthrough) {
    std::string out;
    EncodeTskv(out, std::string_view("a=b"), TskvMode::kValue);
    EXPECT_EQ(out, "a=b");
}

TEST(TskvEscape, PeriodReplacement) {
    std::string out;
    EncodeTskv(out, std::string_view("a.b.c"), TskvMode::kKeyReplacePeriod);
    EXPECT_EQ(out, "a_b_c");
}

TEST(TskvEscape, Backslash) {
    std::string out;
    EncodeTskv(out, std::string_view("a\\b"), TskvMode::kValue);
    EXPECT_EQ(out, "a\\\\b");
}
