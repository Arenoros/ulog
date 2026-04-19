#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sinks/instrumented_sink.hpp>
#include <ulog/sinks/null_sink.hpp>
#include <ulog/sinks/sink_stats.hpp>
#include <ulog/sync_logger.hpp>

namespace {

class CountingSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view record) override {
        std::lock_guard lock(mu_);
        records_.emplace_back(record);
    }
    std::size_t Size() const {
        std::lock_guard lock(mu_);
        return records_.size();
    }
private:
    mutable std::mutex mu_;
    std::vector<std::string> records_;
};

class ThrowingSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view) override { throw std::runtime_error("boom"); }
};

class SlowSink final : public ulog::sinks::BaseSink {
public:
    explicit SlowSink(std::chrono::nanoseconds delay) : delay_(delay) {}
    void Write(std::string_view) override { std::this_thread::sleep_for(delay_); }
private:
    std::chrono::nanoseconds delay_;
};

}  // namespace

TEST(InstrumentedSink, DefaultStatsAreZero) {
    // Non-instrumented sink returns a zero-valued SinkStats snapshot.
    auto raw = std::make_shared<CountingSink>();
    const auto stats = raw->GetStats();
    EXPECT_EQ(stats.writes, 0u);
    EXPECT_EQ(stats.errors, 0u);
    EXPECT_EQ(stats.total_write_ns, 0u);
    EXPECT_EQ(stats.max_write_ns, 0u);
    for (auto c : stats.latency_hist) EXPECT_EQ(c, 0u);
}

TEST(InstrumentedSink, CountsSuccessfulWrites) {
    auto inner = std::make_shared<CountingSink>();
    auto wrap = ulog::sinks::MakeInstrumented(inner);
    wrap->Write("a");
    wrap->Write("bb");
    wrap->Write("ccc");
    const auto s = wrap->GetStats();
    EXPECT_EQ(s.writes, 3u);
    EXPECT_EQ(s.errors, 0u);
    EXPECT_EQ(inner->Size(), 3u);
}

TEST(InstrumentedSink, CountsErrorsAndRethrows) {
    auto wrap = ulog::sinks::MakeInstrumented(std::make_shared<ThrowingSink>());
    // Exception must propagate so SyncLogger's callers can observe it.
    EXPECT_THROW(wrap->Write("x"), std::runtime_error);
    EXPECT_THROW(wrap->Write("y"), std::runtime_error);
    const auto s = wrap->GetStats();
    EXPECT_EQ(s.writes, 0u);
    EXPECT_EQ(s.errors, 2u);
    // Latency is still recorded — two samples, one in some bucket.
    std::uint64_t total = 0;
    for (auto c : s.latency_hist) total += c;
    EXPECT_EQ(total, 2u);
}

TEST(InstrumentedSink, AccumulatesLatencyHistogram) {
    // SlowSink sleeps for a known minimum; after many writes the bucket
    // spanning that duration must carry at least one sample.
    using namespace std::chrono;
    auto wrap = ulog::sinks::MakeInstrumented(
        std::make_shared<SlowSink>(milliseconds(2)));
    constexpr int kN = 5;
    for (int i = 0; i < kN; ++i) wrap->Write("x");
    const auto s = wrap->GetStats();
    EXPECT_EQ(s.writes, static_cast<std::uint64_t>(kN));
    // max_write_ns must at least be the 2ms we asked for (2'000'000 ns).
    EXPECT_GE(s.max_write_ns, 2'000'000u);
    // total_write_ns >= kN * 2ms.
    EXPECT_GE(s.total_write_ns,
              static_cast<std::uint64_t>(kN) * 2'000'000u);
    // Percentile helpers are wired up.
    const auto p99 = s.PercentileNs(0.99);
    EXPECT_GT(p99, 0u);
    const auto mean = s.MeanWriteNs();
    EXPECT_GT(mean, 0.0);
}

TEST(InstrumentedSink, ForwardsFlushAndReopen) {
    // Decorator must transparently forward control-plane calls. A sink
    // that counts calls via side-channel captures the forwarding.
    struct TrackingSink final : ulog::sinks::BaseSink {
        int flushes = 0;
        int reopens = 0;
        void Write(std::string_view) override {}
        void Flush() override { ++flushes; }
        void Reopen(ulog::sinks::ReopenMode) override { ++reopens; }
    };
    auto inner = std::make_shared<TrackingSink>();
    auto wrap = ulog::sinks::MakeInstrumented(inner);
    wrap->Flush();
    wrap->Reopen(ulog::sinks::ReopenMode::kAppend);
    EXPECT_EQ(inner->flushes, 1);
    EXPECT_EQ(inner->reopens, 1);
}

TEST(InstrumentedSink, ConcurrentWritesAccumulateDeterministically) {
    // The counters must survive multi-producer contention — this catches
    // regressions on the atomic ordering + the max-bumping CAS loop.
    auto wrap = ulog::sinks::MakeInstrumented(std::make_shared<CountingSink>());
    constexpr int kThreads = 4;
    constexpr int kPer = 500;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&] {
            for (int i = 0; i < kPer; ++i) wrap->Write("rec");
        });
    }
    for (auto& t : ts) t.join();
    const auto s = wrap->GetStats();
    EXPECT_EQ(s.writes, static_cast<std::uint64_t>(kThreads * kPer));
    EXPECT_EQ(s.errors, 0u);
}

TEST(InstrumentedSink, InnerReturnsOriginalSink) {
    auto raw = std::make_shared<CountingSink>();
    auto wrap = ulog::sinks::MakeInstrumented(raw);
    EXPECT_EQ(wrap->Inner().get(), raw.get());
}

TEST(InstrumentedSink, ComposesWithSyncLogger) {
    // End-to-end path: SyncLogger -> InstrumentedSink -> concrete sink.
    // Asserts the stats match the number of records that actually made
    // it through the level filter.
    auto counting = std::make_shared<CountingSink>();
    auto instrumented = ulog::sinks::MakeInstrumented(counting);
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kInfo);
    logger->AddSink(instrumented);

    ulog::SetDefaultLogger(logger);
    LOG_DEBUG() << "dropped";   // below logger level -> filtered, no sink call
    LOG_INFO()  << "a";
    LOG_ERROR() << "b";
    ulog::LogFlush();
    ulog::SetDefaultLogger(nullptr);

    const auto s = instrumented->GetStats();
    EXPECT_EQ(s.writes, 2u);
    EXPECT_EQ(s.errors, 0u);
    EXPECT_EQ(counting->Size(), 2u);
}

TEST(SinkStats, PercentileZeroForEmptyHistogram) {
    ulog::sinks::SinkStats s;
    EXPECT_EQ(s.PercentileNs(0.5), 0u);
    EXPECT_EQ(s.PercentileNs(0.99), 0u);
    EXPECT_EQ(s.MeanWriteNs(), 0.0);
}

TEST(SinkStats, PercentileMonotonicInP) {
    // Synthesize a histogram so we can assert the helper returns
    // non-decreasing bucket caps as p grows.
    ulog::sinks::SinkStats s;
    s.latency_hist[4] = 10;   // bucket [16, 32) ns
    s.latency_hist[10] = 5;   // bucket [1024, 2048) ns
    s.latency_hist[20] = 1;   // bucket [1M, 2M) ns
    const auto p50 = s.PercentileNs(0.5);
    const auto p90 = s.PercentileNs(0.9);
    const auto p99 = s.PercentileNs(0.99);
    EXPECT_LE(p50, p90);
    EXPECT_LE(p90, p99);
    EXPECT_GT(p99, 0u);
}
