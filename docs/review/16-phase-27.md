# Review 16 — Phase 27 (short-circuit formatter alloc verification)

Scope: confirm the BACKLOG item "Short-circuit formatter alloc when logger rejects level" is redundant and close it without a code change.

## Finding

The hypothesis in `docs/BACKLOG.md` read:

> **Проблема:** макрос уже делает `ShouldNotLog` check — LogHelper конструируется только если логирование прошло фильтр. Этот пункт возможно уже закрыт и остался в review по ошибке.

Verified true against the current macro expansion.

### Evidence

`ULOG_LOG_TO` (include/ulog/log.hpp:186-194):

```cpp
#define ULOG_LOG_TO(logger, lvl, ...)                                     \
    if (auto&& ulog_logger__ = (logger);                                  \
        ULOG_IMPL_DYNAMIC_DEBUG_ENTRY().ShouldNotLog(ulog_logger__, (lvl))) {} \
    else                                                                  \
        for (bool ulog_once__ = true; ulog_once__; ulog_once__ = false)   \
            ::ulog::LogHelper(                                            \
                std::forward<decltype(ulog_logger__)>(ulog_logger__),     \
                (lvl),                                                    \
                ULOG_IMPL_LOG_LOCATION())
```

The `ShouldNotLog(logger, level)` check runs **before** `LogHelper` is constructed. When the logger rejects the level, the `if` branch takes the empty body and the `for`-loop never runs — `LogHelper(...)` never executes, no formatter is allocated, no `TextLogItem` either.

All public logging entry points route through `ULOG_LOG_TO`:
- `ULOG_LOG` / `LOG_INFO` / `LOG_TO` / `LOG_INFO_TO` — all expand to `ULOG_LOG_TO` (log.hpp:198, 244-253, 286).
- `LOG_LIMITED_*` layers a rate-limit check on top but still delegates to `ULOG_LOG_TO` (log.hpp:265).
- `ULOG_IMPL_LOG_TO` (log.hpp:175) exists but is only referenced inside `ULOG_IMPL_ERASE` (log.hpp:204), which wraps it in `for (bool x = false; x; x = false)` — a guaranteed-never-executes dead-code scaffold used by the compile-erase feature.

The only way to reach `LogHelper` construction without the gate is **direct instantiation** (`ulog::LogHelper lh(logger, level, loc)`). That path is intentional — it's how the formatter-idempotency test added in Phase 25 exercises the formatter directly. Direct callers take responsibility for their own gating.

### LogHelper::Impl formatter allocation

For completeness, `src/log_helper.cpp:49-57` shows the formatter is built only when `active=true` is passed. Every macro path uses the 3-arg constructor (defaults `active=true`). The 4-arg `NoLog` overload exists for `LOG_LIMITED_*` and similar opt-outs, which pass `active=false` so `MakeFormatter` is skipped — a separate optimization layer that was already in place.

## Changes

- Delete the entry from `docs/BACKLOG.md` — no code change.

## Results

- **Build**: untouched (no source change).
- **Tests**: 79/79 green from Phase 25, unchanged.

## Findings

### 🔴 Critical / 🟡 Non-critical
- None — phase closes as "already-implemented".

## Acceptance: ✅ mark closed, advance
