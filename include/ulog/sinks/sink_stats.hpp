#pragma once

/// @file ulog/sinks/sink_stats.hpp
/// @brief Per-sink observability counters + latency histogram.
///
/// Snapshot-shaped: `GetStats()` returns a copy — the caller is free to
/// diff against a prior snapshot to compute per-interval rates. The
/// latency histogram uses power-of-two ns buckets covering roughly
/// 1 ns … 4 s, which is enough resolution for ops-scale tracking
/// ("is the TCP sink pausing for hundreds of milliseconds?") without
/// paying the cost of a true HDR/t-digest histogram on the hot path.

#include <cstddef>
#include <cstdint>

namespace ulog::sinks {

struct SinkStats {
    /// Successful Write() calls since the sink was constructed.
    std::uint64_t writes{0};
    /// Write() calls that threw an exception. Added to by the decorator
    /// before the exception is rethrown to the caller.
    std::uint64_t errors{0};
    /// Sum of Write() durations (nanoseconds), successful and failed.
    std::uint64_t total_write_ns{0};
    /// Highest single Write() duration observed (nanoseconds).
    std::uint64_t max_write_ns{0};

    /// Log-base-2 histogram of per-call nanosecond durations. Bucket `i`
    /// covers `[2^i, 2^(i+1))` ns. 32 buckets covers ≈ 1 ns … 4 s, with
    /// the last bucket capped as saturating ("≥ 2^31 ns").
    static constexpr std::size_t kLatencyBuckets = 32;
    std::uint64_t latency_hist[kLatencyBuckets]{};

    /// Mean write duration (ns). 0 when no writes have been recorded.
    double MeanWriteNs() const noexcept {
        const auto total = writes + errors;
        return total == 0 ? 0.0
                          : static_cast<double>(total_write_ns) / static_cast<double>(total);
    }

    /// Returns the upper bound (ns) of the bucket that contains the
    /// requested latency percentile. Coarse by design — reports the
    /// power-of-two cap, not an interpolated value. Returns 0 if no
    /// samples have been recorded. `p` is clamped to [0, 1].
    std::uint64_t PercentileNs(double p) const noexcept {
        std::uint64_t total = 0;
        for (auto c : latency_hist) total += c;
        if (total == 0) return 0;
        if (p < 0.0) p = 0.0;
        if (p > 1.0) p = 1.0;
        const auto target =
            static_cast<std::uint64_t>(static_cast<double>(total) * p + 0.5);
        std::uint64_t cum = 0;
        for (std::size_t i = 0; i < kLatencyBuckets; ++i) {
            cum += latency_hist[i];
            if (cum >= target) {
                // bucket i covers [2^i, 2^(i+1)) — return the inclusive
                // upper bound so callers can say "p99 ≤ X ns".
                if (i + 1 >= 64) return UINT64_MAX;
                return (std::uint64_t{1} << (i + 1)) - 1;
            }
        }
        return 0;
    }
};

}  // namespace ulog::sinks
