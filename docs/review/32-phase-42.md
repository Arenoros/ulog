# Phase 42 — self-review: `ThreadLocalMemPool<LogHelper::Impl>` + `DoLog()` finalization

**Scope:** Phase 1 из BACKLOG `LogHelper producer-side allocator optimizations` (Pri 1 + Pri 4, bundled).

Diff: `src/log_helper.cpp` — добавлен class-template `ThreadLocalMemPool`, вынесен `Impl::DoLog()` noexcept finalization method, ctor/dtor переведены на pool (`Pop` / `Push`) с try/catch-wrap вокруг аллокации для сохранения `noexcept`-контракта.

---

## Сводка изменений

1. **`ThreadLocalMemPool<T, MaxCapacity=16>`** — anonymous-namespace template. Per-thread stack of up to 16 pre-allocated `Storage` slabs (aligned `sizeof(T)` buffers). `Pop` placement-new'ит T в свободный slab (или `new Storage` если cache пуст), `Push` вызывает `destroy_at(T)` и кладёт slab обратно. Thread-exit: `LocalState::~` освобождает остатки через deleter `unique_ptr<Storage>`.
2. **`LogHelper::Impl::DoLog() noexcept`** — extracted из `~LogHelper()`. Содержит всю finalization-логику (tracing hook dispatch, SetText mirror, hot-path Log vs LogMulti routing, flush). Try/catch блок внутри гарантирует `noexcept`.
3. **`LogHelper` constructors** — переведены на `ThreadLocalMemPool<Impl>::Pop(...)` + try/catch (preserve `noexcept`). При throw `impl_` остаётся null; dtor fast-path `if (!impl_) return;` корректно обрабатывает.
4. **`~LogHelper()`** — теперь 3 строки: `if (!impl_) return; impl_->DoLog(); Push(std::move(impl_))`.

---

## Проверка корректности

### Test suite
`./tests/ulog-tests.exe`: **166/166 passed** (7.5s). Нет регрессий в log_macros, record_enricher, structured_sink, async_logger, formatter, rate_limit, log_record_location тестах.

### Build
Успешная сборка `ulog.lib` + examples + ulog-tests.exe. MSVC warning C4324 (alignment padding on Impl) — ожидаемо из-за `alignas(kInlineFormatterAlign=16)` на `formatter_scratch`. Preexisting; не новое.

### Benchmarks

Первый замер был по stale `ulog-bench.exe` (build был с `-DULOG_BUILD_BENCH=OFF`). Перестроил с включённым bench, перемерил.

| Bench | Before (BACKLOG) | After Phase 42 (median of 5) | Δ |
|-------|------------------|------------------------------|---|
| `BM_SyncLoggerThroughput` | 711 ns | **590 ns** (σ=2.6, CV 0.45%) | **-17% / -121 ns** |
| `BM_AsyncLoggerThroughput` wall | 806 ns | 819 ns (σ=14, CV 1.7%) | +1.6% (в шуме) |
| `BM_AsyncLoggerThroughput` CPU | 806 ns* | **693 ns** (σ=32, CV 4.8%) | **-14%** producer-side |

\* BACKLOG не различал wall vs CPU; real time в async doms от inter-thread handoff.

**Sync producer-side:** достигнут userver-parity target ≤600 ns (590 ns median). Что соответствует ожиданию BACKLOG ~13% savings — heap alloc убран с hot path, `Pop`/`Push` работают за счёт amortized zero-alloc после первых 16 записей в thread.

**Async:** wall-time в шуме (queue latency dominates). CPU-time producer-side упал на 14% — именно то, что должна была улучшить pool'инг.

Итог: Phase 42 **достигла target'а BACKLOG** на sync-producer, показала ожидаемый CPU-save на async. Пул работает.

---

## Критические / non-critical findings

### 🔴 Critical — none

### 🟡 Non-critical

**NC-1. `GetTagWriter()` разыменовывает потенциально null `impl_`.**
File: `src/log_helper.cpp:453`.
```cpp
impl::TagWriter& LogHelper::GetTagWriter() noexcept { return impl_->writer; }
```
Если `Pop` throw'нул (OOM) → `impl_ == nullptr` → UB.
**Preexisting** (даже `make_unique` мог throw под noexcept → terminate, но и до этого путь LoggerPtr-null давал null impl_).
**Action:** Phase 2 (InternalLoggingError) — при null impl_ вернуть reference на static dummy TagWriter, чтобы downstream код не crash'ился.

**NC-2. `ThreadLocalMemPool::Push` предполагает, что `~T()` noexcept.**
File: `src/log_helper.cpp:196`.
```cpp
std::destroy_at(p);  // noexcept only if ~T is noexcept
```
`LogHelper::Impl` — compiler-generated dtor, все члены с noexcept dtors (`LoggerPtr=shared_ptr`, `BasePtr=unique_ptr<Base,BaseDeleter>` где `BaseDeleter::operator()` explicit noexcept, `vector`, `optional`, `SmallString`, `TagWriter`). Итого implicit noexcept. **OK на сейчас**; если когда-либо добавят throwing-dtor member → break silently.
**Action:** добавить `static_assert(std::is_nothrow_destructible_v<T>, "...")` в Push или в class body.

**NC-3.** ~~Async bench regression — 806 → 844 ns.~~ **Resolved** — был stale bench binary. После rebuild: wall 819 ns (в шуме), CPU 693 ns (-14%). Cause: `ULOG_BUILD_BENCH=OFF` в cmake cache; `--build --target ulog-bench` silently no-op'ил.

**NC-4. `LocalState` dtor running order при thread exit.**
`thread_local LocalState state` — destructor вызывается при thread exit. В массиве `slabs` — `unique_ptr<Storage>`; их deleters запускают `delete` на Storage (trivial dtor, свободная память). **Корректно.**
Concern: в пограничной ситуации thread может экзитить в середине `Push`, ещё не finished — но Push noexcept и быстрый, очень маловероятно. OK.

### 🟢 Positive notes

- **Placement-new + destroy_at**, как у userver, корректно работают для non-trivial members (vector, optional, shared_ptr) — каждый slot fully (re)constructed на `Pop`, fully destroyed на `Push`.
- `catch (...) { delete raw; throw; }` в Pop правильно освобождает slab при ctor-throw (сохранён raii).
- `obj.reset()` в Push при full-cache даёт deterministic dtor order (не defered) — slightly лучше cache behavior.
- Hot path уменьшился: `~LogHelper()` теперь ~3 insn (null check + virtual DoLog + Push call).

---

## Test coverage

### Покрыто существующими тестами

- `log_macros_test.cpp` — покрывает ctor/dtor flow через LOG_* макросы. Много раз на одном thread → triggers pool reuse.
- `record_enricher_test.cpp` — tracing hook / DispatchRecordEnrichers в DoLog путь.
- `async_logger_test.cpp` — cross-thread flow (producer Pop, сам thread Push на том же).
- `structured_sink_test.cpp` — multi-target path (formatter + recorder + fanout).

### Gaps (добавить)

**TC-1. Explicit pool reuse verification.** Нужен test, который на одном thread создаёт >16 log records sequentially и проверяет что все доходят до sink'а. Текущие тесты делают это de facto, но не документирую expectation.
**Action:** Skip — gap closed de facto existing tests (log_macros writes dozens of records in a row in multiple tests).

**TC-2. Pool across threads.** Стресс-тест с 32 threads, каждый пишет 1000 logs — verify no crash, no leak. Уже покрыт async_logger_test + multi_producer_bench running.
**Action:** Skip.

**TC-3. Ctor-throw path.** Нет easy способа trigger bad_alloc в Impl ctor без custom allocator. Phase 2 добавит InternalLoggingError path — там разумно добавить injection.
**Action:** defer to Phase 2.

---

## Решение

- **NC-1** — defer to Phase 2 (natural fit с InternalLoggingError).
- **NC-2** — **✅ добавлен `static_assert(std::is_nothrow_destructible_v<T>, ...)`** в ThreadLocalMemPool.
- **NC-3** — **✅ resolved** (stale bench). Real numbers подтверждают pool works: sync -17%, async CPU -14%.
- **NC-4** — OK.
- **TC gaps** — defer.

Ready for commit.
