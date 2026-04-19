# Review 09 — Phase 19 (AsyncLogger ownership refactor + batch notify)

Scope: eliminate the per-record `std::string` copy on the async producer path, straighten the logger/formatter ownership API, and reduce spurious `cv.notify_all` calls. Performance target was a measurable win on `BM_AsyncLoggerThroughput`; outcome was perf parity with a cleaner code shape — details below.

## Changes

### Ownership refactor

Before — `formatters::Base::ExtractLoggerItem()` returned a reference to a `TextLogItem` inlined into the formatter. Consumers had to call the method while the formatter was still alive. `AsyncLogger::Log` worked around the lifetime by copying `text_item.payload.view()` into a `std::string` inside a new `LogRecord`.

After — ownership transfer:

- `formatters::Base::ExtractLoggerItem()` now returns `std::unique_ptr<LoggerItemBase>` (the formatter relinquishes the item).
- `LoggerBase::Log(Level, std::unique_ptr<LoggerItemBase>)` takes ownership of the item.
- TskvFormatter / LtsvFormatter / RawFormatter / JsonFormatter hold their item as `std::unique_ptr<TextLogItem>` allocated in the ctor; `ExtractLoggerItem` `std::move`s the pointer out.
- `LogHelper::~LogHelper` passes `std::move(item)` into `logger.Log(...)` — no copy on any path.
- Sync/Mem/Null loggers consume and drop the item at end of `Log` scope.
- AsyncLogger moves the unique_ptr into `LogRecord` and across the moodycamel queue. Worker drains, extracts the payload view from the TextLogItem, dispatches to sinks, and drops the unique_ptr.

### TextLogItem inline buffer

`TextLogItem::payload` reduced from `SmallString<4096>` to `SmallString<512>`. Typical log records (timestamp + level + module + text + small tag set) land well under 512 B, so heap-allocating a TextLogItem now costs ~540 B rather than ~4 KB. Records that spill past the inline buffer fall back to SmallString's std::string path (one extra small allocation).

### Batch notify

`AsyncLogger::TryEnqueueRecord` now uses:

```cpp
records.enqueue(std::move(rec));
const auto prev = pending.fetch_add(1, release);
if (prev == 0) cv.notify_one();   // only wake when worker was idle
```

Previously every enqueue called `cv.notify_all`. Under steady-state load the worker is already draining and the notify is wasted. One relaxed atomic load + branch per record replaces the always-notify. The worker-side `WakeForControl` stays (it's called once per drained batch to release producers blocked on capacity).

## Performance

Windows MSVC 14.44, RelWithDebInfo, `--benchmark_min_time=0.2s`:

| Benchmark | Pre-Phase 19 | Post-refactor (SmallString<4096>) | Final (SmallString<512>) |
|---|---|---|---|
| `BM_GetDefaultLoggerPtr` | 10 ns | 11 ns | 10 ns |
| `BM_LogThroughNullDefault` | 10 ns | 11 ns | 11 ns |
| `BM_SyncLoggerThroughput` | 683 ns | 764 ns | **711 ns** |
| `BM_AsyncLoggerThroughput` | 858 ns | 1788 ns | **851 ns** |

The intermediate row documents why `SmallString<512>` matters: with the original 4 KB inline buffer, heap-allocating a TextLogItem per record dominated async cost and the worker's 4 KB `free` kept the producer blocked on capacity. Dropping to 512 B restored producer throughput to the pre-refactor baseline.

**Net perf**: essentially flat. The "no std::string copy" saving is roughly offset by the extra `TextLogItem` allocation that was previously inlined in the formatter. Batch notify removes `cv.notify_all` in the fast path but at 65 k-deep unblocked queue the worker is already draining; gain is marginal under the current benchmark.

## Where the redesign still pays off

- **Real I/O sinks.** When a sink blocks (FileSink fsync, TcpSink send), the producer-side cost dominates the "Log" latency; the new path never allocates a second string and moves the item through a single unique_ptr. Difference would surface on a benchmark with an artificially slow sink — tracked in BACKLOG.
- **Large records.** The old path copied every record into a heap `std::string` of exact size; the new path keeps sub-512 B records entirely inline. Spill cost is identical to pre-refactor for larger records.
- **Memory.** Queue slot cost drops from `sizeof(Level) + sizeof(std::string)` (~24-32 B) to `sizeof(Level) + sizeof(unique_ptr)` (~16 B). Queue block allocation pressure eased.
- **API clarity.** Ownership is now explicit; no more "the formatter must outlive the logger call" dance. New loggers written against the base will not have to guess.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Two heap allocations per log (formatter + TextLogItem)** instead of one (inline). A follow-up could combine them via a single placement-new in a fixed arena. Added to BACKLOG.
- **SmallString<512> may spill more often than <4096> for verbose tag-heavy records.** Real impact is one extra small std::string allocation — still cheaper than the old 4 KB formatter alloc.
- **`BM_LogThroughNullDefault` still routed through the full pipeline up to ShouldLog**, which short-circuits at the `for`-loop predicate. Not measured separately; the pre-existing macro fast-path covers it.
- **Batch notify uses `notify_one`**, assuming a single worker thread. Correct for the current design; if multi-worker async lands later, revisit.

### Deferred to BACKLOG
- Sink-cost sweep bench (discard / file / tcp) — shows where async wins once sink latency dominates.
- Arena-based per-record allocation to collapse formatter + item into one allocation.

## Acceptance: ✅ parity perf, cleaner ownership, commit + advance
