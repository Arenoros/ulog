// Multi-producer and backpressure async benchmarks.
//
//   BM_AsyncMultiProducerEnqueue   producer-side cost scaling. kDiscard +
//                                  oversized queue so producers never
//                                  block — measures queue-push contention
//                                  alone. A well-behaved lock-free queue
//                                  scales near-linearly with thread count
//                                  until the allocator or fmt hot path
//                                  saturates. Reports `dropped` for the
//                                  run so contention is visible.
//
//   BM_AsyncMultiProducerEndToEnd  sustained throughput with kBlock. Worker
//                                  drain rate caps total throughput —
//                                  per-thread items/sec drops as producers
//                                  contend for the single consumer.
//
//   BM_AsyncBackpressureBlock      producer rate under deliberate worker
//                                  slowdown. kBlock + slow sink — producer
//                                  latency inflates to match the sink. Tail
//                                  latency lives in the latency bench;
//                                  this one just shows the mean stall.
//
//   BM_AsyncBackpressureDiscard    same slow sink but kDiscard — producer
//                                  stays fast, `dropped` counter climbs.
//                                  Fixes the "what does overflow look like
//                                  from the producer's perspective" blind
//                                  spot of the block variant.
//
//   BM_AsyncSlowSinkByWorkers      worker-count scaling with a fixed-cost
//                                  sink. Ideal: items/sec scales linearly
//                                  with worker_count until producers or
//                                  queue become the bottleneck.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <pthread.h>
#  include <sched.h>
#endif

#include <benchmark/benchmark.h>

#include <ulog/async_logger.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace {

class DiscardSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view /*record*/) override {}
};

/// Pin the calling thread to a single logical core. No-op on unsupported
/// platforms (macOS) — the bench is still correct, just without the
/// measurement-stabilisation affinity gives on Windows/Linux.
void PinThreadToCore(unsigned core_index) noexcept {
    const auto ncores = std::thread::hardware_concurrency();
    if (ncores == 0) return;
    const auto core = core_index % ncores;
#if defined(_WIN32)
    ::SetThreadAffinityMask(::GetCurrentThread(),
                             static_cast<DWORD_PTR>(1ull) << core);
#elif defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    ::pthread_setaffinity_np(::pthread_self(), sizeof(set), &set);
#else
    (void)core;
#endif
}

// Spin-wait sink — burns `delay` on every Write. `sleep_for` is unusable on
// Windows (15 ms quantum) and on Linux the context-switch mass would swamp
// sub-µs measurements.
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

// ---------------------------------------------------------------------------
// Shared-logger fixtures. Google Benchmark's Threads() fixtures run the
// same function across N threads; `state.thread_index() == 0` owns
// setup/teardown so the logger is constructed exactly once per run.
// ---------------------------------------------------------------------------

std::shared_ptr<ulog::AsyncLogger>& SharedLogger() {
    static std::shared_ptr<ulog::AsyncLogger> g;
    return g;
}

// ---------------------------------------------------------------------------
// Enqueue cost — scaling
// ---------------------------------------------------------------------------

void SetupEnqueue(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        ulog::AsyncLogger::Config cfg;
        cfg.format = ulog::Format::kTskv;
        cfg.queue_capacity = 1u << 20;
        cfg.overflow = ulog::OverflowBehavior::kDiscard;
        auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(std::make_shared<DiscardSink>());
        SharedLogger() = logger;
        ulog::SetDefaultLogger(logger);
    }
}

void TeardownEnqueue(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        auto& g = SharedLogger();
        if (g) g->Flush();
        ulog::SetDefaultLogger(nullptr);
        SharedLogger().reset();
    }
}

void BM_AsyncMultiProducerEnqueue(benchmark::State& state) {
    SetupEnqueue(state);
    PinThreadToCore(static_cast<unsigned>(state.thread_index()));
    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "t=" << state.thread_index() << " i=" << counter;
        ++counter;
    }
    // SetItemsProcessed reports `items_per_second` as aggregate across
    // threads — that is the *total system throughput*. To read per-thread
    // cost directly, divide by state.threads(); the explicit counters
    // below surface both so the user does not have to.
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.counters["per_thread_rate"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate | benchmark::Counter::kAvgThreads);
    if (state.thread_index() == 0) {
        state.counters["dropped"] = benchmark::Counter(
            static_cast<double>(SharedLogger()->GetDroppedCount()),
            benchmark::Counter::kAvgIterations);
    }
    TeardownEnqueue(state);
}
BENCHMARK(BM_AsyncMultiProducerEnqueue)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16);

// ---------------------------------------------------------------------------
// End-to-end — scaling
// ---------------------------------------------------------------------------

void SetupEndToEnd(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        ulog::AsyncLogger::Config cfg;
        cfg.format = ulog::Format::kTskv;
        cfg.queue_capacity = 65536;
        cfg.overflow = ulog::OverflowBehavior::kBlock;
        auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(std::make_shared<DiscardSink>());
        SharedLogger() = logger;
        ulog::SetDefaultLogger(logger);
    }
}

void TeardownEndToEnd(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        auto& g = SharedLogger();
        if (g) g->Flush();
        ulog::SetDefaultLogger(nullptr);
        SharedLogger().reset();
    }
}

void BM_AsyncMultiProducerEndToEnd(benchmark::State& state) {
    SetupEndToEnd(state);
    PinThreadToCore(static_cast<unsigned>(state.thread_index()));
    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "t=" << state.thread_index() << " i=" << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.counters["per_thread_rate"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate | benchmark::Counter::kAvgThreads);
    TeardownEndToEnd(state);
}
BENCHMARK(BM_AsyncMultiProducerEndToEnd)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16);

// ---------------------------------------------------------------------------
// Backpressure — block vs discard with a deliberately slow sink
// ---------------------------------------------------------------------------

std::shared_ptr<ulog::AsyncLogger>& SharedBackpressureLogger() {
    static std::shared_ptr<ulog::AsyncLogger> g;
    return g;
}

void SetupBackpressure(const benchmark::State& state, ulog::OverflowBehavior ov) {
    if (state.thread_index() == 0) {
        ulog::AsyncLogger::Config cfg;
        cfg.format = ulog::Format::kTskv;
        cfg.queue_capacity = 1024;  // small — trigger overflow quickly
        cfg.overflow = ov;
        auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
        logger->SetLevel(ulog::Level::kTrace);
        // 10 us per Write — ~100k rec/s ceiling on the consumer. Producer
        // will run many times faster, so backpressure is guaranteed.
        logger->AddSink(std::make_shared<SlowDelaySink>(std::chrono::microseconds(10)));
        SharedBackpressureLogger() = logger;
        ulog::SetDefaultLogger(logger);
    }
}

void TeardownBackpressure(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        auto& g = SharedBackpressureLogger();
        if (g) g->Flush();
        ulog::SetDefaultLogger(nullptr);
        SharedBackpressureLogger().reset();
    }
}

void BM_AsyncBackpressureBlock(benchmark::State& state) {
    SetupBackpressure(state, ulog::OverflowBehavior::kBlock);
    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "t=" << state.thread_index() << " i=" << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    TeardownBackpressure(state);
}
BENCHMARK(BM_AsyncBackpressureBlock)->Threads(1)->Threads(4);

void BM_AsyncBackpressureDiscard(benchmark::State& state) {
    SetupBackpressure(state, ulog::OverflowBehavior::kDiscard);
    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "t=" << state.thread_index() << " i=" << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    if (state.thread_index() == 0) {
        state.counters["dropped"] = benchmark::Counter(
            static_cast<double>(SharedBackpressureLogger()->GetDroppedCount()),
            benchmark::Counter::kAvgIterations);
    }
    TeardownBackpressure(state);
}
BENCHMARK(BM_AsyncBackpressureDiscard)->Threads(1)->Threads(4);

// ---------------------------------------------------------------------------
// Worker-count scaling with a fixed-cost sink
// ---------------------------------------------------------------------------

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
        SharedSlowLogger().reset();
    }
}

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
