// Producer-side latency distribution. Google Benchmark's built-in timing
// reports mean + stddev, which hides the tail. For real-time use cases the
// tail (p99.9, max) is the signal — a logger whose p99.9 is 50× p50 has a
// hidden allocation, mutex, or page fault that will show up as jitter in
// production.
//
// Each sample is the wall-clock latency of one `LOG_INFO()` call. Samples
// are stored in a pre-allocated vector so the hot path makes no heap
// traffic of its own; percentiles are computed after the loop.
//
// Google Benchmark is set to `Iterations(1)`, so the outer `for (auto _ :
// state)` runs once — the inner loop (`kIters`) is the real workload. This
// keeps percentile counters stable across runs; bench runtime is bounded
// by kIters × mean latency.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include <ulog/async_logger.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace {

class DiscardSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view /*record*/) override {}
};

constexpr std::size_t kLatencyIters = 200'000;

void ReportPercentiles(benchmark::State& state, std::vector<std::uint64_t>& samples) {
    if (samples.empty()) return;
    std::sort(samples.begin(), samples.end());
    const auto pct = [&](double p) -> double {
        auto idx = static_cast<std::size_t>(p * (samples.size() - 1));
        return static_cast<double>(samples[idx]);
    };
    state.counters["p50_ns"]    = pct(0.50);
    state.counters["p90_ns"]    = pct(0.90);
    state.counters["p99_ns"]    = pct(0.99);
    state.counters["p99_9_ns"]  = pct(0.999);
    state.counters["p99_99_ns"] = pct(0.9999);
    state.counters["max_ns"]    = pct(1.0);
}

// ---------------------------------------------------------------------------
// Sync logger — baseline. No queue, every call does the full format +
// sink-write on the producer thread. Tail is dominated by timestamp
// formatting / allocator behavior.
// ---------------------------------------------------------------------------

void BM_SyncLogLatency(benchmark::State& state) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::vector<std::uint64_t> samples(kLatencyIters);
    std::uint64_t counter = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < kLatencyIters; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            LOG_INFO() << "tick " << counter;
            ++counter;
            const auto t1 = std::chrono::steady_clock::now();
            samples[i] = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }
    ReportPercentiles(state, samples);
    state.SetItemsProcessed(static_cast<std::int64_t>(kLatencyIters));

    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_SyncLogLatency)->Iterations(1)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------
// Async logger — producer cost only. kDiscard + huge queue so the producer
// never blocks. Tail comes from queue contention + occasional allocator
// work in the formatter.
// ---------------------------------------------------------------------------

void BM_AsyncLogLatencyEnqueue(benchmark::State& state) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    cfg.queue_capacity = 1u << 20;
    cfg.overflow = ulog::OverflowBehavior::kDiscard;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::vector<std::uint64_t> samples(kLatencyIters);
    std::uint64_t counter = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < kLatencyIters; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            LOG_INFO() << "tick " << counter;
            ++counter;
            const auto t1 = std::chrono::steady_clock::now();
            samples[i] = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }
    ReportPercentiles(state, samples);
    state.counters["dropped"] = static_cast<double>(logger->GetDroppedCount());
    state.SetItemsProcessed(static_cast<std::int64_t>(kLatencyIters));

    logger->Flush();
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_AsyncLogLatencyEnqueue)->Iterations(1)->Unit(benchmark::kMicrosecond);

// ---------------------------------------------------------------------------
// Async logger — end-to-end latency under backpressure. kBlock + bounded
// queue, so once the queue fills the producer's per-call latency is
// dominated by wait-for-space. Max latency shows the worst-case stall.
// ---------------------------------------------------------------------------

void BM_AsyncLogLatencyBlock(benchmark::State& state) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    cfg.queue_capacity = 65536;
    cfg.overflow = ulog::OverflowBehavior::kBlock;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::vector<std::uint64_t> samples(kLatencyIters);
    std::uint64_t counter = 0;
    for (auto _ : state) {
        for (std::size_t i = 0; i < kLatencyIters; ++i) {
            const auto t0 = std::chrono::steady_clock::now();
            LOG_INFO() << "tick " << counter;
            ++counter;
            const auto t1 = std::chrono::steady_clock::now();
            samples[i] = static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }
    ReportPercentiles(state, samples);
    state.SetItemsProcessed(static_cast<std::int64_t>(kLatencyIters));

    logger->Flush();
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_AsyncLogLatencyBlock)->Iterations(1)->Unit(benchmark::kMicrosecond);

}  // namespace
