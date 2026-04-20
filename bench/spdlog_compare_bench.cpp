// spdlog comparison benchmarks — mirrors sync_throughput_bench.cpp,
// async_throughput_bench.cpp, and multi_producer_bench.cpp to give an
// apples-to-apples reference point against ulog.

#include <cstdint>
#include <memory>

#include <benchmark/benchmark.h>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace {

// ---------------------------------------------------------------------------
// Sync — mirrors BM_SyncLoggerThroughput
// ---------------------------------------------------------------------------

void BM_SpdlogSyncThroughput(benchmark::State& state) {
    auto sink = std::make_shared<spdlog::sinks::null_sink_st>();
    auto logger = std::make_shared<spdlog::logger>("bench_sync", sink);
    logger->set_level(spdlog::level::trace);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        logger->info("tick {}", counter);
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_SpdlogSyncThroughput);

// ---------------------------------------------------------------------------
// Async — mirrors BM_AsyncLoggerThroughput
// queue_capacity=65536, 1 worker, overflow=block
// ---------------------------------------------------------------------------

void BM_SpdlogAsyncThroughput(benchmark::State& state) {
    spdlog::init_thread_pool(65536, 1);
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "bench_async", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_level(spdlog::level::trace);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        logger->info("tick {}", counter);
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    logger->flush();
    spdlog::shutdown();
}
BENCHMARK(BM_SpdlogAsyncThroughput);

// ---------------------------------------------------------------------------
// Multi-producer async — mirrors BM_AsyncMultiProducer
// ---------------------------------------------------------------------------

std::shared_ptr<spdlog::async_logger>& SharedSpdlogLogger() {
    static std::shared_ptr<spdlog::async_logger> g;
    return g;
}

void SpdlogMPSetup(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        spdlog::init_thread_pool(65536, 1);
        auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        SharedSpdlogLogger() = std::make_shared<spdlog::async_logger>(
            "bench_mp", sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        SharedSpdlogLogger()->set_level(spdlog::level::trace);
    }
}

void SpdlogMPTeardown(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        auto& g = SharedSpdlogLogger();
        if (g) g->flush();
        g.reset();
        spdlog::shutdown();
    }
}

void BM_SpdlogAsyncMultiProducer(benchmark::State& state) {
    SpdlogMPSetup(state);
    auto& logger = SharedSpdlogLogger();
    std::uint64_t counter = 0;
    for (auto _ : state) {
        logger->info("t={} i={}", state.thread_index(), counter);
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    SpdlogMPTeardown(state);
}
BENCHMARK(BM_SpdlogAsyncMultiProducer)->Threads(1)->Threads(2)->Threads(4)->Threads(8);

}  // namespace
