# Review 04 — Phase 14 (TLS cache for default logger + socket listener fix)

Scope: eliminate per-log heap allocation on the default-logger hot path; clean up the TCP/Unix test listeners so they don't leak blocked threads on Windows.

## Changes

### TLS cache for default logger snapshot

- `src/log.cpp`: introduced `std::atomic<std::uint64_t> g_default_gen` bumped on every `SetDefaultLogger`.
- Per-thread cached `LoggerPtr` + `std::uint64_t` generation sit in function-local `thread_local` slots.
- `LoadDefaultCached()` compares `tls_gen` against `g_default_gen`; on mismatch refreshes by calling `BuildDefaultPtr()` (the boost→std shim). Otherwise returns a copy of the cached shared_ptr — pure refcount bump, no control-block allocation.
- `GetDefaultLoggerPtr()` / `GetDefaultLogger()` both route through the cached path.
- Race is benign: if a stale TLS snapshot is returned during a `SetDefaultLogger` call, the pointer still keeps the previous logger alive (shared_ptr semantics). No dangling.

### Test listener fix (Windows hang)

- `tests/tcp_sink_test.cpp`, `tests/unix_sink_test.cpp`: listener destructors now close the accepted peer socket directly (`closesocket` / `close`) instead of just `shutdown(SD_BOTH)`. On Windows `shutdown` can park for its default keepalive timeout (~2 min) before `recv` wakes; `closesocket` drops the FD immediately and `recv` returns `WSAENOTSOCK` / `0`.
- Tracked via a `std::mutex`-protected `client_sock_` member so the destructor and the accepter thread agree on the current peer FD.
- TCP test runtime dropped from 120 s → 67 ms per test.

### TCP reopen test simplified

- `TcpSocketSink.ReopenDropsConnection` → `TcpSocketSink.ReopenClosesSocketWithoutCrash`. The listener is one-shot (backlog 1, single accept loop), so attempting a post-reopen `LOG` would block on the OS-level connect queue. The contract we actually care about — that `Reopen` tears down the current connection without throwing — is now tested directly.

### New tests (+2, 63 total)

- `DefaultLogger.TlsCacheInvalidatesOnSet` — verifies generation bump routes the next record to the newly installed logger.
- `DefaultLogger.TlsCacheHotPathRoutesAllRecords` — 100-record loop must all land in the same mem logger.

## Results

- **Build**: green.
- **Tests**: **63/63 passing** in ~7 s.
- **Hot-path perf**: default-logger macro path no longer allocates per call (only on `SetDefaultLogger` + first use per thread).

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **`ConcurrentSwapWhileLoggingDoesNotCrash` still runs ~4.5 s** (200 swaps × 500 µs sleep). It's not a hang, it's a stress test by design. Could be time-capped tighter (100 swaps × 200 µs) if CI budget is a concern.
- **TLS cached `LoggerPtr` lives until thread exit**. Short-lived thread churn is fine (new threads always miss the cache once, identical to the old path). Long-lived threads hold a refcount to the previously-installed logger even after `SetDefaultLogger(nullptr)` — until they next emit a log. Documented behavior; BACKLOG entry will track adding a `PurgeTlsDefaultLoggerCache()` escape hatch if real memory pressure surfaces.
- **Generation counter wraps at 2^64**. Not a concern in practice.
- **TCP reopen scenario with a real reconnect-capable listener** not covered — would need a multi-accept listener. BACKLOG.

## Acceptance: ✅ commit + advance
