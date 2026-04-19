#pragma once

/// @file ulog/sinks/instrumented_sink.hpp
/// @brief Decorator that records write counts, errors, and a per-call
/// latency histogram around any existing sink.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

#include <ulog/sinks/base_sink.hpp>
#include <ulog/sinks/sink_stats.hpp>

namespace ulog::sinks {

/// Wraps any `BaseSink` and records per-call telemetry. Overhead on the
/// write hot path is two `steady_clock::now()` calls plus a handful of
/// relaxed atomic fetch_adds — roughly 40-60 ns per record on modern
/// hardware. If that overhead matters for a particular sink, wrap only
/// the ones that need observability.
///
/// Exception-safety: on a throw from the inner sink, the error counter
/// is bumped and the exception is rethrown to preserve existing caller
/// behavior (AsyncLogger swallows, SyncLogger propagates). Timing is
/// recorded regardless so a misbehaving sink's slow failures still show
/// up in the histogram.
class InstrumentedSink final : public BaseSink {
public:
    explicit InstrumentedSink(SinkPtr inner) noexcept : inner_(std::move(inner)) {}

    void Write(std::string_view record) override;
    void Flush() override;
    void Reopen(ReopenMode mode) override;
    SinkStats GetStats() const noexcept override;

    /// Accessor for the wrapped sink — useful when callers need to call
    /// a sink-specific method (e.g. `TcpSocketSink::IsOpen`) without
    /// giving up the instrumentation.
    const SinkPtr& Inner() const noexcept { return inner_; }

private:
    SinkPtr inner_;

    std::atomic<std::uint64_t> writes_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> total_write_ns_{0};
    std::atomic<std::uint64_t> max_write_ns_{0};
    std::array<std::atomic<std::uint64_t>, SinkStats::kLatencyBuckets> hist_{};
};

/// Convenience factory — wraps `inner` and returns the decorator via
/// `shared_ptr<InstrumentedSink>` so the caller keeps a typed handle
/// for `GetStats()` without upcasting back through `SinkPtr`.
inline std::shared_ptr<InstrumentedSink> MakeInstrumented(SinkPtr inner) {
    return std::make_shared<InstrumentedSink>(std::move(inner));
}

}  // namespace ulog::sinks
