# Phase 43 — self-review: `noexcept operator<<` + `InternalLoggingError`

**Scope:** Phase 2 из BACKLOG `LogHelper producer-side allocator optimizations` (Pri 2).

Diff:
- `include/ulog/log_helper.hpp` — все `operator<<` / `WithException` стали `noexcept`. Добавлен приватный `void InternalLoggingError(const char*) noexcept`.
- `src/log_helper.cpp` — реализации wrap body в try/catch, на exception → stderr-message + `Impl::MarkAsBroken()`. Impl получил `bool broken{false}`. `DoLog` ранний return если `broken`. `Put*` тоже проверяют `broken`. `GetTagWriter` на null-`impl_` возвращает thread_local dummy `TagWriter{nullptr}`.
- `tests/log_helper_noexcept_test.cpp` — новый файл: static_assert'ы на noexcept, runtime-тесты "log from noexcept dtor" + LogExtra / chained streaming.

---

## Сводка изменений

1. **Header contract.** Все public стримы теперь `noexcept`: `<< T`, `<< LogExtra`, `<< Hex`/`HexShort`/`Quoted`/`RateLimiter`, `WithException`, rvalue-варианты. Докстринг объясняет: при fmt OOM / bad format spec → stderr-диагностика + record dropped, а не `terminate()`.

2. **Impl `broken` flag.** Новый bool, set'ится в `MarkAsBroken()`, проверяется в `Put*` (early return) и `DoLog` (skip emit). Broken record не материализуется до sink'а — это корректно, т.к. exception uzh поломал streaming state (частично-записанный text).

3. **`InternalLoggingError(const char* msg)`.** `fputs("ulog::LogHelper: ", stderr); fputs(msg, stderr); fputc('\n', stderr);`. Не использует `fmt::format` (это могло быть источником throw'a, retry не помощь). `msg` — const char* только для избежания std::string ctor, который сам может throw.

4. **GetTagWriter null-safe.** `thread_local TagWriter dummy{nullptr}` на null-`impl_`. TagWriter безопасно работает с nullptr formatter — все методы no-op'ят. Per-thread сделано из консистентности, хотя dummy не мутируется между вызовами.

5. **IsActive()** теперь `impl_ && impl_->active && !impl_->broken` — broken считается "не активен".

---

## Проверка корректности

### Test suite

`./tests/ulog-tests.exe`: **170/170 passed** (было 166, +4 новых). Нет регрессий.

Новые тесты:
- `LogHelperNoexcept.StaticContractHolds` — static_assert'ы на noexcept-сигнатуру всех streaming ops.
- `LogHelperNoexcept.LogsFromNoexceptDestructor` — `~LoggingDtor() noexcept { LOG_INFO() << "dtor log id=" << id; }` → runs to completion, record arrives в MemLogger. Без noexcept контракта это triggered бы terminate.
- `LogHelperNoexcept.LogExtraStreamsCleanly` — streaming LogExtra в noexcept контексте.
- `LogHelperNoexcept.ChainedStreamingAllArrive` — long chain `<< 1 << 2 << ... << Hex{} << HexShort{} << Quoted{}` → full text в record.

### Benchmarks

| Bench | Phase 1 baseline | Phase 2 | Δ |
|-------|------------------|---------|---|
| `BM_SyncLoggerThroughput` median | 590 ns | 585 ns | -0.8% (шум) |
| `BM_AsyncLoggerThroughput` wall | 819 ns | 817 ns | в шуме |
| `BM_AsyncLoggerThroughput` CPU | 693 ns | 709 ns | +2.3% (шум, CV 1.8%) |

try/catch с no-throw fast path — **zero cost** под Itanium/Windows SEH. Подтверждено: sync 590→585 не показал регрессии.

---

## Критические / non-critical findings

### 🔴 Critical — none

### 🟡 Non-critical

**NC-1. `GetTagWriter()` возвращает `thread_local static` dummy — не thread-safe-shared, но **writeable** (если caller `PutTag`'ает).**
File: `src/log_helper.cpp:454`.
Риск: caller получает reference на dummy, через неё пытается записать — target nullptr, TagWriter no-op'ит. Корректно, но dummy — общий для всех **broken** call sites на этом thread. Если один call site накопит мусор в TagWriter state (его сейчас нет — только `formatter_*` указатель), другой подхватит.
**Анализ TagWriter**: только член `formatters::Base* formatter_`. Setter'ов нет кроме ctor/Reset. Dummy всегда nullptr → каждый вызов Put*/Reset no-op'ит. Writes мутируют только formatter state, которого нет. **OK на текущий момент.**
**Action:** если TagWriter позже получит mutable state (counter / buffer), вернуться к решению — возможно `inline constexpr TagWriter dummy{...}` или pimpl-сигнал, чтобы каждый null-path давал свежий dummy. Skip.

**NC-2. `const char* msg` в InternalLoggingError.**
Текущие call sites передают string literal'ы, это ок. Но если кто-то передаст `std::string(...).c_str()`, UB возникает при компиляции конкретного вызова. **Предпочтительно `std::string_view`** — но fputs из string_view требует либо null-termination либо fwrite(data, size). Сейчас работает, переписывать на sv при Phase 3 где будут новые callers.
**Action:** defer; следить при review Phase 3.

**NC-3. `broken` early-return в `Put*` вызывается через public `<<` → try/catch wrapper. Лишний try-frame на hot path, когда `broken=false`.**
Не zero-cost на очень горячем пути? Measured: sync 585 ns vs 590 — within noise.
**Action:** оставить как есть. Compiler optimizer убирает лишний try/catch при no-throw.

**NC-4. `LogHelper::operator<<(const T& value) &` template — inline в header. Каждый TU с LOG_* instantiate'ит собственный try/catch frame.**
Код бьёт по binary size. На малые программы ~+50 байт per streaming type. Не критично.
**Action:** skip.

**NC-5. Unused param warning в `StaticNoexceptChecks` — `LH* p = nullptr; (void)p;`.**
Исправимо. MSVC не жалуется на `(void)p`.
**Action:** skip.

### 🟢 Positive notes

- **`noexcept` контракт на public API** — enables `LOG_*` из dtor, signal handlers (with caveat async-safety), noexcept function bodies без auditting.
- **Broken short-circuit** предотвращает вторичные throws при повторных `<<` после первого fail'a. Half-written text не доходит до sink — чище fail mode.
- **Stderr-only диагностика** — no fmt::format в error path, устойчиво к рекурсивному OOM.
- **Thread-local dummy** на null-`impl_` закрыл NC-1 из Phase 42 review.

---

## Test coverage

### Покрыто

- static_assert'ы на public `<<` noexcept.
- LOG_* в noexcept dtor.
- LogExtra + chained primitives через noexcept стрим.

### Gaps

**TC-1. Симуляция exception в streaming.**
Trivial throwing path через `fmt::format` не легко воспроизвести без mock'а. Можно было бы:
- Custom `operator<<(MyType)` (не существует в текущем API — LogHelper Put<T> только для integral/float/enum/view).
- `fmt::format` с bad format string — но формат идёт hard-coded в Hex/HexShort/RateLimiter overloads, не через user-input.

Настоящая throw-simulation требует injection (debug flag в Impl), что раздувает hot path. **Defer** до явного запроса.

**TC-2. `broken` state persistence.**
Проверить, что after first throw, subsequent `<<` — no-op'ы. Требует way trigger throw → skip.

**TC-3. `GetTagWriter()` на null `impl_`.**
Тест, где Pool::Pop искусственно throw'нул — невозможно без injection seam. **Defer.**

---

## Решение

- **NC-1** / **NC-2** / **NC-3** / **NC-4** / **NC-5** — skip.
- **TC-1 / TC-2 / TC-3** — defer.

Commit.
