#include <gtest/gtest.h>

#include <ulog/log.hpp>
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

    auto log_ptr = ulog::GetDefaultLoggerPtr();
    auto& log = *log_ptr;  // pin with the shared_ptr snapshot.
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

TEST(DefaultLogger, LevelGuards) {
    const auto prev = ulog::GetDefaultLoggerLevel();
    {
        ulog::DefaultLoggerLevelScope scope(ulog::Level::kTrace);
        EXPECT_EQ(ulog::GetDefaultLoggerLevel(), ulog::Level::kTrace);
    }
    EXPECT_EQ(ulog::GetDefaultLoggerLevel(), prev);
}
