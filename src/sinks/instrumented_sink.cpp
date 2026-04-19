#include <ulog/sinks/instrumented_sink.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

namespace ulog::sinks {

namespace {

/// floor(log2(v)) for v > 0; returns 0 for v = 0. Maps a nanosecond
/// duration to its histogram bucket index. Uses compiler intrinsics when
/// available so the hot path stays single-digit ns.
std::size_t BucketOf(std::uint64_t ns) noexcept {
    if (ns == 0) return 0;
#if defined(__GNUC__) || defined(__clang__)
    const int idx = 63 - __builtin_clzll(ns);
#elif defined(_MSC_VER)
    unsigned long idx_ul = 0;
    _BitScanReverse64(&idx_ul, ns);
    const int idx = static_cast<int>(idx_ul);
#else
    int idx = 0;
    std::uint64_t v = ns;
    while (v >>= 1) ++idx;
#endif
    const auto cap = static_cast<int>(SinkStats::kLatencyBuckets) - 1;
    return static_cast<std::size_t>(idx < cap ? idx : cap);
}

/// Monotonic-max helper: lifts `current_max` until it reaches `sample`.
/// Used so `max_write_ns_` races correctly across many producer threads.
void BumpMax(std::atomic<std::uint64_t>& slot, std::uint64_t sample) noexcept {
    auto prev = slot.load(std::memory_order_relaxed);
    while (sample > prev &&
           !slot.compare_exchange_weak(prev, sample,
                                       std::memory_order_relaxed,
                                       std::memory_order_relaxed)) {
        // `prev` updated by the failed CAS; loop re-checks.
    }
}

}  // namespace

void InstrumentedSink::Write(std::string_view record) {
    const auto t0 = std::chrono::steady_clock::now();
    std::exception_ptr pending;
    try {
        if (inner_) inner_->Write(record);
    } catch (...) {
        pending = std::current_exception();
        errors_.fetch_add(1, std::memory_order_relaxed);
    }
    const auto t1 = std::chrono::steady_clock::now();
    const auto ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    if (!pending) writes_.fetch_add(1, std::memory_order_relaxed);
    total_write_ns_.fetch_add(ns, std::memory_order_relaxed);
    BumpMax(max_write_ns_, ns);
    hist_[BucketOf(ns)].fetch_add(1, std::memory_order_relaxed);
    if (pending) std::rethrow_exception(pending);
}

void InstrumentedSink::Flush() {
    if (inner_) inner_->Flush();
}

void InstrumentedSink::Reopen(ReopenMode mode) {
    if (inner_) inner_->Reopen(mode);
}

SinkStats InstrumentedSink::GetStats() const noexcept {
    SinkStats out;
    out.writes = writes_.load(std::memory_order_relaxed);
    out.errors = errors_.load(std::memory_order_relaxed);
    out.total_write_ns = total_write_ns_.load(std::memory_order_relaxed);
    out.max_write_ns = max_write_ns_.load(std::memory_order_relaxed);
    for (std::size_t i = 0; i < SinkStats::kLatencyBuckets; ++i) {
        out.latency_hist[i] = hist_[i].load(std::memory_order_relaxed);
    }
    return out;
}

}  // namespace ulog::sinks
