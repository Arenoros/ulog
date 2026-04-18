#include <gtest/gtest.h>

#include <ulog/dynamic_debug.hpp>
#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

namespace {

std::shared_ptr<ulog::MemLogger> InstallMem() {
    auto logger = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kWarning);
    ulog::SetDefaultLogger(logger);
    return logger;
}

}  // namespace

TEST(DynamicDebug, ForceEnableBypassesLoggerLevel) {
    auto mem = InstallMem();
    ulog::ResetDynamicDebugLog();

    // Below logger level (kWarning); normally suppressed.
    LOG_INFO() << "suppressed";
    ASSERT_TRUE(mem->GetRecords().empty());

    // Force-enable the whole file.
    ulog::EnableDynamicDebugLog("dynamic_debug_test.cpp");
    LOG_INFO() << "forced";

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_NE(recs.front().find("text=forced"), std::string::npos);

    ulog::ResetDynamicDebugLog();
    ulog::SetDefaultLogger(nullptr);
}

TEST(DynamicDebug, ForceDisableSuppressesEvenAboveLevel) {
    auto mem = InstallMem();
    mem->SetLevel(ulog::Level::kTrace);
    ulog::ResetDynamicDebugLog();

    ulog::DisableDynamicDebugLog("dynamic_debug_test.cpp");
    LOG_ERROR() << "should-not-appear";
    EXPECT_TRUE(mem->GetRecords().empty());

    ulog::ResetDynamicDebugLog();
    ulog::SetDefaultLogger(nullptr);
}

TEST(DynamicDebug, ResetRestoresDefault) {
    auto mem = InstallMem();
    ulog::EnableDynamicDebugLog("dynamic_debug_test.cpp");
    LOG_DEBUG() << "x";
    ASSERT_EQ(mem->GetRecords().size(), 1u);

    ulog::ResetDynamicDebugLog();
    mem->Clear();
    LOG_DEBUG() << "suppressed-again";
    EXPECT_TRUE(mem->GetRecords().empty());

    ulog::SetDefaultLogger(nullptr);
}
