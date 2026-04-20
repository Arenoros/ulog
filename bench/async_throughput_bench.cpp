// Async logger throughput benchmarks.
//
// Three distinct measurements — collapsing them into one number hides the
// real shape of the logger:
//
//   BM_AsyncEnqueueCost     producer-side cost when the queue is not
//                           backpressured. kDiscard + oversized queue so
//                           Log() never blocks. Reports `dropped` counter
//                           — if non-zero the producer outran the worker
//                           during the run (still a valid enqueue-cost
//                           measurement; the atomic counter bump is on
//                           the producer path anyway).
//
//   BM_AsyncEndToEnd        sustained pipeline throughput. kBlock +
//                           bounded queue — producer rate converges to
//                           worker drain rate once the queue saturates.
//                           Flush() in teardown so the timer does not
//                           end with unwritten records.
//
//   BM_AsyncDisabledLogCost cost of a filtered-out LOG_DEBUG when level
//                           == kInfo. Should collapse to an atomic level
//                           check + early return — a handful of ns. If
//                           it shows double-digit ns, arguments format
//                           before the level gate, which means debug
//                           lines cost production throughput.

#include <cstdint>
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

// ---------------------------------------------------------------------------
// Enqueue cost — producer never blocks.
// ---------------------------------------------------------------------------

void BM_AsyncEnqueueCost(benchmark::State& state) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    cfg.queue_capacity = 1u << 20;  // 1 Mi slots — exceeds typical iteration count
    cfg.overflow = ulog::OverflowBehavior::kDiscard;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "tick " << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.counters["dropped"] = benchmark::Counter(
        static_cast<double>(logger->GetDroppedCount()),
        benchmark::Counter::kAvgIterations);

    // Drain before destruction so the next benchmark does not inherit a
    // still-running worker. Not part of the measurement window.
    logger->Flush();
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_AsyncEnqueueCost);

// ---------------------------------------------------------------------------
// End-to-end — producer blocks once the queue fills; throughput = worker drain.
// ---------------------------------------------------------------------------

void BM_AsyncEndToEnd(benchmark::State& state) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    cfg.queue_capacity = 65536;
    cfg.overflow = ulog::OverflowBehavior::kBlock;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "tick " << counter;
        ++counter;
    }
    // Flush() inside the timed region of the last iteration is unavoidable
    // here — we time the average `Log()` call; the final flush amortises
    // across all iterations and is negligible at high iteration counts.
    logger->Flush();
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_AsyncEndToEnd);

// ---------------------------------------------------------------------------
// Disabled log cost — level filter must short-circuit before formatting.
// ---------------------------------------------------------------------------

void BM_AsyncDisabledLogCost(benchmark::State& state) {
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kInfo);  // DEBUG filtered out
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_DEBUG() << "payload=" << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    logger->Flush();
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_AsyncDisabledLogCost);

}  // namespace
