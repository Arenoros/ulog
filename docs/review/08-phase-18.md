# Review 08 — Phase 18 (Google Benchmark perf suite)

Scope: stand up micro-benchmarks for the hot paths so future changes can be compared against a reference baseline.

## Changes

### Build wiring

- `conanfile.txt`: added `benchmark/1.9.4` with `shared=False`.
- `CMakeLists.txt` already had `ULOG_BUILD_BENCH` option (OFF by default). Flipping it to ON at configure time pulls in the benchmarks.
- `bench/CMakeLists.txt` — defines the `ulog-bench` executable linking `ulog::ulog + benchmark::benchmark`.
- `bench/bench_main.cpp` — single TU with `BENCHMARK_MAIN()`.

### Benchmarks

- `bench/default_logger_bench.cpp`:
  - `BM_GetDefaultLoggerPtr` — isolated cost of the TLS cache snapshot used by every `LOG_*` macro.
  - `BM_LogThroughNullDefault` — full LOG pipeline into a NullLogger (record discarded at the sink; measures macro + ShouldLog fast-path).
- `bench/sync_throughput_bench.cpp`:
  - `BM_SyncLoggerThroughput` — end-to-end TSKV format + dispatch into a DiscardSink. Reports `items_per_second`.
- `bench/async_throughput_bench.cpp`:
  - `BM_AsyncLoggerThroughput` — producer-side cost with a 65k-slot block-on-full queue + DiscardSink + explicit Flush at teardown.

### Conan / toolchain fix

Benchmark was cached with `compiler.version=195`; my local `cl` is 19.44 (v194) whose STL lacks the newer `__std_regex_transform_primary_char` symbol emitted by 195-era headers. Same class of issue as gtest earlier. Resolved with per-package override: `-s "benchmark/*:compiler.version=194"`. Recorded in `ulog/build-helper.bat` invocation notes.

## Results

Windows MSVC 14.44, release-with-debuginfo, `--benchmark_min_time=0.2s`:

| Benchmark | Time | Notes |
|---|---|---|
| `BM_GetDefaultLoggerPtr` | 10 ns | TLS cache hit + refcount bump |
| `BM_LogThroughNullDefault` | 10 ns | full macro with level=kNone — skips body |
| `BM_SyncLoggerThroughput` | 683 ns | 1.52 M records/s, TSKV + discard sink |
| `BM_AsyncLoggerThroughput` | 858 ns | 1.12 M records/s, producer cost; queue block-on-full |

Regression check: 67/67 tests still green in 7.2 s.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **BM_LogThroughNullDefault measures the NullLogger short-circuit path** (ShouldLog returns false because `kNone`). That's the correct "logging disabled" reference, but the number doesn't represent a real logging cost. A complementary `BM_LogThroughDiscardAtSinkLevel` (logger accepts, sink swallows) would capture the "always-emit" baseline; covered by `BM_SyncLoggerThroughput`.
- **Async bench uses `kBlock`** to avoid drops under the producer. `kDiscard` would be a separate data point — add when the need arises.
- **Single-threaded async bench only**. Multi-producer contention is useful; add `BENCHMARK(BM_AsyncLoggerThroughput)->Threads(4)` variant when tuning.
- **No formatter A/B**. TSKV vs JSON comparison would be useful to expose which format dominates the sync latency.

### Deferred to BACKLOG
- Multi-threaded async bench, `kDiscard` variant, formatter A/B, sanitizer builds (ASan/TSan/UBSan).

## Acceptance: ✅ commit + advance
