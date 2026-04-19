#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ulog/async_logger.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace {

class CapturingSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view record) override {
        std::lock_guard lock(mu_);
        records_.emplace_back(record);
    }
    std::vector<std::string> GetRecords() const {
        std::lock_guard lock(mu_);
        return records_;
    }
    std::size_t Size() const {
        std::lock_guard lock(mu_);
        return records_.size();
    }

private:
    mutable std::mutex mu_;
    std::vector<std::string> records_;
};

}  // namespace

TEST(AsyncLogger, DrainsAllRecordsBeforeShutdown) {
    auto sink = std::make_shared<CapturingSink>();
    auto logger = std::make_shared<ulog::AsyncLogger>();
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);

    ulog::SetDefaultLogger(logger);
    for (int i = 0; i < 500; ++i) {
        LOG_INFO() << "msg " << i;
    }
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);
    logger.reset();  // joins worker

    EXPECT_EQ(sink->Size(), 500u);
}

TEST(AsyncLogger, QueueDepthAndTotalLoggedMetrics) {
    auto sink = std::make_shared<CapturingSink>();
    auto logger = std::make_shared<ulog::AsyncLogger>();
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);

    EXPECT_EQ(logger->GetTotalLogged(), 0u);
    EXPECT_EQ(logger->GetQueueDepth(), 0u);

    ulog::SetDefaultLogger(logger);
    for (int i = 0; i < 500; ++i) LOG_INFO() << "m " << i;
    ulog::LogFlush();

    EXPECT_EQ(logger->GetTotalLogged(), 500u);
    EXPECT_EQ(logger->GetQueueDepth(), 0u);

    ulog::SetDefaultLogger(nullptr);
}

TEST(AsyncLogger, FlushIsSynchronous) {
    auto sink = std::make_shared<CapturingSink>();
    auto logger = std::make_shared<ulog::AsyncLogger>();
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);

    ulog::SetDefaultLogger(logger);
    for (int i = 0; i < 100; ++i) LOG_INFO() << "tick " << i;
    ulog::LogFlush();
    // After Flush() returns, every record must have hit the sink.
    EXPECT_EQ(sink->Size(), 100u);

    ulog::SetDefaultLogger(nullptr);
}

TEST(AsyncLogger, DiscardOverflowCountsDrops) {
    // Parks the worker inside its first Write() so the queue saturates
    // deterministically. The previous version relied on "worker is
    // slower than the producer" timing, which failed on the
    // linux-clang-cpp20 runner where the worker kept up with 5000
    // synchronous LOG_INFO calls and zero drops were observed.
    struct BlockingSink final : ulog::sinks::BaseSink {
        std::atomic<int> writes{0};
        std::mutex mu;
        std::condition_variable cv;
        bool released = false;
        void Write(std::string_view) override {
            {
                std::unique_lock lk(mu);
                cv.wait(lk, [this] { return released; });
            }
            writes.fetch_add(1, std::memory_order_relaxed);
        }
        void Release() {
            {
                std::lock_guard lk(mu);
                released = true;
            }
            cv.notify_all();
        }
    };

    ulog::AsyncLogger::Config cfg;
    cfg.queue_capacity = 32;
    cfg.overflow = ulog::OverflowBehavior::kDiscard;

    auto sink = std::make_shared<BlockingSink>();
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);

    ulog::SetDefaultLogger(logger);
    // Worker is parked in the first sink->Write(). Queue fills to 32,
    // the remaining ~4968 calls hit overflow and are dropped.
    for (int i = 0; i < 5000; ++i) LOG_INFO() << "x " << i;
    sink->Release();  // drain the queued records.
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const std::uint64_t delivered =
        static_cast<std::uint64_t>(sink->writes.load(std::memory_order_relaxed));
    const auto dropped = logger->GetDroppedCount();
    EXPECT_EQ(delivered + dropped, 5000u);
    EXPECT_GT(dropped, 0u);
}

TEST(AsyncLogger, SingleThreadPublishesToMultipleLoggers) {
    // Exercises the TLS producer-token cache under the "one thread, two
    // AsyncLoggers" scenario: each enqueue alternates loggers so the TLS
    // slot is invalidated and re-bound every call. Both sinks must still
    // receive every record for the logger they're attached to.
    auto sink_a = std::make_shared<CapturingSink>();
    auto sink_b = std::make_shared<CapturingSink>();
    auto logger_a = std::make_shared<ulog::AsyncLogger>();
    auto logger_b = std::make_shared<ulog::AsyncLogger>();
    logger_a->SetLevel(ulog::Level::kTrace);
    logger_b->SetLevel(ulog::Level::kTrace);
    logger_a->AddSink(sink_a);
    logger_b->AddSink(sink_b);

    constexpr int kN = 300;
    for (int i = 0; i < kN; ++i) {
        ulog::LogHelper(*logger_a, ulog::Level::kInfo, {}) << "a " << i;
        ulog::LogHelper(*logger_b, ulog::Level::kInfo, {}) << "b " << i;
    }
    logger_a->Flush();
    logger_b->Flush();

    EXPECT_EQ(sink_a->Size(), static_cast<std::size_t>(kN));
    EXPECT_EQ(sink_b->Size(), static_cast<std::size_t>(kN));

    logger_a.reset();
    logger_b.reset();
}

TEST(AsyncLogger, TlsCacheSurvivesLoggerRecycleAtSameAddress) {
    // Create a logger, publish, destroy, then create a new one at a
    // (potentially) recycled allocation. The per-State generation counter
    // must force a cache miss so the thread does not hand out a token
    // bound to the freed queue.
    for (int round = 0; round < 3; ++round) {
        auto sink = std::make_shared<CapturingSink>();
        auto logger = std::make_shared<ulog::AsyncLogger>();
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(sink);

        ulog::SetDefaultLogger(logger);
        for (int i = 0; i < 50; ++i) LOG_INFO() << "round=" << round << " i=" << i;
        ulog::LogFlush();
        ulog::SetDefaultLogger(nullptr);
        logger.reset();

        EXPECT_EQ(sink->Size(), 50u) << "round " << round;
    }
}

TEST(AsyncLogger, MultiThreadedProducers) {
    auto sink = std::make_shared<CapturingSink>();
    auto logger = std::make_shared<ulog::AsyncLogger>();
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);

    ulog::SetDefaultLogger(logger);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 200;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([t] {
            for (int i = 0; i < kPerThread; ++i) LOG_INFO() << "t=" << t << " i=" << i;
        });
    }
    for (auto& t : ts) t.join();

    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);
    logger.reset();

    EXPECT_EQ(sink->Size(), static_cast<std::size_t>(kThreads * kPerThread));
}
