# Review 06 — Phase 16 (observability metrics)

Scope: close two observability gaps tracked in `BACKLOG.md` — add a process-wide rate-limit drop counter and expose async queue depth / total records for AsyncLogger.

## Changes

### RateLimiter global drop counter

- `impl::g_rate_limit_dropped_total` — file-local `std::atomic<std::uint64_t>` incremented once per drop via `fetch_add(relaxed)`. Zero overhead on the common emit path.
- Public API:
  - `std::uint64_t ulog::GetRateLimitDroppedTotal() noexcept`
  - `void ulog::ResetRateLimitStats() noexcept`
- Unit test `RateLimit.GlobalDroppedCounterAggregates` floods 64 records, asserts `emitted + dropped == 64` and that dropped > 0.

### AsyncLogger stats

- Two new accessors on `ulog::AsyncLogger`:
  - `GetQueueDepth() -> std::size_t` — live snapshot of `pending` atomic.
  - `GetTotalLogged() -> std::uint64_t` — cumulative records handed to sinks.
- `total_logged` is bumped inside `drain_records` alongside the existing `pending -= n`, so sinks-to-monitoring count matches dequeue count exactly.
- Unit test `AsyncLogger.QueueDepthAndTotalLoggedMetrics` — both counters start at 0, 500 records through flush → `total == 500`, `depth == 0`.

## Results

- **Build**: green.
- **Tests**: **65/65 passing** (up from 63) in ~7 s.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **`total_logged` is bumped on dequeue, not after sink write completes.** If a sink throws during `Write`, the record is counted as logged even though it was actually dropped. Given sinks already `try/catch` internally and swallow their own failures, the counter reflects "attempted to log" not "successfully persisted". Documented behavior. A strict "delivered to at least one sink" counter would need per-sink bookkeeping.
- **No per-sink metrics.** BACKLOG entry retained — per-sink write latency / error count are still open.
- **`ResetRateLimitStats` does not reset per-thread `RateLimitData` slots.** The next emit window still uses whatever `count_since_reset` / `dropped_count` the producers saw last. Global counter is separate from the per-line budget — intentional (resetting all per-thread buckets would race with producers).
- **`GetQueueDepth` is `memory_order_relaxed`** — readers may see slightly stale values. Fine for diagnostics.

## Acceptance: ✅ commit + advance
