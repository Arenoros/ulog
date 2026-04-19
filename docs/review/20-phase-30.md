# Review 20 — Phase 30 (coverage-gap tests batch)

Scope: close three of the five BACKLOG "Coverage / testing gaps" in one sweep. The remaining two (compile-erase binary assertion, stacktrace real symbolication) require platform-specific CMake fixtures that belong in their own phase.

## Changes

### `tests/formatter_test.cpp`

- **`FormatterJson.YaDeployPassesThroughAddJsonTag`** — closes the BACKLOG item "AddJsonTag + YaDeploy combo test". Prior coverage exercised the YaDeploy variant only via the stringifying `AddTag` path; this pins the raw-JSON path so a `JsonString` tag lands unquoted and the canonical `@timestamp` / `_level` / `_message` renames stay intact in the same record.
- **`LogHelperExceptions.WithExceptionCapturesTypeAndMessage`** — closes "LogHelper::WithException test". Throws a `std::runtime_error("boom-42")`, catches it, attaches via `WithException(ex)`. Asserts that both `exception_msg=boom-42` and an `exception_type=` tag containing the substring `runtime_error` land in the TSKV record alongside the streamed text. The type-string assertion is a substring match because `typeid(ex).name()` is implementation-defined (MSVC: human-readable, libstdc++: Itanium-mangled).

### `tests/tcp_sink_test.cpp`

- **`MultiAcceptListener`** — a new helper class that loops on `accept()`, spawning one reader thread per accepted peer. Consolidated buffer across connections; `AcceptedConnections()` counter. The dtor explicitly closes both the listen socket and any held peer sockets before joining readers — otherwise a leftover blocked `recv()` hangs the test (seen in first iteration before the explicit teardown was added).
- **`TcpSocketSink.ReopenReconnectsToListener`** — closes "TCP reopen с multi-accept listener". Writes record → Flush → `Reopen(kAppend)` → writes another record → Flush. Asserts both records land in the consolidated buffer AND the listener accepted exactly two separate connections. Catches the silent-failure shape where `Reopen` + subsequent `Write` could have left the sink in a state that swallowed the second record.

## Why a dedicated listener, not a patched `LocalListener`

`LocalListener` is used by the existing pre-reopen delivery test and the single-shot reopen test. Those tests depend on the current behavior (accept once, read into one buffer, close both sockets in dtor) — extending the same class to multi-accept adds state that a single-conn test doesn't want to pay for. Keeping the two listeners side-by-side keeps each test's reasoning local; total LOC cost is ~70 lines.

## Findings during implementation

- **First iteration hung** because the listener dtor only closed the listen socket; the reader thread that was still inside `recv()` on an accepted peer never woke up. Fix: track peer sockets under a mutex and close them explicitly in the dtor. The reader also nulls its entry under the same mutex before its own `closesocket` so the dtor doesn't double-close. Added to the listener design above.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean.
- **Tests**: 94/94 green (91 previous + 3 new). Full TCP suite runs in ~240 ms including the 170 ms multi-accept case (blocking sleeps dominate).

## Remaining Coverage-gap items

- **Compile-erase binary assertion** — needs a CMake add_test fixture that runs platform-specific `dumpbin /SYMBOLS` (Windows) or `objdump -t` (Linux/macOS) against a build with `ULOG_ERASE_LOG_WITH_LEVEL=3` and greps the binary for the erased text. Medium effort, deferred.
- **Stacktrace с реальной символизацией** — gated on PDB availability (MSVC) / debug symbols (gcc). Flaky across CI runners. Deferred.

## Findings on the new tests

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Listener teardown pattern is specific to the test.** If a future test forgets the "close listen + close peers + join accepter + join readers" sequence, it will hang on Windows (peer recv doesn't wake on peer-side closesocket in all scenarios). Worth extracting into a shared helper if a third variant lands — premature for two.
- **The multi-accept test relies on sleeps for synchronization** (50 ms, 100 ms). Flaky budget is low on CI boxes with loaded runners; an "expected conn count" retry loop would be safer. Kept simple for now because the existing single-accept test uses the same 50 ms pattern without observed flake.

### Test coverage gaps (addressed)
- AddJsonTag + YaDeploy: covered.
- WithException: covered.
- TCP multi-accept reopen: covered.

## Acceptance: ✅ commit + advance
