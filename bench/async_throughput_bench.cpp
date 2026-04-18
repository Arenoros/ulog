// Async logger throughput: measures producer-side records/sec. Worker drain
// speed bounds the real throughput; producers see queue overhead only.
// With OverflowBehavior::kDiscard and a small queue, steady-state represents
// hot-path cost.

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

void BM_AsyncLoggerThroughput(benchmark::State& state) {
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
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));

    // Drain before teardown so we measure producer cost rather than
    // destructor cost.
    logger->Flush();
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_AsyncLoggerThroughput);

}  // namespace
