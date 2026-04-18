# ulog — backlog

Follow-ups identified during review, tracked separately to keep incoming PRs scoped.

## Observability

### RateLimiter: no aggregated drop metric

`logging::impl::RateLimiter` keeps `dropped_count` in a `thread_local RateLimitData` per `LOG_LIMITED_*` call site. The counter is resurfaced into the *next* emitted record as a `[dropped=N]` suffix, then cleared every second.

There is no process-wide aggregation:
- No `GetTotalDroppedByRateLimit()` / stats handler.
- No callback invoked on drop.
- Monitoring systems cannot observe rate-limit pressure until an eligible record finally escapes the throttle.

**Options to evaluate**

1. **Atomic global counter**: bump a `std::atomic<uint64_t>` alongside the thread-local; expose via public getter. Simple, adds one atomic op per dropped record.
2. **Per-source-line registry**: intrusive list of `RateLimitData` instances, enumerable by callers that want a breakdown. Matches userver's statistics::Writer style. Heavier — each LOG_LIMITED site registers once on first hit.
3. **Drop callback**: `SetRateLimitDropHandler(void(*)(const char* file, int line, uint64_t count))`. Lets apps push to their preferred observability system without a pull-based API. No built-in storage.

Preferred: **(1) atomic counter + (3) optional callback**. (2) is a bigger change and duplicates what dynamic_debug already manages.

### Other observability gaps

- No `AsyncLogger::GetQueueDepth()` / pending-records metric (only `GetDroppedCount`).
- No formatter-level record-length histogram (for tuning `SmallString<N>`).
- No per-sink write-latency / error counter.

## Correctness / safety

- **`GetDefaultLogger()` returning `LoggerRef`** is documented as short-lived. Consider deprecating in favor of `GetDefaultLoggerPtr()` in a future major, or at least annotating with `[[deprecated]]` when called in a long-lived context can be detected.
- **`LogHelper(LoggerRef, …)` direct construction bypasses dynamic-debug + ShouldLog**. Consider making that ctor private + friending the macros, so users who want a manual helper go through `LogHelper(LoggerPtr, …)` which keeps ownership and still bypasses the macro filter (user responsibility).
- **Tracing hook is a single callback slot**. Multi-hook (chain) support would let multiple tracing systems coexist without the app writing a fan-out manually.

## Performance

- **Per-log heap alloc on the default-logger path**: `GetDefaultLoggerPtr()` cross-converts `boost::shared_ptr` ↔ `std::shared_ptr` via a lambda-captured deleter. One control-block allocation per LOG call. Options: TLS cache with generation counter, or switch LoggerPtr to boost throughout.
- **AsyncLogger payload copy**: every record is copied from `SmallString<4096>` into a heap `std::string`. Consider a `SmallString`-backed `LogRecord` or an arena.
- **`dynamic_debug::LookupDynamicDebugLog` takes a `shared_lock` even when the registry is empty**. Add an `std::atomic<bool> has_entries` fast path to avoid the lock on the hot read side.
- **TSKV `module=` field emits backslash-escaped Windows paths** (`H:\\Workspace\\...`). Add `ULOG_SOURCE_ROOT` / `USERVER_FILEPATH`-style source-relative trimming.

## Coverage

- Tests for `LOG_LIMITED_*` drop count behavior (flood > 1 second, assert `[dropped=N]` suffix and reset).
- Tests for `DefaultLoggerGuard`.
- Tests for concurrent `SetDefaultLogger` + logging (stress; confirm atomic_shared_ptr path never dangles).
- TCP/Unix socket sink smoke tests against a local listener thread.
- `LogExtra::Stacktrace()` / `Stacktrace` cache behavior.
- `ULOG_ERASE_LOG_WITH_LEVEL=N` compile-erase tests (build + nm/dumpbin assertion that the removed LOG_* expansions produce no symbols).

## Build / CI

- **No CI yet**. Add GitHub Actions matrix: Ubuntu (gcc, clang), macOS (clang), Windows (msvc, clang-cl). Each runs conan install + cmake + ctest.
- **No benchmarks**. Add Google Benchmark suite:
  - Throughput (records/sec, single-producer, multi-producer).
  - Latency distribution (p50/p99 from emit to sink write).
  - Sync vs async vs null logger baseline.
  - Compare with spdlog + glog.

## Features

- **`JsonFormatter::Variant::kYaDeploy`** is currently a no-op stub. Either implement (`@timestamp`/`_level`/`_message` rename) or remove the enum until there's a concrete spec.
- **`ulog::yaml` module** (optional yaml-cpp loader for `LoggerConfig`) — placeholder in CMake, not wired.
- **OTLP sink** — tracked in extraction plan, not started.
- **POSIX Windows-named-event reopen handler** — analog to SIGUSR1 for Windows services. Low priority (apps usually call `RequestReopen` from a control RPC instead).
