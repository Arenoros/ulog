#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/mem_logger.hpp>

namespace {

std::shared_ptr<ulog::MemLogger> MakeMem() {
    auto m = std::make_shared<ulog::MemLogger>(ulog::Format::kTskv);
    m->SetLevel(ulog::Level::kTrace);
    return m;
}

}  // namespace

TEST(DefaultLoggerGuard, SwapsDefaultWithinScope) {
    auto outer = MakeMem();
    auto inner = MakeMem();
    ulog::SetDefaultLogger(outer);

    LOG_INFO() << "outer-first";
    {
        ulog::DefaultLoggerGuard guard(inner);
        LOG_INFO() << "inside-guard";
    }
    LOG_INFO() << "outer-second";

    EXPECT_EQ(outer->GetRecords().size(), 2u);
    EXPECT_EQ(inner->GetRecords().size(), 1u);
    EXPECT_NE(inner->GetRecords().front().find("text=inside-guard"), std::string::npos);

    ulog::SetDefaultLogger(nullptr);
}

TEST(DefaultLoggerGuard, NestedGuardsRestoreInOrder) {
    auto a = MakeMem();
    auto b = MakeMem();
    auto c = MakeMem();
    ulog::SetDefaultLogger(a);

    LOG_INFO() << "to-a";
    {
        ulog::DefaultLoggerGuard g1(b);
        LOG_INFO() << "to-b";
        {
            ulog::DefaultLoggerGuard g2(c);
            LOG_INFO() << "to-c";
        }
        LOG_INFO() << "back-to-b";
    }
    LOG_INFO() << "back-to-a";

    EXPECT_EQ(a->GetRecords().size(), 2u);
    EXPECT_EQ(b->GetRecords().size(), 2u);
    EXPECT_EQ(c->GetRecords().size(), 1u);

    ulog::SetDefaultLogger(nullptr);
}

TEST(DefaultLogger, TlsCacheInvalidatesOnSet) {
    auto a = MakeMem();
    auto b = MakeMem();
    ulog::SetDefaultLogger(a);
    LOG_INFO() << "into-a";
    ulog::SetDefaultLogger(b);
    LOG_INFO() << "into-b";
    EXPECT_EQ(a->GetRecords().size(), 1u);
    EXPECT_EQ(b->GetRecords().size(), 1u);
    EXPECT_NE(a->GetRecords().front().find("text=into-a"), std::string::npos);
    EXPECT_NE(b->GetRecords().front().find("text=into-b"), std::string::npos);
    ulog::SetDefaultLogger(nullptr);
}

TEST(DefaultLogger, TlsCacheHotPathRoutesAllRecords) {
    auto a = MakeMem();
    ulog::SetDefaultLogger(a);
    for (int i = 0; i < 100; ++i) LOG_INFO() << "msg " << i;
    EXPECT_EQ(a->GetRecords().size(), 100u);
    ulog::SetDefaultLogger(nullptr);
}

TEST(DefaultLogger, ConcurrentSwapWhileLoggingDoesNotCrash) {
    // Verifies the boost::atomic_shared_ptr snapshot path: readers must not
    // dangle when SetDefaultLogger replaces the slot mid-log.
    auto a = MakeMem();
    auto b = MakeMem();
    ulog::SetDefaultLogger(a);

    std::atomic<bool> stop{false};
    constexpr int kProducers = 4;
    std::vector<std::thread> threads;
    for (int t = 0; t < kProducers; ++t) {
        threads.emplace_back([&stop, t] {
            int i = 0;
            while (!stop.load(std::memory_order_relaxed)) {
                LOG_INFO() << "t=" << t << " i=" << (i++);
            }
        });
    }

    // Flip the default ~200 times across ~200ms.
    for (int i = 0; i < 200; ++i) {
        ulog::SetDefaultLogger((i % 2 == 0) ? b : a);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    // Every record landed in either a or b; nothing dangled. Counts are
    // intentionally not asserted — the point is no TSAN / segfault.
    EXPECT_GT(a->GetRecords().size() + b->GetRecords().size(), 0u);

    ulog::SetDefaultLogger(nullptr);
}
