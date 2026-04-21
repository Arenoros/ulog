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
    ulog::impl::SetDefaultLoggerRef(*logger);
    return logger;
}

}  // namespace

TEST(DynamicDebug, ForceEnableBypassesLoggerLevel) {
    auto mem = InstallMem();

    // Below logger level (kWarning); normally suppressed.
    LOG_INFO() << "suppressed";
    ASSERT_TRUE(mem->GetRecords().empty());

    // Force-enable the whole file.
    ulog::AddDynamicDebugLog("dynamic_debug_test.cpp");
    LOG_INFO() << "forced";

    const auto recs = mem->GetRecords();
    ASSERT_EQ(recs.size(), 1u);
    EXPECT_NE(recs.front().find("text=forced"), std::string::npos);

    
    ulog::SetNullDefaultLogger();
}

TEST(DynamicDebug, ForceDisableSuppressesEvenAboveLevel) {
    auto mem = InstallMem();
    mem->SetLevel(ulog::Level::kTrace);
    

    ulog::RemoveDynamicDebugLog("dynamic_debug_test.cpp");
    LOG_ERROR() << "should-not-appear";
    EXPECT_TRUE(mem->GetRecords().empty());

    
    ulog::SetNullDefaultLogger();
}

TEST(DynamicDebug, ForEachLogEntryEnumeratesRegisteredSites) {
    
    auto mem = InstallMem();

    // Expand two LOG_* macros at distinct lines; the expansions register
    // one StaticLogEntry per site.
    const int marker_line_a = __LINE__ + 1;
    LOG_INFO() << "enum-a";
    const int marker_line_b = __LINE__ + 1;
    LOG_DEBUG() << "enum-b";

    bool saw_a = false;
    bool saw_b = false;
    for (auto&& entry : ulog::GetDynamicDebugLocations()) {
        ASSERT_NE(entry.path, nullptr);
        const std::string_view file(entry.path);
        if (file.find("dynamic_debug_test") == std::string_view::npos) continue;
        if (entry.line == marker_line_a) saw_a = true;
        if (entry.line == marker_line_b) saw_b = true;
    }

    EXPECT_TRUE(saw_a);
    EXPECT_TRUE(saw_b);

    ulog::SetNullDefaultLogger();
}

TEST(DynamicDebug, ForEachLogEntryReflectsOverrideState) {
    

    const int target_line = __LINE__ + 1;
    LOG_INFO() << "state-probe";

    // Default state before any override.
    ulog::DynamicDebugState observed = ulog::DynamicDebugState::kForceEnabled;
    bool found = false;
    for (auto&& entry : ulog::GetDynamicDebugLocations()) {
        if (entry.line != target_line) continue;
        if (std::string_view(entry.path).find("dynamic_debug_test") == std::string_view::npos) continue;
        observed = entry.state.load().force_enabled_level != ulog::Level::kNone ? ulog::DynamicDebugState::kForceEnabled : ulog::DynamicDebugState::kDefault;
        found = true;
    }
    ASSERT_TRUE(found);
    EXPECT_EQ(observed, ulog::DynamicDebugState::kDefault);

    // Flip override and re-enumerate — state must track the registry.
    ulog::RemoveDynamicDebugLog("dynamic_debug_test.cpp", target_line);
    found = false;
    
    for (auto&& entry : ulog::GetDynamicDebugLocations()) {
        if (entry.line != target_line) continue;
        if (std::string_view(entry.path).find("dynamic_debug_test") == std::string_view::npos) continue;
        observed = entry.state.load().force_enabled_level != ulog::Level::kNone ? ulog::DynamicDebugState::kForceEnabled : ulog::DynamicDebugState::kDefault;
        found = true;
    }
    ASSERT_TRUE(found);
    EXPECT_EQ(observed, ulog::DynamicDebugState::kForceDisabled);

    
}

TEST(DynamicDebug, ForEachLogEntryNullCallbackIsNoop) {
    // Must not crash. Nothing else to assert.
    for (auto&& entry : ulog::GetDynamicDebugLocations()) {
        (void)(entry);
    }
}

TEST(DynamicDebug, ResetRestoresDefault) {
    auto mem = InstallMem();
    ulog::AddDynamicDebugLog("dynamic_debug_test.cpp");
    LOG_DEBUG() << "x";
    ASSERT_EQ(mem->GetRecords().size(), 1u);

    
    mem->Clear();
    LOG_DEBUG() << "suppressed-again";
    EXPECT_TRUE(mem->GetRecords().empty());

    ulog::SetNullDefaultLogger();
}
