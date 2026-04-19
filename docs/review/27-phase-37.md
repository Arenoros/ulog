# Review 27 — Phase 37 (`LFMT_*` + `LPRINT_*` macro families)

Scope: add two new macro families that let callers pass a format string + arguments instead of streaming values piece by piece. Motivated by migration from legacy `printf`-style logs and from bare `fmt::format`-then-stream patterns in existing code.

## Changes

### Two new families, same level ladder

- **`LFMT_*`** — fmt-braces format. `LFMT_INFO("user={} id={}", name, id)`.
- **`LPRINT_*`** — printf conversion specifiers. `LPRINT_WARNING("pct=%5.2f%%", pct)`.

Both follow the existing `LOG_*` layout:
- Per-level short (`LFMT_TRACE`, `LFMT_DEBUG`, ..., `LFMT_CRITICAL`) and long (`ULOG_LFMT_TRACE`, ...) aliases.
- `*_TO(logger, ...)` variants that log to an explicit logger instead of the default.
- All short forms are gated on `ULOG_NO_SHORT_MACROS` exactly like `LOG_*`.
- All go through `ULOG_IMPL_LOGS_<LEVEL>_ERASER` so `-DULOG_ERASE_LOG_WITH_LEVEL=N` compile-erasure applies uniformly.

### Macro shape

```cpp
#define ULOG_LFMT_TO(logger, lvl, ...) \
    ULOG_LOG_TO(logger, lvl) << ::fmt::format(__VA_ARGS__)
#define ULOG_LPRINT_TO(logger, lvl, ...) \
    ULOG_LOG_TO(logger, lvl) << ::fmt::sprintf(__VA_ARGS__)
```

Both forward to the existing `ULOG_LOG_TO` expansion. `ULOG_LOG_TO` expands to an `if (ShouldNotLog) {} else for(...) LogHelper(...)`; the `<< fmt::format(...)` or `<< fmt::sprintf(...)` hangs off the `for` body. **The format call sits on the else branch** — if the level is filtered, neither `fmt::format` nor `fmt::sprintf` runs. Locked in by the `LFMTSkipsFormatWhenLevelFiltered` test: a side-effecting arg is confirmed NOT evaluated on a dropped record.

### Include surface

`include/ulog/log.hpp` now pulls in `<fmt/format.h>` and `<fmt/printf.h>` directly. fmt was already a PUBLIC link from ulog (via log_helper), so no new Conan dep or consumer-visible change. `fmt/printf.h` adds a handful of KB of templates.

### Tests (`tests/log_macros_test.cpp`, +4)

- `LFMTDeliversFormattedPayload` — `LFMT_INFO("user={} id={}", "alice", 42)` / `LFMT_ERROR("delta={:.2f}", 3.14159)` — asserts formatted text lands in the TSKV record.
- `LPRINTDeliversPrintfPayload` — `LPRINT_INFO("code=%d path=%s", 7, "/api/x")` / `LPRINT_WARNING("pct=%5.2f%%", 99.9)` — same, printf side.
- `LFMTSkipsFormatWhenLevelFiltered` — MemLogger at level `kError`; `LFMT_INFO` with a side-effect arg must not evaluate the arg (counter stays 0). `LFMT_ERROR` with the same arg bumps the counter. Locks the short-circuit contract.
- `LFMTAndLPRINTToExplicitLogger` — `_TO` variants deliver to the passed logger, bypassing the default slot.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean.
- **Tests**: 108/108 green (104 previous + 4 new).

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **No `LFMT_LIMITED_*` / `LPRINT_LIMITED_*` rate-limited variants.** Easy to add by layering the same `ULOG_IMPL_LOGS_<LEVEL>_ERASER(ULOG_LFMT_LIMITED_TO, ...)` path. Deferred — the streaming `LOG_LIMITED_*` family can be combined with `fmt::format` manually today (`LOG_LIMITED_INFO() << fmt::format(...)`) and the explicit variant adds 12 macros without a strong caller ask.
- **No `FMT_STRING` / compile-time format checking.** fmt's consteval-checked format-string path is available in fmt 12 via `FMT_STRING("…")` but forcing it would break the variadic-args ergonomics we just gained. Callers who want the check can pass `FMT_STRING(...)` directly inside the macro invocation (it still works; `fmt::format(FMT_STRING("x={}"), v)` compiles).
- **`fmt::sprintf` is slower than `fmt::format`.** printf's positional-conversion grammar doesn't optimize as well; for hot paths the streaming form still wins. Documented in the macro comment; only use `LPRINT_*` for legacy migration, not greenfield code.
- **`LFMT_INFO("literal without args")` works but does one unnecessary `fmt::format` call.** Callers who don't need formatting should use `LOG_INFO() << "literal"` — marginally cheaper.

### Test coverage
- Payload delivery: covered (LFMT + LPRINT).
- Short-circuit on filtered level: covered (LFMT — applies to LPRINT identically via same macro shape).
- `_TO` variants: covered.
- Not covered: `_TO` with a `LoggerPtr` (only `LoggerRef` asserted). Trivial to add; same LogHelper path.
- Not covered: compile-erase via `ULOG_ERASE_LOG_WITH_LEVEL`. Existing binary-grep BACKLOG item would cover both `LOG_*` and `LFMT_*` once implemented.

### Deferred to BACKLOG (notional)
- `LFMT_LIMITED_*` / `LPRINT_LIMITED_*` variants if callers ask.
- Example program that demonstrates migration from `printf(...); … ; log_old(...)` to `LPRINT_INFO(...)`.

## Acceptance: ✅ commit + advance
