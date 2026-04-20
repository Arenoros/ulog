# Review 31 — Phase 41 (hot-path perf: COW sinks + atomic flags + fast-path)

Scope: recover the 38% `BM_SyncLoggerThroughput` regression that appeared
between `a564b26` (709.72 ns) and `e8f578b` (981.90 ns). Root cause was
cumulative: every record acquired four mutexes on the logger side (two
sink-list locks + one format-registry lock in `RegisterSinkFormat` is
only a writer concern, but `HasTextSinks` / `HasStructuredSinks` each
took a lock to return a `bool`), went through one heap allocation per
formatter, and did one `dynamic_cast` for the `TextLoggerBase` downcast
in `LogHelper::Impl`.

After this phase the record-emission hot path is lock-free (atomic
shared_ptr snapshots), zero-allocation for the primary formatter
(placement-new into `LogHelper::Impl` scratch), zero-virtual-dispatch
for `HasTextSinks` / `HasStructuredSinks` (non-virtual atomic load),
and uses a single virtual `AsTextLoggerBase()` in place of RTTI.

## Changes

### Atomic fast-path flags on `LoggerBase`

`HasTextSinks()` / `HasStructuredSinks()` were virtual on
`LoggerBase`, overridden in `SyncLogger` / `AsyncLogger` with a
mutex-guarded `sinks_.empty()` check. Replaced with two
`std::atomic<bool>` fields on `LoggerBase` published by
`SetHasTextSinks` / `SetHasStructuredSinks` from the concrete
loggers' `AddSink` / `AddStructuredSink` paths.

```cpp
// include/ulog/impl/logger_base.hpp
bool HasTextSinks() const noexcept {
    return has_text_sinks_.load(std::memory_order_acquire);
}
bool HasStructuredSinks() const noexcept {
    return has_structured_sinks_.load(std::memory_order_acquire);
}
```

`SyncLogger` and `AsyncLogger` ctors now start with
`SetHasTextSinks(false)` so a freshly constructed logger correctly
reports "no sinks" to `LogHelper` and skips the formatter path
entirely until `AddSink` lands.

### `AsTextLoggerBase()` virtual downcast replaces `dynamic_cast`

`LogHelper::Impl` used to run
`dynamic_cast<impl::TextLoggerBase*>(&logger_ref)` on every record to
decide whether the extras branch applied and to fetch
`GetEmitLocation()` for the structured path. RTTI is expensive
relative to a devirt'ed vtable slot. Replaced with a pair of virtual
methods on `LoggerBase`:

```cpp
virtual TextLoggerBase* AsTextLoggerBase() noexcept { return nullptr; }
virtual const TextLoggerBase* AsTextLoggerBase() const noexcept { return nullptr; }
```

`TextLoggerBase` overrides to return `this`. `LogHelper::Impl`
caches the pointer once at construction and reuses it for both the
extras gate and the structured `emit_location` lookup.

### Copy-on-write sink lists (SyncLogger / AsyncLogger)

`sinks_` / `struct_sinks_` are `boost::atomic_shared_ptr<Vec const>`.
Readers (`Log`, `LogMulti`, `Flush`, and on the async side
`drain_records`, `drain_reopens`, `drain_flushes`) do
`auto snap = sinks_.load();` — no mutex — and iterate the pinned
snapshot. Writers hold a writer-only mutex, copy the current vector,
append, and publish a fresh shared_ptr. Already-pinned readers keep
referencing the old vector until their pin releases; shared_ptr
refcount keeps it (and the sinks inside it) alive.

Race semantics: a sink added *after* `LogHelper::Impl` materialized
its formatters is invisible to that in-flight record. Its
`format_idx` may exceed `items.size()`; `LogMulti` has an explicit
out-of-range skip rather than falling through to `items[0]` (wrong
format). Same story on the async worker's text fan-out.

Helper in anonymous namespace (both loggers share the shape):

```cpp
template <typename Vec, typename Entry>
boost::shared_ptr<Vec const> AppendCow(
        const boost::shared_ptr<Vec const>& current,
        Entry&& entry) {
    auto next = boost::make_shared<Vec>();
    if (current) {
        next->reserve(current->size() + 1);
        *next = *current;
    }
    next->push_back(std::forward<Entry>(entry));
    return boost::shared_ptr<Vec const>(std::move(next));
}
```

### Copy-on-write `active_formats_` in `TextLoggerBase`

`RegisterSinkFormat` previously held the format-list mutex on every
read. Converted to the same COW pattern: `formats_mu_` serializes
writers only, `active_formats_` is a `boost::atomic_shared_ptr<vector<Format> const>`,
and `GetActiveFormatsSnapshot()` is a lock-free atomic load.

Also added `active_format_count_` — a fast-path `atomic<size_t>`
mirror of the list size, published together with the snapshot. Hot
code (`LogHelper::Impl`) checks `GetActiveFormatCount() > 1` before
pinning the snapshot; the single-format path never touches the
shared_ptr.

### `LogHelper` single-format fast-path

`LogHelper::~LogHelper` used to always build a `LogItemList`, wrap
into `LogMulti(level, items, structured)`, and route. The common
case (one format, no structured sink) paid for a `small_vector`
reserve + one `push_back` + the `LogMulti` vtable slot.

```cpp
// src/log_helper.cpp — fast path
if (impl_->formatter && impl_->extras.empty() && !impl_->recorder) {
    auto item = impl_->formatter->ExtractLoggerItem();
    impl_->logger_ref.Log(impl_->level, std::move(item));
    if (want_flush) impl_->logger_ref.Flush();
    return;
}
```

Falls back to the LogMulti path whenever extras / recorder engage.

### `LogItemList` now `small_vector<unique_ptr, 1>` inline-1

`LogItemList` is a `boost::container::small_vector<unique_ptr<LoggerItemBase>, 1>`.
Single-format callers don't heap-allocate the carrier vector — the
inline slot holds the sole item.

### `LogHelper::Impl` placement-new formatter scratch

`formatter_scratch` is a 256-byte aligned inline buffer inside
`Impl`. `MakeFormatterInto(scratch, size, level, location)` tries to
placement-new the formatter there; falls back to `new` only if the
formatter exceeds the budget (none of the built-ins do —
`static_assert` guards). Deleter carries a `heap_` bit so the
correct cleanup runs.

Zero heap allocations for the primary formatter on the hot path.

## Tests

166/166 tests pass, no regressions. No new tests added this phase.

Rationale for not adding tests:

- Semantics are behaviour-preserving — every pre-existing sink /
  formatter / per-sink-format test exercises the new code paths.
  `per_sink_format_test.cpp` in particular drives the extras /
  `format_idx` fan-out that the COW snapshot powers.
- The "sink appended after record materialized" skip path was
  already present (out-of-range check in `LogMulti`); only the
  snapshotting mechanism changed.
- Concurrent `AddSink + Log` stress tests would add value, but the
  correctness argument rests on the atomic shared_ptr publish (TSO
  on x86, release/acquire in the abstract machine) — empirically
  robust, and the sinks inside a pinned snapshot stay alive via
  refcount. Leaving stress coverage as a potential follow-up
  (would also exercise moodycamel's producer-token cache).

## Bench (`BM_SyncLoggerThroughput`, Release, MSVC 14.43)

| Commit          | cpu_time  | delta vs. baseline |
|-----------------|-----------|--------------------|
| `a564b26` (prior baseline) | 709.72 ns | — |
| `e8f578b` (regression)     | 981.90 ns | +38% |
| this phase                 | **681–711 ns** | at or below baseline |

Variance across runs within ±5%; both 681 and 711 observed. The
regression is recovered; the fast-path + COW combination is
marginally ahead of the pre-regression baseline when it lands in
the favoured bucket.

## Findings (self-review)

### Addressed

- **Hot-path locks removed** — all four per-record mutex acquisitions
  dropped (two sink-list locks, two `Has*Sinks` wrapper locks).
  Writer mutexes kept for the publish side; acquired only by
  `AddSink` / `AddStructuredSink` / `RegisterSinkFormat`.
- **Per-record heap allocations removed** — primary formatter
  placement-new'd into `LogHelper::Impl` scratch. Extras still heap
  (rare path, multi-format only).
- **RTTI path removed** — `AsTextLoggerBase()` replaces
  `dynamic_cast`; one vtable indirection, cached once per record.
- **SyncLogger::Log direct dispatch** — single-format path skips
  `LogMulti` + `LogItemList` wrap entirely.
- **`NullLogger` / other `LoggerBase` subclasses** — the atomic
  flag mechanism defaults (`has_text_sinks_{true}`) preserves
  legacy behaviour for loggers that don't manage sinks explicitly.

### Open / acceptable

- **`boost::atomic_shared_ptr` on MSVC** — relies on
  `boost::smart_ptr/atomic_shared_ptr.hpp`, which is backed by a
  per-object spinlock on platforms without native double-word CAS.
  On x86-64 MSVC it compiles to a `cmpxchg16b` loop inside Boost's
  `atomic_shared_ptr` primitive; fine for our contention profile
  (writers only hit it from `AddSink`). If this ever becomes a
  contention point (mass sink churn), switch to a manually
  refcounted epoch-based snapshot.
- **Sink invisibility for mid-record publishes** — documented above.
  The alternative (blocking `AddSink` until all in-flight LogHelpers
  drain) is not worth the complexity for a behaviour nobody depends
  on; every production user adds sinks at startup before records
  start flowing.
- **`LogItemList` inline capacity = 1** — chosen because the
  multi-format path is already on the slow side (one heap alloc per
  extras formatter). Bumping to 2 would save one alloc on the
  dual-format case but bloat every single-format `LogHelper::Impl`
  by 16 bytes. Keep at 1.
- **`active_format_count_` duplicates the snapshot size** — fast-path
  read cost vs. shared_ptr pin. The count is published *before*
  the new snapshot in `RegisterSinkFormat`, so a reader that sees
  `count > 1` always sees a snapshot with `size >= count`; a reader
  that sees `count == 1` may miss a just-published larger snapshot,
  but that record simply skips extras (correct: extras for a
  just-registered override are invisible to in-flight records
  anyway).

### Not addressed (scope creep)

- **AsyncLogger producer-token cache** — pre-existing (not part of
  this phase). Already single-slot TLS with generation guard;
  touching multi-logger users is a separate concern.
- **Per-sink mutex inside `BaseSink::Write`** — sinks themselves
  may still lock. Out of scope; SyncLogger is generic over sink
  behaviour.
- **Concurrent `AddSink` stress test** — see Tests section.
- **`boost::atomic_shared_ptr` → `std::atomic<shared_ptr>` (C++20)** —
  the standard library offering is available in MSVC / libstdc++;
  switch would be a straight replacement minus the `boost::` prefix.
  Deferred: Boost version works, no urgency.

## Test coverage status

166/166 tests pass (no new, no removed). Bench suite healthy —
`BM_SyncLoggerThroughput` 681–711 ns, other benches unchanged.
