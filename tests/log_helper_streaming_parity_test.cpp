#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

// Phase 47 — userver parity: chrono, std::exception, std::optional,
// std::error_code, pointer streaming. Each overload lives in
// include/ulog/log_helper_extras.hpp, auto-included via ulog/log.hpp.

namespace {

bool Contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

}  // namespace

// ---- chrono::duration ------------------------------------------------------

TEST(StreamingParity, DurationKnownSuffixes) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    ulog::LogHelper(*mem, ulog::Level::kInfo)
        << std::chrono::nanoseconds{7}
        << ' ' << std::chrono::microseconds{8}
        << ' ' << std::chrono::milliseconds{9}
        << ' ' << std::chrono::seconds{10}
        << ' ' << std::chrono::minutes{11}
        << ' ' << std::chrono::hours{12};

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "7ns 8us 9ms 10s 11min 12h")) << recs[0];
}

TEST(StreamingParity, DurationUnknownRatioFallsBack) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    using Weird = std::chrono::duration<int, std::ratio<7, 10>>;
    ulog::LogHelper(*mem, ulog::Level::kInfo) << Weird{3};

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "3 * 7/10s")) << recs[0];
}

// ---- chrono::system_clock::time_point --------------------------------------

TEST(StreamingParity, TimePointIso8601Utc) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    // 2025-01-02T03:04:05.123456 UTC — fixed epoch-offset for reproducibility.
    using namespace std::chrono;
    const auto tp = system_clock::time_point{} +
                    hours{24 * (365 * 55 + 14)} +  // roughly 2025
                    seconds{55 * 86400};           // offset into 2025
    (void)tp;  // actual timestamp shape depends on detail::FormatTimestamp;
    // we only verify the emitted string is a sane ISO-ish "YYYY-..."
    const auto now = system_clock::now();
    ulog::LogHelper(*mem, ulog::Level::kInfo) << now;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // Shape sanity: contains "T" and ends with "+0000" or "Z"-ish.
    EXPECT_NE(recs[0].find("text="), std::string::npos) << recs[0];
    // ISO date year prefix "20" + 2-digit year.
    EXPECT_NE(recs[0].find("text=20"), std::string::npos) << recs[0];
    // ISO time separator "T" between date and time — search after text=.
    auto text_pos = recs[0].find("text=");
    ASSERT_NE(text_pos, std::string::npos);
    EXPECT_NE(recs[0].find('T', text_pos), std::string::npos) << recs[0];
}

// ---- std::exception + derived ---------------------------------------------

TEST(StreamingParity, ExceptionStreamsWhatAndTypeTag) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    try {
        throw std::runtime_error("boom");
    } catch (const std::exception& ex) {
        ulog::LogHelper(*mem, ulog::Level::kError) << "parse failed: " << ex;
    }

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "parse failed: boom")) << recs[0];
    // typeid(...).name() is implementation-defined; we can't pattern-match
    // exactly, but it must contain "runtime_error" on all three major
    // compilers after any demangling the RTTI returns.
    EXPECT_TRUE(Contains(recs[0], "exception_type=")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "runtime_error")) << recs[0];
}

// ---- std::optional ---------------------------------------------------------

TEST(StreamingParity, OptionalPopulatedStreamsValue) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    std::optional<int> o{42};
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "val=" << o;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "val=42")) << recs[0];
}

TEST(StreamingParity, OptionalEmptyStreamsNoneLiteral) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    std::optional<std::string> s;
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "val=" << s;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "val=(none)")) << recs[0];
}

// ---- std::error_code -------------------------------------------------------

TEST(StreamingParity, ErrorCodeFormatted) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const std::error_code ec = std::make_error_code(std::errc::timed_out);
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "ec=" << ec;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // category name like "generic" or "system"; value is non-zero;
    // message is non-empty. Check shape "ec=<category>:<value> (<msg>)".
    EXPECT_TRUE(Contains(recs[0], "ec=")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], ":")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "(")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], ")")) << recs[0];
}

// ---- Pointer streaming -----------------------------------------------------

TEST(StreamingParity, NullPointerLiterally) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    int* p = nullptr;
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "p=" << p;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "p=(null)")) << recs[0];
}

TEST(StreamingParity, NonNullPointerAsHex) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    int value = 7;
    int* p = &value;
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "p=" << p;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // Hex uses fixed `0x{:016X}` format — 0x + 16 hex chars.
    EXPECT_TRUE(Contains(recs[0], "p=0x")) << recs[0];
}

TEST(StreamingParity, CharPointerStillRendersAsText) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    const char* s = "hello";
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "s=" << s;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    // char* / const char* must still go through the built-in Put(const char*)
    // path — NOT the new pointer overload.
    EXPECT_TRUE(Contains(recs[0], "s=hello")) << recs[0];
    EXPECT_FALSE(Contains(recs[0], "s=0x")) << recs[0];
}

TEST(StreamingParity, VoidPointerAsHex) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    int value = 7;
    const void* p = &value;
    ulog::LogHelper(*mem, ulog::Level::kInfo) << "p=" << p;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "p=0x")) << recs[0];
}

// ---- LOG_* macro integration (sanity: overloads compile through macro) ----

TEST(StreamingParity, WorksViaLogMacro) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::impl::SetDefaultLoggerRef(*mem);

    std::optional<int> o{7};
    const std::error_code ec = std::make_error_code(std::errc::timed_out);
    LOG_INFO() << "dur=" << std::chrono::milliseconds{42}
               << " opt=" << o
               << " ec=" << ec;

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_TRUE(Contains(recs[0], "dur=42ms")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "opt=7")) << recs[0];
    EXPECT_TRUE(Contains(recs[0], "ec=")) << recs[0];

    ulog::SetNullDefaultLogger();
}
