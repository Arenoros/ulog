#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

namespace {

std::shared_ptr<ulog::MemLogger> InstallMem() {
    auto m = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    m->SetLevel(ulog::Level::kTrace);
    ulog::SetDefaultLogger(m);
    return m;
}

}  // namespace

TEST(RateLimit, DropsRepeatedCalls) {
    auto mem = InstallMem();
    // Rate limiter emits at count = 1 and every subsequent power of two.
    for (int i = 0; i < 32; ++i) LOG_LIMITED_INFO() << "x " << i;
    const auto recs = mem->GetRecords();
    // Expected emits at counts 1, 2, 4, 8, 16, 32 — that's 6.
    EXPECT_EQ(recs.size(), 6u);
    ulog::SetDefaultLogger(nullptr);
}

TEST(RateLimit, DroppedSuffixPresent) {
    auto mem = InstallMem();
    // Flood past the first emit; subsequent records carry [dropped=N].
    for (int i = 0; i < 10; ++i) LOG_LIMITED_INFO() << "msg " << i;
    const auto recs = mem->GetRecords();
    ASSERT_GE(recs.size(), 2u);
    // First record is count == 1, drops still zero — no suffix.
    EXPECT_EQ(recs[0].find("[dropped="), std::string::npos);
    // Second record is count == 2, dropped count == 0 → no suffix either
    // (drops only accumulate between emit slots).
    // The 4th emit (count == 4) has seen drops at counts 3 → expect suffix.
    bool saw_dropped_suffix = false;
    for (const auto& r : recs) {
        if (r.find("[dropped=") != std::string::npos) { saw_dropped_suffix = true; break; }
    }
    EXPECT_TRUE(saw_dropped_suffix);
    ulog::SetDefaultLogger(nullptr);
}

TEST(RateLimit, GlobalDroppedCounterAggregates) {
    auto mem = InstallMem();
    ulog::ResetRateLimitStats();
    for (int i = 0; i < 64; ++i) LOG_LIMITED_INFO() << "flood " << i;
    const auto emitted = mem->GetRecords().size();
    const auto dropped = ulog::GetRateLimitDroppedTotal();
    EXPECT_EQ(emitted + dropped, 64u);
    EXPECT_GT(dropped, 0u);
    ulog::ResetRateLimitStats();
    EXPECT_EQ(ulog::GetRateLimitDroppedTotal(), 0u);
    ulog::SetDefaultLogger(nullptr);
}

TEST(RateLimit, ResetsAfterOneSecond) {
    auto mem = InstallMem();
    for (int i = 0; i < 16; ++i) LOG_LIMITED_INFO() << "first " << i;
    const auto before = mem->GetRecords().size();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    LOG_LIMITED_INFO() << "post-reset";
    const auto after = mem->GetRecords().size();

    // First new record after reset always emits (count == 1).
    EXPECT_EQ(after, before + 1u);
    ulog::SetDefaultLogger(nullptr);
}
