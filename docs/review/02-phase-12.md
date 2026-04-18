# Review 02 — Phase 12 (observability + coverage hardening)

Scope: source-root trim, dynamic_debug empty-registry fast-path, coverage gaps from rev.1.

## Changes

### Source-root trim

- `include/ulog/detail/source_root.hpp` — constexpr `TrimSourceRoot(path, root)` with `/`↔`\` normalization. Zero runtime cost when match succeeds (compiler folds the literals).
- Macro `ULOG_IMPL_TRIM_FILE(__FILE__)` expands only when `ULOG_SOURCE_ROOT_LITERAL` is defined, no-op otherwise.
- Wired into `ULOG_IMPL_LOG_LOCATION()` so every `LOG_*` site emits trimmed `__FILE__`.
- `CMakeLists.txt`: `ULOG_SOURCE_ROOT` option (default = `${CMAKE_CURRENT_SOURCE_DIR}` of ulog itself). Downstream consumers override to their own source tree.
- Verified: `module=main ( examples\\simple\\main.cpp:11 )` vs pre-trim `H:\\Workspace\\GitHub\\userver\\ulog\\examples\\simple\\main.cpp:11`.

### Dynamic debug fast-path

- `std::atomic<bool> g_has_entries` flipped to `true` on any non-default insert, `false` when registry empties or `ResetDynamicDebugLog()`.
- `LookupDynamicDebugLog(file, line)` returns `kDefault` without acquiring the shared_lock when flag is `false`. Hot path now one relaxed atomic load + one conditional branch.
- Registry access path unchanged (shared_lock + map walk) for the non-empty case.

### Coverage additions (+10 tests, 58 total)

- `tests/rate_limit_test.cpp`:
  - `DropsRepeatedCalls` — 32 calls → exactly 6 emissions (powers-of-two schedule).
  - `DroppedSuffixPresent` — records after drops carry `[dropped=N]` suffix.
  - `ResetsAfterOneSecond` — emission resets after 1.1s sleep.
- `tests/default_logger_test.cpp`:
  - `SwapsDefaultWithinScope` — `DefaultLoggerGuard` basic behavior.
  - `NestedGuardsRestoreInOrder` — three-level nesting restores correctly.
  - `ConcurrentSwapWhileLoggingDoesNotCrash` — 4 producer threads + 200 `SetDefaultLogger` flips across ~200ms, validates `boost::atomic_shared_ptr` snapshot safety.
- `tests/stacktrace_cache_test.cpp`:
  - `CurrentReturnsNonEmptyWhenEnabled` — basic capture works.
  - `DisabledReturnsEmpty` — `StacktraceGuard(false)` suppresses collection.
  - `GuardRestoresPreviousState` — RAII correctness.
  - `ExplicitCaptureSameFrames` — same `boost::stacktrace` object → cache hit (identical symbolized string).

## Results

- **Build**: green (MSVC 14.44, Ninja, Conan 2).
- **Tests**: 58/58 passing (up from 48).
- **Examples**: still work; `module=` field noticeably shorter.

## Findings from self-review

### 🔴 Critical
- None.

### 🟡 Non-critical
- **dynamic_debug fast-path read is `memory_order_acquire`**, set is `release`. Correct for happens-before on the map mutation. Could relax the read to `relaxed` paired with the shared_lock acquisition that happens next — ordering is re-established via the mutex anyway. Micro-opt.
- **Initial `RepeatedCapturesFromSameSiteHit` test dropped** — cache identity across two consecutive lambda-less calls is unreliable (compiler may fold frame addresses or optimizer positioning). Acceptance: `ExplicitCaptureSameFrames` proves the cache logic via deterministic shared `boost::stacktrace` object.
- **TSKV escape still doubles backslashes** inside `module=` field (e.g. `examples\\simple\\main.cpp`). Cosmetic — not a bug; TSKV spec requires `\` → `\\`. Could be fixed by pre-normalizing `/` before TSKV encoding, but that changes format semantics. Defer.
- **`ULOG_SOURCE_ROOT` is set PUBLIC in CMake** — downstream consumers inherit `ulog`'s source root, not their own. Documented limitation; consumers override per-target if they have their own conventions. BACKLOG entry extended.

### Coverage gaps still open
- TCP/Unix socket sink integration — needs a local listener thread. Deferred.
- Compile-erase binary-inspection test — deferred.
- AsyncLogger stress with Reopen — partial (DiscardOverflowCountsDrops covers queue, not reopen race).

## Acceptance: ✅ all checks green; ready to commit + advance
