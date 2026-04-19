// Multi-producer async bench. Captures the cost when many threads emit
// records concurrently — queue contention + batch notify effectiveness.

#include <atomic>
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

}  // namespace
