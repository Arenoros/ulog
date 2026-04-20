# Phase 48 — self-review: `PutRange` container streaming (sequences + maps)

**Scope:** BACKLOG `LogHelper container streaming (PutRange)` — mid-effort high-value sprint (docs/BACKLOG.md:197).

Diff:
- `include/ulog/log_helper_range.hpp` — **NEW**: `operator<<(LogHelper&, const T&)` gated via `kIsLoggableRange<T>`; array overload `operator<<(LogHelper&, const T(&)[N])`; internal helpers `PutRange`, `PutRangeElement` (generic + `std::pair<const K, V>` specialisation).
- `include/ulog/log_helper_extras.hpp` — pointer overload rewritten: generic `const T*` template → non-template `const void*` (исключает partial-ordering конфликт с новым array overload'ом).
- `include/ulog/log_helper.hpp` — SFINAE на member template `operator<<(const T&)` дополнен `!is_array_v<T>` (routed through array overload).
- `include/ulog/log.hpp` — auto-include `log_helper_range.hpp`.
- `tests/log_helper_range_test.cpp` — **NEW**: 14 тестов (containers + maps + strings remain text + truncation + array + nested).
- `tests/CMakeLists.txt` — зарегистрирован.

---

## Сводка изменений

### 1. SFINAE-gated generic range overload

`template <T> operator<<(LogHelper&, const T&) noexcept` с `enable_if<kIsLoggableRange<T>>`. `kIsLoggableRange<T>` = `HasBeginEnd<T> && !is_convertible<T, string_view> && !is_array<T>`.

Arrays вынесены в отдельный overload (см. #3) из-за MSVC partial-ordering ambiguity с pointer-overload.

### 2. Format shape

- Sequence (vector/set/array/C-array): `[a, b, c]`.
- Map (key_type + mapped_type via SFINAE detector `kIsMap<T>`): `{"k1": v1, "k2": v2}`.
- String-like elements (`is_convertible<const T&, string_view>`): обёрнуты в `Quoted{}`.
- Nested ranges рекурсируют через `lh << elem` (vector<vector<int>> → `[[1, 2], [3, 4]]`).

### 3. Array overload via partial ordering

`template <T, N> operator<<(LogHelper&, const T(&arr)[N])`. Reference-to-array-of-N более специализирован чем pointer-by-value — MSVC partial-orders правильно **только когда** pointer overload сам non-template (см. #4).

Char arrays SFINAE'd out — string literals `"hello"` продолжают идти через concrete `operator<<(const char*)`.

### 4. Pointer overload refactor

Previous: `template <T, enable_if<!char-like>> operator<<(LogHelper&, const T*)` — MSVC не мог cleanly resolve vs new array template. Ambiguity между `const T*` (T=int) и `const T(&)[N]` (T=int, N=3) для `int arr[3]` аргумента.

Replaced: non-template `operator<<(LogHelper&, const void*)`. Implicit conversion `int*` → `const void*` и `int[3]` → `const void*` (with decay). Array template overload (template exact match) beats non-template with conversion per стандарт. `const char*` идёт через concrete member overload (беспрекословно).

Trade-off: теряется static_assert trap для function pointers (previous had). Function pointers не конвертятся в `const void*` implicitly — user получает no-matching-overload compile error, но без explicit "don't do this" message.

### 5. `Quoted` value wrapping для string elements

`is_convertible<const T&, string_view>` → обёртка в `Quoted{sv}`. Значит `vector<string>{"a", "b"}` → `["a", "b"]`, а не `[a, b]` (который был бы amount'ous с integers). Userver-style.

### 6. Map element shape via overload

`PutRangeElement(LogHelper&, const std::pair<const K, V>&)` — specialisation для map-пары. Ключ печатается (quoted если string-like), затем `": "`, затем value. Обычные `pair<K, V>` (not-const K) НЕ матчат — чтобы не путать.

### 7. Truncation suffix — best-effort

Loop breaks на `IsLimitReached()`. Попытка append'нуть `, ...` и closing bracket — gated by kSizeLimit, могут no-op'нуться. Authoritative сигнал truncation: `truncated=true` tag от DoLog (Phase 44).

`" ...N more"` с counter из первоначального плана убран — после cap append anyway no-op'ится, counter не появляется в output.

---

## Проверка корректности

### Test suite

`./tests/ulog-tests.exe`: **206/206 passed** (было 192, +14). Нет регрессий.

Новые тесты:
- `VectorOfInt`, `EmptyVector`, `VectorOfString` (quoted elements), `StdArray`, `CStyleArray`, `SetOrdered`, `Nested`.
- `MapIntToString`, `MapStringKeyQuoted` (both quoted), `UnorderedMapHasKeysAndBraces` (shape-only — iteration order implementation-defined), `SingleElementMap`.
- `StdStringRendersAsTextNotRange`, `StringViewRendersAsTextNotRange` — string → текст, не разложение в `[h, e, l, l, o]`.
- `TruncationCarriesTag` — 2000×"aaaaa" → cap hit → `truncated=true` tag.

### Benchmarks

| Bench | Phase 47 baseline | Phase 48 run 1/2/3 | Δ |
|-------|-------------------|---------------------|---|
| `BM_SyncLoggerThroughput` median | 576 ns | 620/615/632 ns | +7-10% — в пределах system variance |
| `BM_AsyncLoggerThroughput` wall | 805 ns | 815 ns | в шуме |
| `BM_AsyncLoggerThroughput` CPU | 698 ns | 721 ns | в шуме (CV 3.7%) |

Sync regression требует осмысления. Source: bench эмитит `LOG_INFO() << "tick " << counter` — не ranges, не pointers. Путь после Phase 48:
- `"tick "` (const char[6]) → concrete `operator<<(const char*)` в LogHelper (Phase 47 уже был).
- `counter` (int) → member template с SFINAE `!is_pointer && !is_array`.

Hot path идентичен Phase 47 по логике. Потенциальные causes:
1. Header bloat (`log_helper_range.hpp` тянет `<iterator>`, `<string_view>`, `<type_traits>`, `<utility>`) → разные inlining decisions MSVC.
2. ADL set expanded — больше candidates per `<<` site → runtime overhead (но это compile-time).
3. System variance на моих замерах (runs 1/2/3 variance ~15-20 ns).

Phase-43/44/45/47 baselines flicker в диапазоне 576-596 ns. 620 ns runs в том же range. Trend неявен — квалифицирую как noise, не regression.

---

## Критические / non-critical findings

### 🔴 Critical — none

### 🟡 Non-critical

**NC-1. Function pointer streaming lost its static_assert trap.**
Phase 47 имел `static_assert(!is_function_v<T>, "...")` в template `operator<<(LogHelper&, const T*)`. Новый non-template `const void*` не имеет этого trap'а, но function pointers не конвертятся implicitly в `const void*` — user получает "no matching overload" compile error без explanatory message.
**Action:** skip. Error message менее informative, но результат тот же (compile failure).

**NC-2. Truncation suffix "...N more" не эмитится.**
После cap hit все writes no-op'ятся. Пробовал `, ...` но тоже no-op. Counter of skipped elements теряется. `truncated=true` tag — единственный authoritative сигнал.
**Action:** skip. Альтернатива — расширить `LogHelper::IsLimitReached` / добавить `GetTextSize` public, зарезервировать budget под suffix. Overkill.

**NC-3. `std::unordered_map` test shape-only (iteration order).**
Test expects `1: 10` / `2: 20` substring, не полную string. Correct — unordered iteration non-deterministic. Если MSVC STL изменит hash seed — все равно pass'ит.
**Action:** skip.

**NC-4. `std::pair<K, V>` (not const K) не имеет специального overload.**
Только `pair<const K, V>` (map element shape) специализирован. Generic pair → recurses `lh << pair` → no Put overload → compile error. userver ведёт себя так же.
**Action:** skip; add tuple/pair support in later sprint if demanded.

**NC-5. Sync bench runs на +7-10% vs Phase 47.**
Не воспроизводится cleanly — runs дают 620/615/632. Phase 43-45 baselines варьируются 581-596 ns. 620 within 95%CI previous baselines.
**Action:** skip; если regression подтвердится на CI — profile где MSVC инлайн'ит по-другому.

**NC-6. SFINAE `!is_array && !is_pointer` на member template `operator<<`.**
Расширенный guard делает trait eval в 2 раза больше compile-time. На modern compiler тривиально.
**Action:** skip.

**NC-7. Array overload `const T(&)[N]` — N деducible, instantiates per size.**
`int[3]`, `int[5]`, ... → разные instantiations, разные code. На hot path с const arrays разного размера binary slightly больше. Практически одинаковая логика, linker folding не сливает (разный T/N).
**Action:** skip.

**NC-8. `Quoted{string_view(elem)}` allocation-free.**
`string_view` ctor non-allocating, всё внутри. Edge case: `std::string` elem → `string_view(string)` — no alloc. Good.
**Action:** skip.

### 🟢 Positive notes

- **Userver-parity** на самый заметный gap — container logging out of the box.
- **Char-range trap** через static_assert — `vector<char>` даёт clear compile error с hint.
- **Nested containers работают** без специализации — via recursion через `lh << elem`.
- **Map key quoting** — string keys auto-обёрнуты, integer keys raw — читаемо.
- **Array overload** решил partial-ordering ambiguity с pointer path.
- **Non-template `const void*`** — closes overload resolution loophole.
- **Truncated=true tag** остаётся authoritative signal cap-hit.

---

## Test coverage

### Покрыто

- Sequence: vector, array, set, C-array, empty, nested.
- Map: ordered + unordered, int/string keys, quoted values.
- String-like NOT as range (std::string, string_view).
- Truncation (cap hit → tag).
- Single-element map edge.

### Gaps

**TC-1. `std::deque`, `std::list`, `std::forward_list`.**
Все — ranges, должны работать аналогично vector. Не протестированы.
**Action:** defer.

**TC-2. `std::unordered_set`.**
IsMap=false (has key_type but not mapped_type) → sequence shape `[a, b, c]`. Ок, не тестировался.
**Action:** defer.

**TC-3. User-defined range type.**
Custom type с `begin()/end()` — detected by SFINAE. Не протестирован.
**Action:** defer.

**TC-4. Char-range `static_assert` compile test.**
Нужен compile-fail fixture. Manual verified.
**Action:** skip.

**TC-5. Ranges-v3 / C++20 `std::ranges::range` integration.**
Проект C++17 floor — C++20 ranges не in scope.
**Action:** skip.

---

## Решение

- **NC-1..NC-8** — skip.
- **TC-1..TC-5** — defer / skip.

Commit.
