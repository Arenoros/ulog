# Review 19 — Phase 29 (per-sink metrics via `InstrumentedSink`)

Scope: close the high-impact BACKLOG item "Per-sink metrics — write latency histogram + error counter". Produces the counter surface operators ask for ("which sink is slow? which one is throwing?") via an opt-in decorator so the cost lands only where users want it.

## Changes

### New headers

- **`include/ulog/sinks/sink_stats.hpp`** — declares `SinkStats`: `writes`, `errors`, `total_write_ns`, `max_write_ns`, and a 32-bucket power-of-two `latency_hist[]`. Inline helpers `MeanWriteNs()` and `PercentileNs(p)` do the common derived reporting.
- **`include/ulog/sinks/instrumented_sink.hpp`** — `InstrumentedSink` is a `BaseSink` decorator that wraps any `SinkPtr`, records per-call timing + counters, and forwards `Flush` / `Reopen`. `Inner()` exposes the wrapped sink for sink-specific downcasts. `MakeInstrumented(inner)` returns the decorator as `shared_ptr<InstrumentedSink>` so callers keep the typed handle they need for `GetStats()`.

### Base API

- **`BaseSink::GetStats()`** added as a virtual returning zeroed stats by default. Non-instrumented sinks pay zero cost (the virtual never fires in hot paths); callers don't have to care whether a given sink is wrapped. Vtable grew by one slot — source-compatible, not strictly ABI-compatible (same caveat as Phase 23's `SetTraceContext`).

### Implementation (`src/sinks/instrumented_sink.cpp`)

- **Bucket mapping** — `BucketOf(ns)` uses `__builtin_clzll` / `_BitScanReverse64` so the hot path stays single-digit ns. Last bucket is saturating ("≥ 2³¹ ns").
- **Max is race-safe** — `BumpMax` does a relaxed CAS loop, so concurrent producers cannot clobber each other's observations. Counters use `fetch_add(relaxed)` because the snapshot reader is the only consumer and doesn't need strict ordering across fields; a future follow-up could add `std::memory_order_acq_rel` paired reads if we ever expose concurrent "delta" snapshots.
- **Exception path** — errors increment `errors_`, timing is recorded regardless, the exception is rethrown via `std::exception_ptr` so both SyncLogger (wants propagation) and AsyncLogger (swallows) keep their existing behavior.

### CMake wiring

- `src/CMakeLists.txt` picks up `sinks/instrumented_sink.cpp`.
- `tests/CMakeLists.txt` adds `instrumented_sink_test.cpp`.

### Tests (`tests/instrumented_sink_test.cpp`)

- `InstrumentedSink.DefaultStatsAreZero` — baseline sink returns the zero struct.
- `InstrumentedSink.CountsSuccessfulWrites` — three writes, `writes=3`, `errors=0`, inner actually received them.
- `InstrumentedSink.CountsErrorsAndRethrows` — throwing inner sink: exception propagates AND `errors=2`, `writes=0`, latency hist carries both samples.
- `InstrumentedSink.AccumulatesLatencyHistogram` — 2 ms SlowSink × 5 writes: `max_write_ns ≥ 2 ms`, `total_write_ns ≥ 5·2 ms`, `PercentileNs(0.99) > 0`.
- `InstrumentedSink.ForwardsFlushAndReopen` — decorator delegates control-plane calls.
- `InstrumentedSink.ConcurrentWritesAccumulateDeterministically` — 4 threads × 500 writes — `writes = 2000`, `errors = 0`. Locks in the atomic ordering.
- `SinkStats.PercentileZeroForEmptyHistogram` — helper stability on empty data.
- `SinkStats.PercentileMonotonicInP` — synthesized histogram, asserts `p50 ≤ p90 ≤ p99`.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean.
- **Tests**: 91/91 green (81 previous + 10 new — 8 core + 2 coverage-gap tests added post-review).

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Overhead per wrapped write is real**: two `steady_clock::now()` calls plus ~5 relaxed atomic fetch_adds. Order of 40-60 ns on commodity x86. For a disk-bound file sink that already costs µs-ms, the overhead is invisible. For an in-process null-sink stress test, it would show up — users who want it strip the wrapper.
- **Histogram is log-base-2, not log-base-10 or linear**: coarse. p99 between `[2^i, 2^(i+1))` can only be named by the upper bound `2^(i+1) - 1`. Acceptable for ops-grade "is the sink OK?" alerts; a true ms-resolution p99 needs HDR/t-digest.
- **`GetStats()` is a snapshot copy** of 32 × 8 B (hist) + 4 × 8 B = 288 B per call. Not free but a small fraction of a monitoring tick.
- **No interval helper** — callers that want "writes per minute" must store two snapshots and subtract. Ergonomic nit; library stays primitive.
- **No per-sink identifier in the snapshot**. Callers track which InstrumentedSink maps to which logical destination themselves. Adding an optional `std::string name` would be trivial if that pattern emerges.

### Test coverage gaps (addressed)
- **`InstrumentedSink.InnerReturnsOriginalSink`** — asserts the accessor returns the wrapped sink verbatim.
- **`InstrumentedSink.ComposesWithSyncLogger`** — end-to-end: `SyncLogger(Info)` + `MakeInstrumented(CountingSink)`, emits one filtered DEBUG + one INFO + one ERROR; stats show `writes=2`, `errors=0`, and the inner sink received exactly the two records that passed the level filter.

### Deferred to BACKLOG
- **`SetRateLimitDropHandler` callback** (separate BACKLOG item — not in this phase).
- **Histogram resolution upgrade** to something HDR-like if ops ever needs proper ms-resolution p99.
- **Optional name field** + stats-aggregation helpers (named collection, merge, printer).

## Acceptance: ✅ commit + advance
