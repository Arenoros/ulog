// Sync logger throughput benchmarks.
//
//   BM_SyncLoggerThroughput  records/sec through SyncLogger with a
//                            discarding sink. Isolates formatter + logger
//                            overhead (no queue, no worker).
//
//   BM_SyncDisabledLogCost   cost of a filtered-out LOG_DEBUG when level
//                            == kInfo. Mirrors the async variant so the
//                            two paths can be compared on the same axis.

#include <cstdint>
#include <memory>
#include <string_view>

#include <benchmark/benchmark.h>

#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace {

class DiscardSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view /*record*/) override {}
};

void BM_SyncLoggerThroughput(benchmark::State& state) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "tick " << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_SyncLoggerThroughput);

void BM_SyncDisabledLogCost(benchmark::State& state) {
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kInfo);  // DEBUG filtered out
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::SetDefaultLogger(logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_DEBUG() << "payload=" << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_SyncDisabledLogCost);

}  // namespace
