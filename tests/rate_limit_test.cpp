#include <atomic>
#include <cstring>
#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

namespace {

std::shared_ptr<ulog::MemLogger> InstallMem() {
    auto m = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    m->SetLevel(ulog::Level::kTrace);
    ulog::impl::SetDefaultLoggerRef(*m);
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
    ulog::SetNullDefaultLogger();
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
    ulog::SetNullDefaultLogger();
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
    ulog::SetNullDefaultLogger();
}

namespace {

// Test harness for the drop handler — plain free function, captures
// per-event data into file-local atomics so the test can observe every
// invocation without TLS gymnastics. Limits: one drop-handler per
// process; a multi-case suite must install/uninstall between tests.
std::atomic<std::uint64_t> g_handler_calls{0};
std::atomic<std::uint64_t> g_handler_last_site_dropped{0};
std::atomic<std::uint64_t> g_handler_last_total_dropped{0};
std::atomic<int> g_handler_last_line{0};
std::atomic<const char*> g_handler_last_file{nullptr};

void TestDropHandler(const ulog::RateLimitDropEvent& ev) noexcept {
    g_handler_calls.fetch_add(1, std::memory_order_relaxed);
    g_handler_last_site_dropped.store(ev.site_dropped, std::memory_order_relaxed);
    g_handler_last_total_dropped.store(ev.total_dropped, std::memory_order_relaxed);
    g_handler_last_line.store(ev.line, std::memory_order_relaxed);
    g_handler_last_file.store(ev.file, std::memory_order_relaxed);
}

void ResetHandlerCounters() noexcept {
    g_handler_calls.store(0, std::memory_order_relaxed);
    g_handler_last_site_dropped.store(0, std::memory_order_relaxed);
    g_handler_last_total_dropped.store(0, std::memory_order_relaxed);
    g_handler_last_line.store(0, std::memory_order_relaxed);
    g_handler_last_file.store(nullptr, std::memory_order_relaxed);
}

}  // namespace

TEST(RateLimit, DropHandlerFiresOnSuppressedCalls) {
    auto mem = InstallMem();
    ulog::ResetRateLimitStats();
    ResetHandlerCounters();
    ulog::SetRateLimitDropHandler(&TestDropHandler);

    // 32 calls → 6 emits (1/2/4/8/16/32) and 26 drops.
    constexpr int kN = 32;
    for (int i = 0; i < kN; ++i) LOG_LIMITED_INFO() << "probe " << i;

    ulog::SetRateLimitDropHandler(nullptr);  // deregister before assertions.

    const auto calls = g_handler_calls.load(std::memory_order_relaxed);
    const auto emitted = mem->GetRecords().size();
    EXPECT_EQ(calls + emitted, static_cast<std::uint64_t>(kN));
    EXPECT_GT(calls, 0u);
    // Last handler invocation reports the aggregate at that moment.
    EXPECT_EQ(g_handler_last_total_dropped.load(std::memory_order_relaxed), calls);
    // Site `file` points at *this* TU (path-suffix match is enough — the
    // exact trimmed prefix depends on ULOG_SOURCE_ROOT).
    const char* file = g_handler_last_file.load(std::memory_order_relaxed);
    ASSERT_NE(file, nullptr);
    EXPECT_NE(std::strstr(file, "rate_limit_test.cpp"), nullptr) << file;
    EXPECT_GT(g_handler_last_line.load(std::memory_order_relaxed), 0);
    ulog::SetNullDefaultLogger();
}

TEST(RateLimit, DropHandlerNotCalledWhenDeregistered) {
    auto mem = InstallMem();
    ulog::ResetRateLimitStats();
    ResetHandlerCounters();

    // No handler installed — producer path must not call anything.
    for (int i = 0; i < 32; ++i) LOG_LIMITED_INFO() << "silent " << i;
    EXPECT_EQ(g_handler_calls.load(std::memory_order_relaxed), 0u);

    // Install, flood once, then uninstall with nullptr. After uninstall,
    // further drops must not invoke the stale pointer.
    ulog::SetRateLimitDropHandler(&TestDropHandler);
    for (int i = 0; i < 32; ++i) LOG_LIMITED_INFO() << "metered " << i;
    const auto installed_calls = g_handler_calls.load(std::memory_order_relaxed);
    EXPECT_GT(installed_calls, 0u);

    ulog::SetRateLimitDropHandler(nullptr);
    for (int i = 0; i < 32; ++i) LOG_LIMITED_INFO() << "silent-again " << i;
    EXPECT_EQ(g_handler_calls.load(std::memory_order_relaxed), installed_calls);

    ulog::SetNullDefaultLogger();
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
    ulog::SetNullDefaultLogger();
}
