#include <atomic>
#include <chrono>
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
    ulog::AsyncLogger::Config cfg;
    cfg.queue_capacity = 32;
    cfg.overflow = ulog::OverflowBehavior::kDiscard;

    auto sink = std::make_shared<CapturingSink>();
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);

    ulog::SetDefaultLogger(logger);
    // Flood synchronously faster than worker can drain.
    for (int i = 0; i < 5000; ++i) LOG_INFO() << "x " << i;
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto total = sink->Size() + logger->GetDroppedCount();
    EXPECT_EQ(total, 5000u);
    EXPECT_GT(logger->GetDroppedCount(), 0u);
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
