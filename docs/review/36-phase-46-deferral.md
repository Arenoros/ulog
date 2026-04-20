# Phase 46 — defer: combined Impl+formatter+item arena alloc (Pri 5)

**Scope:** Pri 5 из BACKLOG `LogHelper producer-side allocator optimizations`.

**Решение:** **defer** — не включаем в текущую фазовую серию (42-45).

---

## Почему defer

### Что Pri 5 предлагал

Один heap block для `LogHelper::Impl` + primary `Formatter` + `TextLogItem`. Экономить ~50 ns vs текущих 2 heap alloc'ов.

После Phase 42 (ThreadLocalMemPool<Impl>) структура heap alloc'ов:
- `Impl` — pool reuse, ~0 heap hits на hot path (после первых 16 per-thread records).
- **`TextLogItem` (~540 B)** — создаётся в ctor формттера через `std::make_unique<TextLogItem>()` (`src/impl/formatters/*.hpp`). Один heap alloc per record. Это actual remaining overhead.
- Formatter сам — уже placement-new'ится в scratch внутри Impl (`kInlineFormatterSize=256`), нет heap alloc'а.

Значит real target Pri 5 = TextLogItem pooling / arena.

### Lifetime-barrier для pooling

`TextLogItem` проходит через async queue:
```
producer                 sink thread
  formatter ctor
   → make_unique<Item>   (alloc)
  formatter->Extract()
   → move ptr to queue
                          Log(level, move item)
                          ~unique_ptr<Item>   (dealloc)
```

На async loggere producer и consumer — разные треды. TLS pool на producer-side не может Push-back'ить (`Push` побежит в consumer-thread TLS).

Опции:
1. **MPSC return channel** — cross-thread pool. Consumer thread Push'ает в lock-free очередь, producer pop'ает оттуда + fallback на heap. Нетривиальный lifetime: что если producer thread умер до consumer returning? Race на pool destruction.
2. **Per-sink arena** — sink держит slab, reuse item across records. Требует кастомный deleter на unique_ptr, который не `delete`, а Push. Усложняет async queue entry (dtor не тривиальный).
3. **Thread-local on CONSUMER side** — sink discards item на consumer, если consumer pool full → heap. Asymmetric: producer side не benefit'ится, allocator pressure лишь сдвинут.
4. **Separate arena для sync-only path** — sync logger не queue'ит, item discard'ится inline. Можно inline TextLogItem в Impl's scratch для sync case, падать на heap для async. Branching по runtime (async check) + code duplication.

Все варианты — значимый refactor (queue entry layout, deleter design, cross-thread cleanup semantics).

### Expected payoff

BACKLOG: ~50 ns save after Phase 42. На baseline 580 ns sync → 530 ns. Относительное — 8%.

Benchmark CV сейчас ~1.4% на sync (8 ns stddev). Delta 50 ns detectable, но:
- В production бытовом логировании 50 ns на ~580 ns baseline → пользователь не заметит.
- Complexity / risk / maintenance burden из cross-thread pool — high.
- Phase 42 уже закрыла main parity (≤600 ns userver goal). Remaining — stretch.

### Trade-off

| Вариант | Perf gain | Lifetime complexity | Maintenance |
|---------|-----------|---------------------|-------------|
| Keep heap alloc (текущее) | baseline | none | trivial |
| Cross-thread MPSC pool | -50 ns | высокая (thread-death races) | средняя |
| Sync-only arena | -50 ns sync, 0 async | средняя (runtime branch) | средняя |
| Per-sink arena | -50 ns если sink done fast | средняя (deleter design) | средняя |

Ни один не оправдывает 8% delta при текущем baseline meet'е parity target'а.

---

## Критерий re-opening

Возврат к Pri 5, если:

- Бенчмарк на load real-world workload показал allocator pressure (mimalloc/jemalloc contention) как bottleneck.
- Появился user-report "log throughput ниже expected на MxN workload".
- Async queue layout меняется по другой причине — можно piggy-back item pooling.

Пока — status quo. `std::make_unique<TextLogItem>()` сохраняется.

---

## BACKLOG update

Отметить Pri 5 как `🟡 DEFERRED — Phase 46`. Сохранить описание для будущего.

## Conclusion for phase series 42-45

Завершено 5 из 6 приоритетов (Pri 1-4, 6). Pri 5 explicitly deferred.

| Priority | Phase | Status | Impact |
|----------|-------|--------|--------|
| 1: ThreadLocalMemPool<Impl> + DoLog | 42 | ✅ | sync 711→590 ns (-17%), async CPU 806→693 ns (-14%) |
| 2: noexcept operator<< + InternalLoggingError | 43 | ✅ | zero perf cost, reliability +++ |
| 3: kSizeLimit = 10000 truncation | 44 | ✅ | runaway protection |
| 4 (DoLog finalization) | 42 (embedded) | ✅ | enables Pri 1 |
| 5: Combined arena alloc | — | 🟡 DEFERRED | ~50 ns estimated, high risk |
| 6: per-logger PrependCommonTags | 45 | ✅ | multi-tenant support |

Main goal: `BM_SyncLoggerThroughput ≤ 600 ns` → **achieved** (581 ns median by Phase 45, ниже userver parity). Producer-side alloc overhead на hot path = 1× TextLogItem heap hit (~540 B) — при TLS allocator contention <5 ns на mimalloc / ptmalloc под single-threaded workload.
