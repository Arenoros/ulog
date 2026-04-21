#include <memory>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>
#include <ulog/null_logger.hpp>

// Null logger discards everything — these tests only verify compilability
// of all LOG_* macro forms and basic runtime safety. Real output tests
// arrive together with the mem_logger / formatters (Phases 3-5).

TEST(LogMacros, CompileCoverage) {
    LOG_TRACE() << "trace";
    LOG_DEBUG() << "debug";
    LOG_INFO() << "info value=" << 42;
    LOG_WARNING() << "warning " << ulog::Hex{0xdeadbeef};
    LOG_ERROR() << "error " << ulog::HexShort{0xabc};
    LOG_CRITICAL() << "critical " << ulog::Quoted{"quoted"};

    auto& log = ulog::GetDefaultLogger();
    LOG_INFO_TO(log) << "to-default";
    LOG_ERROR_TO(log) << "to-default err";

    LOG(ulog::Level::kInfo) << "level";
    LOG_TO(log, ulog::Level::kInfo) << "explicit";
}

TEST(LogMacros, LimitedCompiles) {
    for (int i = 0; i < 100; ++i) {
        LOG_LIMITED_INFO() << "tick " << i;
        LOG_LIMITED_ERROR() << "tick " << i;
    }
    LOG_LIMITED(ulog::Level::kWarning) << "explicit limited";
}

TEST(LogMacros, LogExtraStream) {
    ulog::LogExtra e{{"k", std::string("v")}, {"n", 7}};
    LOG_INFO() << "msg" << e;
}

TEST(LogMacros, LFMTDeliversFormattedPayload) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::impl::SetDefaultLoggerRef(*mem);

    LFMT_INFO("user={} id={}", "alice", 42);
    LFMT_ERROR("delta={:.2f}", 3.14159);

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_NE(recs[0].find("text=user=alice id=42"), std::string::npos) << recs[0];
    EXPECT_NE(recs[1].find("text=delta=3.14"), std::string::npos) << recs[1];
    ulog::SetNullDefaultLogger();
}

TEST(LogMacros, LPRINTDeliversPrintfPayload) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);
    ulog::impl::SetDefaultLoggerRef(*mem);

    LPRINT_INFO("code=%d path=%s", 7, "/api/x");
    LPRINT_WARNING("pct=%5.2f%%", 99.9);

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_NE(recs[0].find("text=code=7 path=/api/x"), std::string::npos) << recs[0];
    EXPECT_NE(recs[1].find("text=pct=99.90%"), std::string::npos) << recs[1];
    ulog::SetNullDefaultLogger();
}

TEST(LogMacros, LFMTSkipsFormatWhenLevelFiltered) {
    // Level gate short-circuits before the fmt::format arguments are
    // evaluated, so a side-effecting argument must not run on a
    // dropped record.
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kError);
    ulog::impl::SetDefaultLoggerRef(*mem);

    int called = 0;
    auto make = [&] { ++called; return 99; };
    LFMT_INFO("x={}", make());  // level INFO < ERROR → dropped
    EXPECT_EQ(called, 0);
    EXPECT_TRUE(mem->GetRecords().empty());

    LFMT_ERROR("x={}", make());  // accepted
    EXPECT_EQ(called, 1);
    EXPECT_EQ(mem->GetRecords().size(), 1u);
    ulog::SetNullDefaultLogger();
}

TEST(LogMacros, LFMTAndLPRINTToExplicitLogger) {
    auto mem = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    mem->SetLevel(ulog::Level::kTrace);

    LFMT_INFO_TO(*mem, "hi {}", 1);
    LPRINT_WARNING_TO(*mem, "pct=%d", 42);

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_NE(recs[0].find("text=hi 1"), std::string::npos) << recs[0];
    EXPECT_NE(recs[1].find("text=pct=42"), std::string::npos) << recs[1];
}

TEST(DefaultLogger, LevelGuards) {
    const auto prev = ulog::GetDefaultLoggerLevel();
    {
        ulog::DefaultLoggerLevelScope scope(ulog::Level::kTrace);
        EXPECT_EQ(ulog::GetDefaultLoggerLevel(), ulog::Level::kTrace);
    }
    EXPECT_EQ(ulog::GetDefaultLoggerLevel(), prev);
}
