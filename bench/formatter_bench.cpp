// Formatter A/B — drives the same LOG_* pattern through TSKV, LTSV, RAW, and
// JSON formatters to expose per-format overhead. All use a DiscardSink so
// only formatter + logger path is measured.

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

template <ulog::Format F>
void BM_FormatThroughput(benchmark::State& state) {
    auto logger = std::make_shared<ulog::SyncLogger>(F);
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(std::make_shared<DiscardSink>());
    ulog::impl::SetDefaultLoggerRef(*logger);

    std::uint64_t counter = 0;
    for (auto _ : state) {
        LOG_INFO() << "tick " << counter
                   << ulog::LogExtra{{"k1", 7}, {"k2", std::string("value")}};
        ++counter;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()));
    ulog::SetNullDefaultLogger();
}

BENCHMARK_TEMPLATE(BM_FormatThroughput, ulog::Format::kTskv);
BENCHMARK_TEMPLATE(BM_FormatThroughput, ulog::Format::kLtsv);
BENCHMARK_TEMPLATE(BM_FormatThroughput, ulog::Format::kRaw);
BENCHMARK_TEMPLATE(BM_FormatThroughput, ulog::Format::kJson);

}  // namespace
