// Slow-sink comparison — simulates I/O latency to show where Async wins
// over Sync. SlowSink busy-waits for a fixed duration on every Write; the
// producer pays that cost synchronously in Sync mode but is shielded from
// it by the worker thread in Async mode.
//
// Busy-wait (rather than std::this_thread::sleep_for) is used because
// Windows' sleep has ~15 ms scheduling granularity — sleep_for(1us) would
// actually yield 15 ms and make all the sub-quantum rows indistinguishable.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string_view>

#include <benchmark/benchmark.h>

#include <ulog/async_logger.hpp>
#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace {

class SlowSink final : public ulog::sinks::BaseSink {
public:
    explicit SlowSink(std::chrono::microseconds delay) : delay_(delay) {}
    void Write(std::string_view /*record*/) override {
        if (delay_.count() <= 0) return;
        const auto end = std::chrono::high_resolution_clock::now() + delay_;
        while (std::chrono::high_resolution_clock::now() < end) {}
    }
private:
    std::chrono::microseconds delay_;
};

void BM_SyncSlowSink(benchmark::State& state) {
    const auto delay = std::chrono::microseconds(state.range(0));
    auto logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<SlowSink>(delay));
    ulog::SetDefaultLogger(logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "tick " << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_SyncSlowSink)->Arg(0)->Arg(1)->Arg(10)->Arg(50)->Iterations(2000);

void BM_AsyncSlowSink(benchmark::State& state) {
    const auto delay = std::chrono::microseconds(state.range(0));
    ulog::AsyncLogger::Config cfg;
    cfg.format = ulog::Format::kTskv;
    cfg.queue_capacity = 65536;
    cfg.overflow = ulog::OverflowBehavior::kBlock;
    auto logger = std::make_shared<ulog::AsyncLogger>(cfg);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<SlowSink>(delay));
    ulog::SetDefaultLogger(logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "tick " << counter;
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    // Don't Flush — the worker can be many seconds behind at 50 us/record;
    // the producer cost is what we wanted. Dropping the logger drains
    // on destruction via the State worker-stop path.
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_AsyncSlowSink)->Arg(0)->Arg(1)->Arg(10)->Arg(50)->Iterations(2000);

}  // namespace
