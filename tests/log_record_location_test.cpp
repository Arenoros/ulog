#include <string_view>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/log_helper.hpp>
#include <ulog/mem_logger.hpp>

namespace {

auto CaptureOneRecord(ulog::Format fmt = ulog::Format::kTskv) {
    auto logger = std::make_shared<ulog::MemLogger>(fmt);
    ulog::SetDefaultLogger(logger);
    return logger;
}

std::string ExtractTskvValue(std::string_view rec, std::string_view key) {
    const std::string needle = std::string(key) + "=";
    const auto p = rec.find(needle);
    if (p == std::string_view::npos) return {};
    const auto start = p + needle.size();
    const auto end = rec.find('\t', start);
    return std::string(rec.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

}  // namespace

TEST(LogRecordLocation, CurrentCapturesCallerFile) {
    const auto here = ulog::LogRecordLocation::Current();
    // The file path must end with this test's filename — exact path
    // depends on source-root trimming, but the basename is stable.
    const std::string file{here.file_name()};
    EXPECT_NE(file.find("log_record_location_test"), std::string::npos) << file;
}

TEST(LogRecordLocation, CurrentCapturesCallerLine) {
    const int expected_line = __LINE__ + 1;
    const auto here = ulog::LogRecordLocation::Current();
    EXPECT_EQ(static_cast<int>(here.line()), expected_line);
}

TEST(LogRecordLocation, LineStringIsPrecomputedDecimal) {
    // Construct via the 3-arg public ctor with a known line — the
    // precomputed decimal string must match the value.
    const ulog::LogRecordLocation loc{"file.cpp", 12345, "fn"};
    EXPECT_EQ(static_cast<int>(loc.line()), 12345);
    EXPECT_EQ(loc.line_string(), std::string_view{"12345"});
}

TEST(LogRecordLocation, ZeroLineStillRendersZero) {
    const ulog::LogRecordLocation loc{"f", 0, "fn"};
    EXPECT_EQ(loc.line_string(), std::string_view{"0"});
}

TEST(LogRecordLocation, EightDigitLineFitsBuffer) {
    // Internal buffer is 8 bytes. A 99999999-line source is not realistic
    // but proves the boundary works.
    const ulog::LogRecordLocation loc{"f", 99999999, "fn"};
    EXPECT_EQ(loc.line_string(), std::string_view{"99999999"});
}

TEST(LogRecordLocation, HasValueDistinguishesEmpty) {
    EXPECT_FALSE(ulog::LogRecordLocation{}.has_value());
    EXPECT_TRUE((ulog::LogRecordLocation{"f", 1, ""}).has_value());
    EXPECT_TRUE((ulog::LogRecordLocation{"", 1, "fn"}).has_value());
    EXPECT_FALSE((ulog::LogRecordLocation{"", 0, ""}).has_value());
}

TEST(LogRecordLocation, TskvModuleUsesPrecomputedLine) {
    auto mem = CaptureOneRecord();
    const int expected_line = __LINE__ + 1;
    LOG_INFO() << "x";
    ulog::SetDefaultLogger(nullptr);

    ASSERT_EQ(mem->GetRecords().size(), 1u);
    const auto module_value = ExtractTskvValue(mem->GetRecords().front(), "module");
    // module shape: `<function> ( <file>:<line> )` — check the line suffix.
    const std::string line_suffix = ":" + std::to_string(expected_line) + " )";
    EXPECT_NE(module_value.find(line_suffix), std::string::npos) << module_value;
}
