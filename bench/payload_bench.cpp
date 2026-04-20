// Payload-shape benchmarks. A single "log one int" number tells you almost
// nothing about real-world cost: floats trigger a different fmt path,
// string literals bypass formatting entirely, large strings exercise the
// buffer-growth path. Run each shape separately so regressions in one
// code path don't hide behind averages.
//
// Every bench uses SyncLogger + DiscardSink so the numbers are pure
// formatter + streaming overhead — no queue, no worker, no I/O.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <benchmark/benchmark.h>

#include <ulog/log.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace {

class DiscardSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view /*record*/) override {}
};

struct LoggerFixture {
    std::shared_ptr<ulog::SyncLogger> logger;
    LoggerFixture() {
        logger = std::make_shared<ulog::SyncLogger>(ulog::Format::kTskv);
        logger->SetLevel(ulog::Level::kTrace);
        logger->AddSink(std::make_shared<DiscardSink>());
        ulog::SetDefaultLogger(logger);
    }
    ~LoggerFixture() { ulog::SetDefaultLogger(nullptr); }
};

// ---------------------------------------------------------------------------
// Int-only — LOG("val=%d", x). Fast path for integer formatting.
// ---------------------------------------------------------------------------

void BM_PayloadInt(benchmark::State& state) {
    LoggerFixture fx;
    std::int64_t x = 0;
    for (auto _ : state) {
        LOG_INFO() << "val=" << x;
        ++x;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_PayloadInt);

// ---------------------------------------------------------------------------
// Float — %.6f. Typically noticeably slower than int.
// ---------------------------------------------------------------------------

void BM_PayloadFloat(benchmark::State& state) {
    LoggerFixture fx;
    double x = 0.0;
    for (auto _ : state) {
        LOG_INFO() << "val=" << x;
        x += 0.125;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_PayloadFloat);

// ---------------------------------------------------------------------------
// Static string — no formatting at all. Measures the logger's fixed cost
// (timestamp, level, module, plumbing) with zero formatter work.
// ---------------------------------------------------------------------------

void BM_PayloadStaticString(benchmark::State& state) {
    LoggerFixture fx;
    for (auto _ : state) {
        LOG_INFO() << "startup complete";
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_PayloadStaticString);

// ---------------------------------------------------------------------------
// Dynamic std::string — tests whether the logger copies or takes ownership.
// Pre-generate a pool of strings so the measurement isolates the streaming
// cost of an lvalue std::string, not the allocation of its contents.
// PauseTiming/ResumeTiming is avoided on purpose — gbench's pause/resume
// has ~100-200 ns overhead that would dominate a sub-µs per-call logger.
// ---------------------------------------------------------------------------

void BM_PayloadDynString(benchmark::State& state) {
    LoggerFixture fx;
    constexpr std::size_t kPoolSize = 1024;
    std::vector<std::string> pool;
    pool.reserve(kPoolSize);
    for (std::size_t i = 0; i < kPoolSize; ++i) {
        pool.emplace_back("dyn_val=" + std::to_string(i));
    }
    std::size_t idx = 0;
    for (auto _ : state) {
        LOG_INFO() << pool[idx];
        if (++idx == kPoolSize) idx = 0;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_PayloadDynString);

// ---------------------------------------------------------------------------
// Multi-arg — realistic production-shape message with 7 mixed-type fields.
// Exercises the streaming operator chain end-to-end.
// ---------------------------------------------------------------------------

void BM_PayloadMultiArg(benchmark::State& state) {
    LoggerFixture fx;
    std::int64_t i = 0;
    for (auto _ : state) {
        LOG_INFO() << "req_id=" << i
                   << " user=alice"
                   << " status=" << 200
                   << " latency_ms=" << 12.345
                   << " retries=" << 0
                   << " cache=hit"
                   << " size=" << 4096;
        ++i;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
}
BENCHMARK(BM_PayloadMultiArg);

// ---------------------------------------------------------------------------
// Large payload — 256 / 1024 / 4096 bytes. Stresses internal buffer growth
// and the per-record truncation cap (kSizeLimit = 10 KB).
// ---------------------------------------------------------------------------

void BM_PayloadLarge(benchmark::State& state) {
    LoggerFixture fx;
    const auto size = static_cast<std::size_t>(state.range(0));
    std::string payload(size, 'x');
    for (auto _ : state) {
        LOG_INFO() << payload;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    state.SetBytesProcessed(
        static_cast<std::int64_t>(state.iterations()) * static_cast<std::int64_t>(size));
}
BENCHMARK(BM_PayloadLarge)->Arg(256)->Arg(1024)->Arg(4096);

}  // namespace
