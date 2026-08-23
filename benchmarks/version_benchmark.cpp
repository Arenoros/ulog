#include <benchmark/benchmark.h>

#include <ulog/version.hpp>

namespace {

void VersionQuery(benchmark::State& state) {
  for ([[maybe_unused]] const auto iteration : state) {
    ulog::Version version = ulog::GetVersion();
    benchmark::DoNotOptimize(version);
  }
}

BENCHMARK(VersionQuery);

}  // namespace

BENCHMARK_MAIN();
