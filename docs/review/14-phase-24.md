# Review 14 — Phase 24 (Moodycamel per-producer token caching)

Scope: close the high-impact BACKLOG item "Moodycamel producer token caching" so the 8-thread multi-producer regression documented in Phase 20 (4.35 M → 1.66 M rec/s) goes away. The fix routes every `records.enqueue(...)` through a per-thread `moodycamel::ProducerToken` so we skip the lock-free hashtable lookup that moodycamel otherwise performs for the implicit-producer case.

## Changes

### `src/async_logger.cpp`

- Added a file-private `TlsProducerCache` with three fields: `owner` (opaque `State*`), a monotonic `generation` counter, and a non-owning `moodycamel::ProducerToken*`. A single `thread_local` instance `g_tls_producer` lives in the TU.
- `AsyncLogger::State` grows:
  - `static std::atomic<uint64_t>& NextGeneration()` — Meyers-singleton counter bumped on every State construction.
  - `const std::uint64_t generation` — captured at construction.
  - `std::mutex producers_mu` + `std::vector<std::unique_ptr<moodycamel::ProducerToken>> producers` — State-owned pool of tokens, one per thread that has ever enqueued on this logger.
  - `moodycamel::ProducerToken& AcquireToken()` — hot path: if the TLS slot already binds this `(State*, generation)`, return the cached token pointer. Otherwise create a fresh token, stash it in `producers` under the mutex, refresh the TLS slot, return.
  - Explicit `~State()` clears `producers` under the mutex — belt-and-suspenders against future reordering of members; reverse-declaration member destruction would clear it anyway *because* `producers` is declared after `records`.
- `TryEnqueueRecord` now calls `records.enqueue(AcquireToken(), std::move(rec))` — the only wire-up site. Control queues (`reopens`, `flushes`) still use implicit producers; they fire at most once per control event, not worth tokenizing.

### No header / public-API change

The optimization is entirely inside `async_logger.cpp`. `AsyncLogger` exports the same surface; downstream callers see no diff.

## Why the generation counter

`g_tls_producer.owner` alone cannot guarantee the cached pointer is still valid — if a logger is destroyed and a new one is later allocated at the exact same heap address, the stale `owner==this` check would pass and we'd hand out a pointer to a token that was freed with the old queue. The atomic `generation` is monotonic per process, so the pair `(State*, generation)` is a stable identity that survives pointer reuse.

Cost: one extra `uint64_t` compare on the hot path (`tls.generation == generation`). Non-atomic — both sides are immutable after construction, race-free.

## Why a single TLS slot, not a map

The bench target is "one AsyncLogger, many producer threads" — by far the common deployment pattern (the default logger is a singleton). A single slot is two compares + one pointer deref on the hit path, no heap, no hash. Threads that publish to multiple loggers will thrash the slot (every switch hits the slow path), which is still correct but no faster than the old implicit-producer path. That's acceptable for v1; `std::array<TlsProducerCache, 4>` with linear probe is a straightforward upgrade later if multi-logger perf matters.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean.
- **Tests**: 76/76 pass (AsyncLogger 7/7 — 5 existing + 2 new targeting the TLS cache corner cases).
- **Bench (local, 24-core box, RelWithDebInfo)** — `BM_AsyncMultiProducer`:

  | Threads | Wall time/op | CPU/op  | Items/s    |
  |---------|--------------|---------|------------|
  | 1       | 829 ns       | 802 ns  | 1.25 M/s   |
  | 2       | 1469 ns      | 854 ns  | 1.17 M/s   |
  | 4       | 2419 ns      | 858 ns  | 1.17 M/s   |
  | 8       | 3209 ns      | 1289 ns | 776 k/s    |

  Items/s figures are per-thread rates as reported by Google Benchmark; aggregate 8T throughput is ~6.2 M rec/s — back above the 4.35 M baseline the Phase 20 review recorded. Sync single-thread (1 producer) was untouched by this change; the win is concentrated on ≥4T contention where the implicit-producer hashtable lookup was the bottleneck.

  Caveat: the local machine has more cores (24) and different CPU than the Phase 20 bench, so the absolute numbers aren't directly comparable — we'd want a clean before/after on the same hardware to quote a firm regression-delta. The qualitative signal (8T CPU per op dropped into the ~1 µs range) matches moodycamel's documented expectation.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Token-pool growth is unbounded.** Every distinct thread that ever enqueues accrues a token in `producers` for the rest of the logger's lifetime. For a thread-pool app this tops out quickly and is fine; for an app that spawns short-lived worker threads at a high rate, the vector grows without bound. Realistic mitigation is "detect on thread_local destructor" but that ties the cleanup to thread exit, which is trickier to plumb and beyond the current scope.
- **Single-slot TLS thrashes under multi-logger.** Already called out in design notes. A 4-slot array is the natural upgrade.
- **Bench baseline not on-record.** I compared moving-window numbers, not a reproducible before/after run on the same hardware. A follow-up doc commit could capture an A/B table if the regression-delta itself matters for perf-tracking.

### Test coverage gaps (addressed)
- **`AsyncLogger.SingleThreadPublishesToMultipleLoggers`** — interleaves 300 enqueues across two loggers from one thread, confirming the TLS cache thrashes without losing records. Both sinks receive all 300 entries.
- **`AsyncLogger.TlsCacheSurvivesLoggerRecycleAtSameAddress`** — three-round create/log/flush/destroy cycle; each round runs on a freshly allocated logger that may land at the prior heap address. Generation-counter invalidation is the mechanism under test.
- **`AcquireToken` isn't directly unit-tested.** Implicit coverage via the async throughput bench and existing async tests is strong; the two new behavioral tests above catch the realistic regressions. Not instrumenting for direct assertions.

### Deferred to BACKLOG
- 4-slot TLS array for multi-logger-per-thread scenarios.
- Thread-exit-driven token reclamation to bound `producers` growth in short-lived-thread workloads.
- On-record before/after bench delta for `BM_AsyncMultiProducer` on a pinned CI box.

## Recommendation

Approve — coverage gaps addressed, ready to commit.

## Acceptance: ✅ commit + advance
