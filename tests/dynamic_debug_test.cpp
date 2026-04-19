#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <string_view>
#include <unordered_set>

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

TEST(DynamicDebug, ForEachLogEntryEnumeratesRegisteredSites) {
    ulog::ResetDynamicDebugLog();
    auto mem = InstallMem();

    // Expand two LOG_* macros at distinct lines; the expansions register
    // one StaticLogEntry per site.
    const int marker_line_a = __LINE__ + 1;
    LOG_INFO() << "enum-a";
    const int marker_line_b = __LINE__ + 1;
    LOG_DEBUG() << "enum-b";

    bool saw_a = false;
    bool saw_b = false;
    ulog::ForEachLogEntry([&](const ulog::LogEntryInfo& info) {
        ASSERT_NE(info.file, nullptr);
        const std::string_view file(info.file);
        if (file.find("dynamic_debug_test") == std::string_view::npos) return;
        if (info.line == marker_line_a) saw_a = true;
        if (info.line == marker_line_b) saw_b = true;
    });

    EXPECT_TRUE(saw_a);
    EXPECT_TRUE(saw_b);

    ulog::SetDefaultLogger(nullptr);
}

TEST(DynamicDebug, ForEachLogEntryReflectsOverrideState) {
    ulog::ResetDynamicDebugLog();

    const int target_line = __LINE__ + 1;
    LOG_INFO() << "state-probe";

    // Default state before any override.
    ulog::DynamicDebugState observed = ulog::DynamicDebugState::kForceEnabled;
    bool found = false;
    ulog::ForEachLogEntry([&](const ulog::LogEntryInfo& info) {
        if (info.line != target_line) return;
        if (std::string_view(info.file).find("dynamic_debug_test") == std::string_view::npos) return;
        observed = info.state;
        found = true;
    });
    ASSERT_TRUE(found);
    EXPECT_EQ(observed, ulog::DynamicDebugState::kDefault);

    // Flip override and re-enumerate — state must track the registry.
    ulog::DisableDynamicDebugLog("dynamic_debug_test.cpp", target_line);
    found = false;
    ulog::ForEachLogEntry([&](const ulog::LogEntryInfo& info) {
        if (info.line != target_line) return;
        if (std::string_view(info.file).find("dynamic_debug_test") == std::string_view::npos) return;
        observed = info.state;
        found = true;
    });
    ASSERT_TRUE(found);
    EXPECT_EQ(observed, ulog::DynamicDebugState::kForceDisabled);

    ulog::ResetDynamicDebugLog();
}

TEST(DynamicDebug, ForEachLogEntryNullCallbackIsNoop) {
    // Must not crash. Nothing else to assert.
    ulog::ForEachLogEntry({});
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
