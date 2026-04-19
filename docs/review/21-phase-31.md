# Review 21 — Phase 31 (API polish batch)

Scope: close the three BACKLOG "API polish" items in one pass — an escape hatch for the per-thread default-logger cache, a `[[deprecated]]` annotation on the raw-reference accessor that hides a lifetime foot-gun, and a README section that tells downstream consumers how to configure `ULOG_SOURCE_ROOT`.

## Changes

### `PurgeTlsDefaultLoggerCache()` — `include/ulog/log.hpp` + `src/log.cpp`

- New public free function. Resets the calling thread's TLS `LoggerPtr` cache and pushes the TLS generation counter to the sentinel so the next `LOG_*` refreshes from the global slot.
- The motivating scenario: a long-lived worker thread receives a `SetDefaultLogger(nullptr)` (or a hot swap); if that thread then idles without emitting another record, its TLS slot keeps the superseded logger — and its sinks — alive indefinitely. Calling `PurgeTlsDefaultLoggerCache()` drops the ref right now.
- Niche but cheap: one `reset()` + one `uint64_t` assignment. No heap, no locks.

### `[[deprecated]]` on `GetDefaultLogger()` — `include/ulog/log.hpp`

- `LoggerRef GetDefaultLogger()` gains `[[deprecated(...)]]` with a message that steers callers toward either the `LOG_*` macros (which internally snapshot a `LoggerPtr` already) or `GetDefaultLoggerPtr()` (for call sites that need explicit access). The raw reference is only safe for a single logging op that will not race with `SetDefaultLogger` — a subtle contract that has burned people before.
- Internal callers migrated: `log.cpp` now uses a file-local `GetDefaultLoggerInternal()` helper for the one-off places that still need a ref (level accessors, `LogFlush`, `DefaultLoggerLevelScope` ctor). That keeps `-W4 -Wdeprecated-declarations`-style warnings from firing on ulog's own TUs.
- Test migration: `tests/log_macros_test.cpp` now pins the logger with `GetDefaultLoggerPtr()` + deref so the `LOG_*_TO(log, …)` variants still compile without tripping the deprecation warning.

### README: `ULOG_SOURCE_ROOT` consumer guide

- New "Trimming `__FILE__` in consumer builds" subsection of the CMake options block. Covers two deployment shapes explicitly:
  - `add_subdirectory` consumers: set `ULOG_SOURCE_ROOT` before the `add_subdirectory(...)` call — all link-through targets inherit the correct root.
  - `find_package(ulog)` / pre-built Conan consumers: the root was baked in at package-build time; prefer rebuilding with a package-level option. If not possible, `target_compile_definitions(myapp PRIVATE ULOG_SOURCE_ROOT_LITERAL="...")` overrides per-TU, at the cost of a redefinition warning.
- Added a new row to the CMake options table for `ULOG_SOURCE_ROOT`.

## Tests

- **`DefaultLogger.PurgeTlsDefaultLoggerCacheReleasesPriorLogger`** — creates a `MemLogger`, installs it, warms the TLS cache with one `LOG_INFO`, drops the global slot + the local strong ref. A `weak_ptr` observation confirms the logger is still held by the TLS cache; then `PurgeTlsDefaultLoggerCache()` is called and the weak ref goes expired. Locks in the purge-drops-ref contract.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean. No `-Wdeprecated-declarations` warnings on ulog's own TUs because all internal callers route through the non-deprecated helper.
- **Tests**: 95/95 green (94 previous + 1 new).

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **`[[deprecated]]` is strictly source-level**: downstream callers will see the warning on the next rebuild. Absent `-Werror=deprecated-declarations` in their build, nothing actually breaks — this is a gentle nudge, not a removal.
- **The README override story for pre-built ulog packages leaves a compile-warning taste**. That is the honest state: ulog's `ULOG_SOURCE_ROOT_LITERAL` is a `PUBLIC` compile definition, so a consumer-level redefine is by design a "consumer wins, compiler warns" pattern on MSVC. A cleaner path is to gate the PUBLIC define on `#ifndef ULOG_SOURCE_ROOT_LITERAL` inside a generated header, but that's a follow-up — the README captures the current, documented state.
- **`GetDefaultLoggerInternal()` is file-local**. If another ulog TU grows a use of the deprecated accessor later, we'll want to promote the helper to `impl::` visibility. Not urgent.

### Test coverage gaps
- **No test for the `[[deprecated]]` annotation itself** — one would need a compile-fail test harness, which ulog doesn't have yet. Considered but not worth the infrastructure.
- **No test for the README snippet** — docs-only change.

### Deferred to BACKLOG
- A `#ifndef ULOG_SOURCE_ROOT_LITERAL` guard around the PUBLIC compile definition so consumer-level overrides don't warn. Belongs in a dedicated "clean up compile-definition story" phase if the demand surfaces.
- Migrating the remaining `GetDefaultLogger()` call site in `default_logger_test.cpp` (there isn't one — already covered).

## Acceptance: ✅ commit + advance
