// Measures the overhead of the default-logger snapshot path used by every
// LOG_* macro. Setup: install a NullLogger so the record is discarded at
// the sink level, isolating the resolve + format path.

#include <memory>

#include <benchmark/benchmark.h>

#include <ulog/log.hpp>
#include <ulog/null_logger.hpp>

namespace {

void BM_GetDefaultLoggerPtr(benchmark::State& state) {
    for (auto _ : state) {
        auto p = ulog::GetDefaultLoggerPtr();
        benchmark::DoNotOptimize(p);
    }
}
BENCHMARK(BM_GetDefaultLoggerPtr);

void BM_LogThroughNullDefault(benchmark::State& state) {
    ulog::SetDefaultLogger(ulog::MakeNullLogger());
    for (auto _ : state) {
        LOG_INFO() << "payload " << 42;
    }
    ulog::SetDefaultLogger(nullptr);
}
BENCHMARK(BM_LogThroughNullDefault);

}  // namespace
