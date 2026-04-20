# Phase 44 — self-review: `kSizeLimit = 10000` + `IsLimitReached()` truncation

**Scope:** Phase 3 из BACKLOG `LogHelper producer-side allocator optimizations` (Pri 3).

Diff:
- `src/log_helper.cpp` — добавлены `constexpr std::size_t kSizeLimit = 10000` (anon-ns), поля `truncated{false}` в `Impl`, `Impl::GetTextSize()` / `Impl::IsLimitReached()`. `Put(std::string_view)` и `Put(char)` гейтят запись на `text.size() >= kSizeLimit` → ставят `truncated=true`, return. `DoLog` эмитит `writer.PutTag("truncated", "true")` до `SetText`, если `truncated`.
- `include/ulog/log_helper.hpp` — publicly добавлен `bool IsLimitReached() const noexcept`. Docstring сообщает о 10 KB budget.
- `tests/log_helper_truncation_test.cpp` — новый файл: 3 теста на короткое сообщение без tag'а, large-message truncation + upper-bound, `IsLimitReached()` predicate transitions.
- `tests/CMakeLists.txt` — зарегистрирован новый тест.

---

## Сводка изменений

1. **`kSizeLimit` константа.** 10000 байт — parity с userver. В anonymous namespace внутри `src/log_helper.cpp`, не expose'ится через header (внутренняя константа).

2. **`truncated` bool в `Impl`.** Флаг выставляется в `Put(sv)` / `Put(char)` когда текущий размер ≥ cap, и в `InternalLoggingError` тоже (так как sticky). `broken` и `truncated` — ортогональны: broken ⇒ drop, truncated ⇒ emit with tag.

3. **Gate в `Put*`.** Перед `text += sv` проверка `text.size() >= kSizeLimit`. Если сработал cap — `truncated = true; return`. Это гарантирует, что overshoot ≤ one write call. На вход одной chunk (скажем, 1024 байта) это значит в худшем случае text.size() = cap-1 + 1024 ≈ 11023 байта.

4. **`DoLog` — tag before `SetText`.** Формат TSKV (и json, pretty) эмитит tags через `writer.PutTag` ДО `SetText(text_view)`. Поэтому `truncated=true` отображается раньше поля `text=` в записи — test-expectation это учитывает (ищет обе подстроки независимо + measure text length от `text=` до `\n`).

5. **`IsLimitReached()` public.** `!impl_ || impl_->IsLimitReached()` — null-safe. `Impl::IsLimitReached()` = `truncated || text.size() >= kSizeLimit`. Callers могут short-circuit до expensive formatting (stack traces, serialised blobs).

---

## Проверка корректности

### Test suite

`./tests/ulog-tests.exe`: **173/173 passed** (170 + 3 новых). Нет регрессий.

Новые тесты:
- `LogHelperTruncation.ShortMessageNoTruncatedTag` — `LOG_INFO() << "short"` → запись без `truncated=true`. Санити-проверка negative path.
- `LogHelperTruncation.LargeMessageTrimsAndTags` — 15 × 1024 байта chunks → `truncated=true` tag есть, длина поля `text=` в диапазоне `[10000, 11500]`. Upper-bound = cap + один chunk overshoot. Прямая конструкция `ulog::LogHelper helper(*mem, Level::kInfo)` в scope block, т.к. `LOG_INFO` — statement-form macro, не expression; helper — non-movable.
- `LogHelperTruncation.IsLimitReachedPredicate` — три шага: пустой false → 5000 a false → +6000 b true → +5000 c true (no-op, остаётся true). Verifies sticky transition.

### Benchmarks

| Bench | Phase 2 baseline | Phase 3 | Δ |
|-------|------------------|---------|---|
| `BM_SyncLoggerThroughput` median | 585 ns | 596 ns | +1.9% (CV 1.36%, в шуме) |
| `BM_AsyncLoggerThroughput` wall | 817 ns | 818 ns | в шуме |
| `BM_AsyncLoggerThroughput` CPU | 709 ns | 724 ns | +2.1% (CV 4.05%, в шуме) |

Gate — один лишний `size >= cap` check per `Put(sv)` (~1 ns на LOG_INFO с 2-3 put'ами). Noise пересекает delta, эффективно нулевой cost на happy path.

---

## Критические / non-critical findings

### 🔴 Critical — none

### 🟡 Non-critical

**NC-1. `kSizeLimit` hard-coded — нет runtime-конфига.**
File: `src/log_helper.cpp:36`. userver тоже hard-codes = 10000. Если log heavy-consumer'у нужен другой cap (e.g. debugging stream с large structured data), нет способа override. Вариант: `static std::atomic<std::size_t>` + setter. Текущее scope: parity-only, skip.
**Action:** skip; реактивно добавить setter если возникнет запрос.

**NC-2. `Put(char v)` и `Put(sv)` — duplicate gate logic.**
Two identical 4-line blocks. Можно вынести в inline `bool CheckAndGate()` helper. Binary size negligible, readability — marginal.
**Action:** skip. Lift когда появится третий call site.

**NC-3. `truncated=true` printed перед `text=` в TSKV.**
TSKV order: timestamp, level, module, tags (через `PutTag` в DoLog), `text=...`. Consumer, ожидающий `truncated=true` после текста, сломается. Текущие consumers у нас нет, но parity с userver — userver эмитит тот же порядок (tag-then-text). **OK.**
**Action:** skip.

**NC-4. `text.size()` проверяется ДО `+= sv` — предыдущий put мог overshoot.**
Last `+=` не отрезает данные: целый sv добавляется. Overshoot ≤ `sv.size()`. Для chunked streaming от `fmt::format` это типично small (single format result), для `<< std::string(10000, 'x')` — overshoot до 10000. Test bound'ы выставлены с этим запасом (`11500u`). Не проблема функционально, но документировать стоит.
**Action:** skip; docstring уже упоминает "overshoot by at most one chunk".

**NC-5. `PutFormatted(std::string s)` проходит через `Put(std::string_view(s))` — temp'ной std::string доносится полностью, даже если только первые байты полезны до cap'a.**
На сценарии "уже truncated, идёт fmt::format('{}', huge_int)" — `fmt::format` аллоцирует std::string, мы его не юзаем. Optimisation: check `text.size() >= kSizeLimit` ДО `fmt::format`. Trade-off: hot path gain near-zero (format вызывается только там, где мы точно хотим emit), explicit early-return через `IsLimitReached()` на caller side — правильнее.
**Action:** skip. Callers могут `if (h.IsLimitReached()) break;` для expensive formats.

### 🟢 Positive notes

- **Predictable overshoot bound.** Max: cap + one sv.size(). Streaming code может оценить worst-case memory pressure на per-record basis.
- **Sticky `truncated` flag.** После первого trip'a дальнейшие writes — no-op, но `IsLimitReached()` остаётся true. Caller'ы не нужно перечитывать state.
- **Tag routing через writer.** `writer.PutTag` ходит в тот же formatter/structured target, что и остальные tags — нет special-case emission path.
- **`text` field length упирается не ровно в cap.** Это намеренно — резать середину format string сломает UTF-8 / TSKV escaping. Overshoot предсказуемый (≤ one chunk), разделить sv по byte-границе не надо.

---

## Test coverage

### Покрыто

- Short path без tag'а.
- Large-body with truncation + bounds.
- `IsLimitReached()` state machine: false → false → true → true (sticky).

### Gaps

**TC-1. `Put(char)` path отдельно не покрыт.**
Внутри hot path `char` gets через LogHelper::Put(char) от HexShort / тех `<< 'x'` сценариев. Текущие тесты через `operator<< std::string` идут только через `Put(sv)`. Character-level truncation пока untested.
**Action:** defer. Низкий риск — тот же gate-код, unit-wise identical.

**TC-2. Truncation не проверена на JSON/Pretty форматах.**
Только TSKV. Но `writer.PutTag` — formatter-agnostic, остальные форматы должны эмитить tag так же. Если JSON формат хранит tags в иерархии, order может отличаться.
**Action:** defer; добавить параметризованный тест на format-list при Phase 5.

**TC-3. LogExtra + truncated interaction.**
`LogExtra::operator<<` идёт через `writer.PutLogExtra`, не через `Put*`. LogExtra не закрывается под cap'ом — tags могут дойти до sink'а сверх 10 KB. Это намеренно (LogExtra — structured, не text body), но не задокументировано. userver ведёт себя так же.
**Action:** skip.

---

## Решение

- **NC-1 / NC-2 / NC-3 / NC-4 / NC-5** — skip.
- **TC-1 / TC-2 / TC-3** — defer.

Commit.
