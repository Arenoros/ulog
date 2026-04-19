# Review 22 — Phase 32 (rate-limit drop callback)

Scope: close the BACKLOG "RateLimiter per-source callback" item — push-style observability for `LOG_LIMITED_*` drops, complementing the pull-style `GetRateLimitDroppedTotal()` surface that already exists.

## Changes

### `include/ulog/log.hpp`

- New public `RateLimitDropEvent` POD: `file`, `line`, `site_dropped`, `total_dropped`.
- New public handler alias `RateLimitDropHandler = void (*)(const RateLimitDropEvent&) noexcept` plus `SetRateLimitDropHandler(handler) noexcept` for installation. Pass `nullptr` to deregister.
- `RateLimiter` ctor gains `file` + `line` parameters so each call site's identity is preserved through to the handler.
- `ULOG_LOG_LIMITED_TO` macro threads `ULOG_IMPL_TRIM_FILE(__FILE__)` and `__LINE__` into the new ctor. The trim helper is the same one `module=` uses, so handler payloads agree with the records' own source paths.

### `src/log.cpp`

- Added `std::atomic<RateLimitDropHandler> g_rate_limit_drop_handler{nullptr}` paired with the existing global drop total.
- `RateLimiter::RateLimiter(data, file, line)` — on drop, bumps the global counter, reads it back, and invokes the handler (if any) with the freshly-incremented `total_dropped` and the per-site `dropped_count`. Handler load uses `acquire` so installers can prepare any handler-local state with release.
- `SetRateLimitDropHandler(h)` stores with release. One process-wide slot; reinstall overwrites.

## Design notes

- **Zero-cost on the emit path.** The handler check only runs on the drop branch. The emit branch is unchanged.
- **Handler may not throw.** Marked `noexcept` on the typedef so the compiler would reject a non-noexcept function pointer. Producers run on the caller's thread — blocking or throwing handlers would corrupt the caller's state.
- **`noexcept` via acquired pointer.** Between load and call, another thread can install a new handler or clear the slot. The ABA-style concern ("freed handler called") doesn't apply because the handler is a plain function pointer with static lifetime.
- **No per-site counter separate from the existing RateLimitData.** The `site_dropped` field returns the current window's drop count (resets every 1 second alongside the emit count). If a consumer wants lifetime-of-site drops, they accumulate in the handler.
- **Trimmed file path.** Matches the shape of `module=` in records so cross-referencing handler output with log lines is trivial.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean.
- **Tests**: 97/97 green (95 previous + 2 new).

## Tests

- `RateLimit.DropHandlerFiresOnSuppressedCalls` — installs the handler, fires 32 `LOG_LIMITED_INFO` in a tight loop (6 emits + 26 drops), deregisters before assertions. Verifies:
  - `calls + emitted == 32` (all events accounted for, either emitted or handler-observed).
  - `calls > 0` (the handler actually ran).
  - `g_handler_last_total_dropped == calls` (global counter in sync with last handler invocation).
  - `file` points at `rate_limit_test.cpp` (substring match so the path-prefix trim doesn't break the assertion).
  - `line > 0` (source line propagated).
- `RateLimit.DropHandlerNotCalledWhenDeregistered` — interleaved no-handler / handler / no-handler runs; asserts handler call count stays 0 before install, grows during install, and stays constant after `SetRateLimitDropHandler(nullptr)`.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **ABI grew one slot in `RateLimiter`.** The ctor signature changed, so any out-of-tree caller that directly constructs `impl::RateLimiter` (there are none in-tree outside the macro) will not compile until they pass file/line. The macro-based public path is unaffected.
- **No per-thread handler.** A single global function-pointer slot; sufficient for push-to-metrics scenarios (Prometheus counter / StatsD), not for scenarios that want different dispatch per thread. Deferred — add `void*` context if the need surfaces.
- **No rate-limit on the handler itself.** If a site drops 1M records in a tight loop, the handler is invoked 1M times. That's the caller's problem (dedup or batch inside the handler if costly).

### Test coverage gaps (addressed)
- Handler fires on drops: covered.
- Handler pointer respects deregistration: covered.

### Deferred to BACKLOG
- `void* user_ctx` parameter on the handler (matches the tracing hook's pattern). Would need one more atomic slot; skipped for now since the global-function-pointer shape covers the common "push to metrics system" use case.
- Per-thread / per-logger handler dispatch, if ever needed.

## Acceptance: ✅ commit + advance
