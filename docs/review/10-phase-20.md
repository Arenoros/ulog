# Review 10 — Phase 20 (bench expansion)

Scope: ship the three bench scenarios that were missing from Phase 18 so the perf picture covers contention, I/O-bound sinks, and formatter A/B.

## Changes

- `bench/multi_producer_bench.cpp` — `BM_AsyncMultiProducer` with `Threads(1/2/4/8)`. Shared `AsyncLogger` is set up on thread 0 before the loop and flushed on thread 0 after.
- `bench/slow_sink_bench.cpp` — `BM_SyncSlowSink` / `BM_AsyncSlowSink` parametrized by `Arg(0/1/10/50)` microseconds of artificial sink latency. SlowSink busy-waits with `std::chrono::high_resolution_clock` because Windows' `std::this_thread::sleep_for(1us)` actually sleeps ~15 ms (scheduler quantum).
- `bench/formatter_bench.cpp` — `BM_FormatThroughput<F>` template instantiated for TSKV, LTSV, RAW, JSON, all routing through a DiscardSink so only formatter cost is on the critical path.
- Iteration caps (`->Iterations(2000)`) on slow-sink bench to keep total wall time bounded; `--benchmark_min_time` would otherwise blow up at 50 µs per call.

## Results

Windows MSVC 14.44, RelWithDebInfo, `--benchmark_min_time=0.05s`:

### Core (unchanged from Phase 18-19)

| | Time |
|---|---|
| `BM_GetDefaultLoggerPtr` | 10.7 ns |
| `BM_LogThroughNullDefault` | 12.8 ns |
| `BM_SyncLoggerThroughput` | 715 ns |
| `BM_AsyncLoggerThroughput` | 806 ns |

### Multi-producer async (`BM_AsyncMultiProducer/threads:N`)

| Threads | Time per iter | Effective throughput |
|---|---|---|
| 1 | 896 ns | 1.12 M rec/s |
| 2 | 1330 ns | 1.50 M rec/s |
| 4 | 920 ns | **4.35 M rec/s** |
| 8 | 4806 ns | 1.66 M rec/s |

Scales near-linearly to 4 producers, degrades at 8 — moodycamel queue contention + cv-wake thrash dominate. Matches expected behavior for a single-consumer MPSC pattern.

### Slow-sink: where Async matters (`BM_SyncSlowSink` vs `BM_AsyncSlowSink`)

| Sink delay | Sync | Async |
|---|---|---|
| 0 µs | 808 ns | 1046 ns |
| 1 µs | 1676 ns | 1023 ns |
| 10 µs | 10865 ns | 873 ns |
| 50 µs | 51252 ns | 10011 ns |

Key observation: producer-side latency of the async logger **does not grow with sink delay until the queue backs up**. Sync logger latency is linear in sink cost. At 10 µs sink delay the async producer is 12× faster; at 50 µs the async producer starts to block because the worker can't keep up (queue saturates). This is the classic async sweet spot — it's visible in numbers now, not folklore.

### Formatter A/B (`BM_FormatThroughput<F>`, with two LogExtra tags)

| Format | Time | Throughput |
|---|---|---|
| `kRaw` | 346 ns | 3.21 M rec/s |
| `kLtsv` | 825 ns | 1.28 M rec/s |
| `kTskv` | 826 ns | 1.27 M rec/s |
| `kJson` | 1235 ns | 717 k rec/s |

RAW (no header) is ~2.4× faster than TSKV/LTSV — the eager timestamp/level/module writes are a real cost. JSON costs ~50 % more than TSKV largely because of the `std::vector<Field>` staging + quote/control-char escaping. A direct-emit JSON formatter (without the staging vector) would narrow the gap; BACKLOG entry added.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Async slow-sink/50 µs bench** shows 10 011 ns/call — producer is now blocking on capacity. The number measures "how fast you can fill a 65 k-deep queue before the worker gates you", not pure producer overhead. Expected behavior at that load; noted in review.
- **Multi-producer at 8 threads regresses.** Traceable to: (a) moodycamel per-producer token miss, (b) shared `pending` atomic contention, (c) cv-notify path even though batch notify is in place. Producer tokens could be cached per-thread → BACKLOG.
- **SlowSink uses busy-wait.** Fine for benches but costs an entire CPU core during the run; the multi-producer × slow-sink combination was not attempted.
- **`BM_LogThroughNullDefault` moved from 11 → 12.8 ns** (pre-Phase-19 was 10.4). Attributable to the extra `make_unique<TextLogItem>` even in the NullLogger path — ShouldLog short-circuits too late. Fix possible by short-circuiting on logger level BEFORE constructing formatter.

### Deferred to BACKLOG
- Direct-emit JSON formatter (no staging vector).
- Multi-producer token caching for moodycamel.
- Short-circuit formatter construction when `logger.ShouldLog` returns false (saves the TextLogItem alloc on sub-level calls).
- Multi-producer × slow-sink matrix bench.

## Acceptance: ✅ commit + advance
