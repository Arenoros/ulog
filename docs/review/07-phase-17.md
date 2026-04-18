# Review 07 — Phase 17 (Windows named-event reopen handler)

Scope: bring SIGUSR1 parity to Windows. Log-rotation tooling on Windows services can't send POSIX signals; a named Win32 Event is the closest equivalent.

## Changes

### Public API

- `include/ulog/win/named_event_handler.hpp` (guarded by `#if defined(_WIN32)`):
  - `InstallNamedEventReopenHandler(logger, name = "Local\\ulog-reopen")` — starts a watcher thread that waits on the named event. On `SetEvent`, calls `logger->RequestReopen(kAppend)`.
  - `UninstallNamedEventReopenHandler()` — stops the watcher, joins, closes handles, drops the weak_ptr. Idempotent.
- Default event name `"Local\\..."` confines signal to the current user session. Use `"Global\\..."` to cross sessions (requires `SeCreateGlobalPrivilege`).

### Implementation

- `src/win/named_event_handler.cpp`:
  - Two Win32 events: the user-visible named auto-reset event (rotation signal) and an anonymous auto-reset "stop" event used to break `WaitForMultipleObjects` on uninstall.
  - Worker thread loops on `WaitForMultipleObjects(2, {event, stop}, FALSE, INFINITE)`. `WAIT_OBJECT_0` (event) → `RequestReopen`; `WAIT_OBJECT_0+1` (stop) → exit; anything else → exit.
  - `logger` held via `std::weak_ptr` so installation doesn't prevent the AsyncLogger from being destroyed.
  - Single global slot — a second `Install` call uninstalls the prior watcher first (documented behavior).

### Test

- `tests/named_event_reopen_test.cpp` (Windows only):
  - `InstallsThenForwardsSetEvent` — installs a handler against an AsyncLogger carrying a sink that counts Reopen calls. External side opens the same named event via `OpenEventA(EVENT_MODIFY_STATE)`, calls `SetEvent`, waits up to 2 s for the counter to bump. Asserts `count >= 1`.
  - `UninstallIsIdempotent` — calling uninstall twice without install is a no-op.

### Build wiring

- `src/CMakeLists.txt`: `if(WIN32) target_sources(ulog PRIVATE win/named_event_handler.cpp)`.
- `tests/CMakeLists.txt`: Windows-only `named_event_reopen_test.cpp`.

## Results

- **Build**: green on Windows (MSVC 14.44).
- **Tests**: **67/67 passing** in ~7.4 s. New Windows suite (2 tests) completes in ~92 ms.
- **POSIX**: unaffected (header guarded, source not compiled).

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Single global watcher slot**. Parallel multi-logger setups would need either multiple slots (keyed by name) or a dedicated watcher type. Current design matches the expected "one service, one logger" deployment; extend on demand.
- **`std::weak_ptr<AsyncLogger>` captured when Install runs**. If the caller drops the logger and subsequently fires the event, the watcher's `lock()` yields null → silently no-op. Intentional.
- **`Install` + `Uninstall` not concurrency-safe against each other** beyond what the internal `mu` guarantees. Best practice: call on a single control thread at service startup / shutdown.
- **Watcher thread is not a daemon** — uninstall must run before process exit or the join blocks indefinitely if the event never fires and stop is never set. Addressed by always pairing Install/Uninstall in application lifecycle hooks.

### Deferred
- Multi-event support (e.g. separate Reopen vs. Shutdown events).
- Integration with `SyncLogger` — rotation still routes through `AsyncLogger::RequestReopen`; SyncLogger users must call `sink->Reopen()` directly.

## Acceptance: ✅ commit + advance
