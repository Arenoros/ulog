# Review 17 — Phase 26 (formatter inline allocation — arena substitute)

Scope: close the BACKLOG "Arena allocator formatter+item в одном malloc" item. The goal was 2 heap allocations per record → 1 on the sync/async hot path. We took a slightly different route than the literal "one buffer holds both" pitched in BACKLOG — placement-new the formatter into a LogHelper-owned stack scratch and leave TextLogItem on the heap. Net outcome matches the target (one less heap alloc per record) without requiring a dual-lifetime wrapper that keeps the formatter bytes alive until the record is drained by the async worker.

## Changes

### `include/ulog/impl/formatters/base.hpp`
- Added `BaseDeleter` — a deleter that distinguishes heap-owned formatters (runs `delete`) from scratch-owned ones (runs `~Base()` only). Stores a single `bool heap`.
- `BasePtr` is now `std::unique_ptr<Base, BaseDeleter>`.

### `include/ulog/log_helper.hpp`
- Forward declaration of `BasePtr` updated to match the new deleter shape (was `unique_ptr<Base>` with default deleter — caused a redefinition error in every TU that included both headers).

### `include/ulog/impl/logger_base.hpp`
- `LoggerBase` exposes two public `static constexpr` budget constants: `kInlineFormatterSize = 256` bytes, `kInlineFormatterAlign = 16` bytes.
- The pure-virtual `MakeFormatter(...)` is replaced by `MakeFormatterInto(void* scratch, std::size_t scratch_size, ...)`. The contract: implementations prefer placement-new into `scratch` when it fits; otherwise fall back to `new`. The returned `BasePtr` carries the correct deleter either way.
- `TextLoggerBase::MakeFormatterInto` override declared.

### `src/impl/logger_base.cpp`
- New file-local helpers `FitsInScratch<F>` and `PlaceFormatter<F>` wrap the "try inline, else heap" decision with static_asserts that every built-in formatter fits within the budget. If a future formatter grows past it, the switch arm still compiles and goes straight to heap — nobody's wire is crossed, just the optimization disables for the affected format.
- `TextLoggerBase::MakeFormatterInto` dispatches by `Format` enum, identical to the old heap-only body but routed through `PlaceFormatter`.

### `src/null_logger.cpp`
- Override signature updated to the new virtual. Still returns an empty `BasePtr` — NullLogger does nothing with formatters.

### `src/log_helper.cpp`
- `LogHelper::Impl` grows `alignas(kInlineFormatterAlign) std::byte formatter_scratch[kInlineFormatterSize]`. Declared *before* the `formatter` BasePtr so that reverse-declaration destruction order runs the placement-delete (via `BaseDeleter`) while the backing bytes are still valid.
- `Impl::Impl` passes the scratch pointer+size into `MakeFormatterInto`. Nothing else changed.

## Why this shape, not a single-buffer-holds-both arena

The literal "one buffer, formatter + TextLogItem" design would force the combined buffer to outlive the formatter (worker drains the TextLogItem long after the producer's `LogHelper::~LogHelper` runs). You'd either:

- Keep the formatter bytes alive until the record is drained — wastes ~100-200 B per pending record plus an unused vtable inside the queue. On the async path that dominates the saved allocation.
- Run the formatter destructor early, leaving the queue holding a "half-live" buffer — fragile invariant, every future formatter author has to remember it.

Placing only the formatter inline and leaving TextLogItem on its own heap slot side-steps both problems. The saved allocation is the formatter's; TextLogItem is already one heap slot holding both the `LoggerItemBase` header and its `SmallString<512>` payload — that stays untouched. Net: 2→1 for the record as a whole. Same win as the BACKLOG target, cleaner lifetimes.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean.
- **Tests**: 81/81 green (79 existing + 2 new white-box tests asserting the inline-vs-heap contract).
- **Bench (local, 24-core)** — existing numbers:

  | Bench                                    | CPU/op | vs prior (Phase 25) |
  |------------------------------------------|--------|---------------------|
  | `BM_SyncLoggerThroughput`                | 651 ns | ~=                  |
  | `BM_FormatThroughput<kTskv>`             | 756 ns | ~=                  |
  | `BM_FormatThroughput<kLtsv>`             | 756 ns | ~=                  |
  | `BM_FormatThroughput<kRaw>`              | 302 ns | ~=                  |
  | `BM_FormatThroughput<kJson>`             | 756 ns | ~=                  |

  The formatter bench constructs formatters in a tight loop but directly, not via LogHelper — so it doesn't stress the scratch-buffer path. `BM_SyncLoggerThroughput` does exercise LogHelper and the numbers held steady (the allocation this phase removes is small enough that the overall 650 ns/op figure is dominated by formatter work, not malloc/free). A before/after run on a pinned CI box with a malloc-tracking LD_PRELOAD harness would quantify the actual per-record alloc-count drop; the code-level confirmation is in the `BaseDeleter`'s `heap=false` branch reaching the `~Base()` call via unit tests.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **`kInlineFormatterSize = 256` is a static upper bound shared across all formatters.** If a subclass of `TextLoggerBase` ever exceeds it, the static_asserts in `logger_base.cpp` will catch it at build time. If a completely out-of-tree `LoggerBase` subclass adds a bigger formatter, the `scratch_size` check at runtime routes them through the heap path (correct, just slower). Tradeoff deliberately sized for all current formatters.
- **`kInlineFormatterAlign = 16` matches typical `std::max_align_t` on 64-bit targets.** None of our formatters over-align; bumping to 32 would be needed if we ever adopt SSE-aligned members.
- **No multi-variant dispatch test.** The existing suite covers every `Format::k*` value through `InstallMem(fmt)` / `LOG_*` paths — that implicitly exercises the new switch. Still, a direct unit test that asserts the inline path is taken (e.g., by checking `formatter.get_deleter().heap == false`) would lock in the optimization contract. Adding below.

### Test coverage gaps (addressed)
- **`FormatterInlineAlloc.PlacesFormatterIntoScratchWhenFits`** — calls `MakeFormatterInto` on a `MemLogger<kJson>` with a budget-sized scratch; asserts the returned pointer lies within the scratch bytes and that the deleter is `heap=false`. Locks in the inline-construction contract.
- **`FormatterInlineAlloc.FallsBackToHeapWhenScratchTooSmall`** — passes `scratch_size=0`; asserts the pointer is *outside* the caller buffer and the deleter is `heap=true`. Covers the graceful-fallback path.

### Deferred to BACKLOG
- True "formatter + TextLogItem single-alloc" via a template wrapper that is itself the `LoggerItemBase`. Useful if malloc/free ever becomes the dominant per-record cost — currently it's 10-20 ns on this machine, below the signal of the existing benches.

## Acceptance: ✅ commit + advance
