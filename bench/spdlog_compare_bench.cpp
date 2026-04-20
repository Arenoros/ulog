// spdlog comparison benchmarks — mirror shape of the ulog benches so the
// numbers can be read side by side.
//
// spdlog's `async_overflow_policy`:
//   block           — producer waits for space (default)
//   overrun_oldest  — producer wins, oldest queued item is dropped
//   discard_new     — producer wins, incoming item is dropped
//
// For "pure enqueue cost" we want the policy that never blocks. `overrun_oldest`
// is preferred over `discard_new` because the latter introduces a pending
// counter bump on the producer path; `overrun_oldest` keeps the push cheap
// and lets the worker see records in order (as many as it can keep up with).
//
// spdlog does not expose a dropped-count metric on `async_logger` the way
// ulog does, so the enqueue benches cannot report `dropped` — the producer
// hot-path timing is what the number reflects either way.

#include <cstdint>
#include <memory>

#include <benchmark/benchmark.h>
#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/null_sink.h>
#include <spdlog/spdlog.h>

namespace {

constexpr std::size_t kLargeQueue = 1u << 20;   // 1 Mi slots
constexpr std::size_t kBoundedQueue = 65536;

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

void BM_SpdlogSyncDisabledLogCost(benchmark::State& state) {
    auto sink = std::make_shared<spdlog::sinks::null_sink_st>();
    auto logger = std::make_shared<spdlog::logger>("bench_sync_off", sink);
    logger->set_level(spdlog::level::info);  // debug filtered

    std::uint64_t counter = 0;
    for (auto _ : state) {
        logger->debug("tick {}", counter);
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_SpdlogSyncDisabledLogCost);

// ---------------------------------------------------------------------------
// Async — enqueue cost
// ---------------------------------------------------------------------------

void BM_SpdlogAsyncEnqueue(benchmark::State& state) {
    spdlog::init_thread_pool(kLargeQueue, 1);
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "bench_async_enq", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
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
BENCHMARK(BM_SpdlogAsyncEnqueue);

// ---------------------------------------------------------------------------
// Async — end-to-end (block policy, bounded queue)
// ---------------------------------------------------------------------------

void BM_SpdlogAsyncEndToEnd(benchmark::State& state) {
    spdlog::init_thread_pool(kBoundedQueue, 1);
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto logger = std::make_shared<spdlog::async_logger>(
        "bench_async_e2e", sink, spdlog::thread_pool(),
        spdlog::async_overflow_policy::block);
    logger->set_level(spdlog::level::trace);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        logger->info("tick {}", counter);
        ++counter;
    }
    logger->flush();
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    spdlog::shutdown();
}
BENCHMARK(BM_SpdlogAsyncEndToEnd);

// ---------------------------------------------------------------------------
// Multi-producer async — enqueue + end-to-end
// ---------------------------------------------------------------------------

std::shared_ptr<spdlog::async_logger>& SharedSpdlogLogger() {
    static std::shared_ptr<spdlog::async_logger> g;
    return g;
}

void SetupSpdlogMPEnqueue(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        spdlog::init_thread_pool(kLargeQueue, 1);
        auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        SharedSpdlogLogger() = std::make_shared<spdlog::async_logger>(
            "bench_mp_enq", sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::overrun_oldest);
        SharedSpdlogLogger()->set_level(spdlog::level::trace);
    }
}

void SetupSpdlogMPEndToEnd(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        spdlog::init_thread_pool(kBoundedQueue, 1);
        auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
        SharedSpdlogLogger() = std::make_shared<spdlog::async_logger>(
            "bench_mp_e2e", sink, spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
        SharedSpdlogLogger()->set_level(spdlog::level::trace);
    }
}

void TeardownSpdlogMP(const benchmark::State& state) {
    if (state.thread_index() == 0) {
        auto& g = SharedSpdlogLogger();
        if (g) g->flush();
        g.reset();
        spdlog::shutdown();
    }
}

void BM_SpdlogAsyncMultiProducerEnqueue(benchmark::State& state) {
    SetupSpdlogMPEnqueue(state);
    auto& logger = SharedSpdlogLogger();
    std::uint64_t counter = 0;
    for (auto _ : state) {
        logger->info("t={} i={}", state.thread_index(), counter);
        ++counter;
    }
    // items_per_second below is aggregate across threads; per_thread_rate
    // exposes the single-producer perspective for contention analysis.
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.counters["per_thread_rate"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate | benchmark::Counter::kAvgThreads);
    TeardownSpdlogMP(state);
}
BENCHMARK(BM_SpdlogAsyncMultiProducerEnqueue)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16);

void BM_SpdlogAsyncMultiProducerEndToEnd(benchmark::State& state) {
    SetupSpdlogMPEndToEnd(state);
    auto& logger = SharedSpdlogLogger();
    std::uint64_t counter = 0;
    for (auto _ : state) {
        logger->info("t={} i={}", state.thread_index(), counter);
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.counters["per_thread_rate"] = benchmark::Counter(
        static_cast<double>(state.iterations()),
        benchmark::Counter::kIsRate | benchmark::Counter::kAvgThreads);
    TeardownSpdlogMP(state);
}
BENCHMARK(BM_SpdlogAsyncMultiProducerEndToEnd)
    ->Threads(1)->Threads(2)->Threads(4)->Threads(8)->Threads(16);

}  // namespace
