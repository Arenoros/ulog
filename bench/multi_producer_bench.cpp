// Multi-producer async bench. Captures the cost when many threads emit
// records concurrently — queue contention + batch notify effectiveness.

#include <atomic>
#include <chrono>
#include <memory>
#include <string_view>

#include <benchmark/benchmark.h>

#include <ulog/async_logger.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace {

class DiscardSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view /*record*/) override {}
};

std::shared_ptr<ulog::AsyncLogger>& SharedLogger() {
    static std::shared_ptr<ulog::AsyncLogger> g;
    return g;
}

void Setup(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        ulog::AsyncLogger::Config cfg;
        cfg.format = ulog::Format::kTskv;
        cfg.queue_capacity = 65536;
        cfg.overflow = ulog::OverflowBehavior::kBlock;
        // Worker count follows producer count so consumer-side
        // drainage doesn't become the bottleneck. DiscardSink is
        // trivially thread-safe (no-op Write).
        cfg.worker_count = static_cast<std::size_t>(state.threads());
        auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(std::make_shared<DiscardSink>());
        SharedLogger() = logger;
        ulog::SetDefaultLogger(logger);
    }
}

void Teardown(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        auto& g = SharedLogger();
        if (g) g->Flush();
        ulog::SetDefaultLogger(nullptr);
        g.reset();
    }
}

void BM_AsyncMultiProducer(benchmark::State& state) {
    Setup(state);
    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "t=" << state.thread_index() << " i=" << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    Teardown(state);
}
BENCHMARK(BM_AsyncMultiProducer)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

// -------------------------------------------------------------------------
// Worker-count scaling with a sink that pays a fixed microsecond cost per
// Write — representative of any I/O-bound sink. With a single worker the
// drain rate is capped at 1M rec/s ÷ sink_delay; adding workers should
// parallelise the sink fan-out and lift the ceiling.
//
// Busy-wait rather than `sleep_for` — Windows' scheduler quantum is
// ~15 ms, so `sleep_for(5us)` would collapse every column of the table
// to the same value. See `slow_sink_bench.cpp` for the same approach.

class SlowDelaySink final : public ulog::sinks::BaseSink {
public:
    explicit SlowDelaySink(std::chrono::microseconds delay) : delay_(delay) {}
    void Write(std::string_view /*record*/) override {
        if (delay_.count() <= 0) return;
        const auto end = std::chrono::high_resolution_clock::now() + delay_;
        while (std::chrono::high_resolution_clock::now() < end) {}
    }
private:
    std::chrono::microseconds delay_;
};

std::shared_ptr<ulog::AsyncLogger>& SharedSlowLogger() {
    static std::shared_ptr<ulog::AsyncLogger> g;
    return g;
}

void SetupSlow(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        ulog::AsyncLogger::Config cfg;
        cfg.format = ulog::Format::kTskv;
        cfg.queue_capacity = 65536;
        cfg.overflow = ulog::OverflowBehavior::kBlock;
        cfg.worker_count = static_cast<std::size_t>(state.range(0));
        auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
        logger->SetLevel(ulog::Level::kTrace);
        // 5 us per Write — well above memory-allocation cost so the
        // sink itself becomes the bottleneck.
        logger->AddSink(std::make_shared<SlowDelaySink>(std::chrono::microseconds(5)));
        SharedSlowLogger() = logger;
        ulog::SetDefaultLogger(logger);
    }
}

void TeardownSlow(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        auto& g = SharedSlowLogger();
        if (g) g->Flush();
        ulog::SetDefaultLogger(nullptr);
        g.reset();
    }
}

// 8 producer threads share the logger; worker_count varies on Arg(...).
// Ideal: items/sec scales linearly with worker_count until either the
// producer allocator saturates or worker_count exceeds available cores.
void BM_AsyncSlowSinkByWorkers(benchmark::State& state) {
    SetupSlow(state);
    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "t=" << state.thread_index() << " i=" << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    TeardownSlow(state);
}
BENCHMARK(BM_AsyncSlowSinkByWorkers)
    ->Threads(8)
    ->Arg(1)->Arg(2)->Arg(4)->Arg(8);

}  // namespace
