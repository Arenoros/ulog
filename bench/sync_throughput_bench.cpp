// Sync logger throughput: measures records/sec through SyncLogger with a
// capturing-but-discarding sink. Isolates formatter + logger overhead.

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

}  // namespace
