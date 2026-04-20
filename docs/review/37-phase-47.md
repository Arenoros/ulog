# Phase 47 — self-review: LogHelper streaming parity (chrono, exception, optional, error_code, pointer)

**Scope:** Phase 47 из BACKLOG `LogHelper streaming parity` Pri 1-5 — 5 low-effort high-value overload'ов из userver inventory (docs/BACKLOG.md:150-196).

Diff:
- `include/ulog/log_helper_extras.hpp` — **NEW**: free-function `operator<<` overloads для `std::chrono::duration<Rep, Period>`, `std::chrono::system_clock::time_point`, `const std::exception&`, `const std::optional<T>&`, `const std::error_code&`, generic `const T*` (null-guard + Hex fallback).
- `include/ulog/log_helper.hpp` — SFINAE на member template `operator<<(const T&)` для исключения pointer-типов; concrete `operator<<(const char*)` чтобы сохранить text-path для string literals / C-strings.
- `include/ulog/log.hpp` — auto-include `log_helper_extras.hpp` (users of `ulog/log.hpp` получают overload'ы по умолчанию).
- `tests/log_helper_streaming_parity_test.cpp` — **NEW**: 12 тестов на каждый overload + macro integration.
- `tests/CMakeLists.txt` — зарегистрирован.

---

## Сводка изменений

### 1. `std::chrono::duration` (Pri 1)

`template <Rep, Period> operator<<(LogHelper&, const duration<Rep, Period>&)` — количество + суффикс через compile-time lookup `DurationSuffix<Period>()`. Known periods: ns / us / ms / s / min / h. Unknown Period → fallback `"N * num/den s"` с числовым выражением ratio.

Суффикс без fmt/chrono dependency — inline `if constexpr` ladder на `std::is_same_v<Period, std::ratio<...>>`.

### 2. `std::chrono::system_clock::time_point` (Pri 1)

Non-template overload, форматирует через существующую `detail::FormatTimestamp(tp, kIso8601Micro)` — `YYYY-MM-DDTHH:MM:SS.ffffff+0000` shape. Timestamp аллоц'ит std::string → try/catch wrap, noexcept invariant сохранён (swallow на OOM).

### 3. `std::exception` + derived (Pri 2)

`operator<<(LogHelper&, const std::exception&)` — stream'ит `ex.what()` как текст, добавляет tag `exception_type=<typeid(ex).name()>` через `GetTagWriter().PutTag`. typeid — runtime-type даже при upcast через `const std::exception&`, так что `throw std::runtime_error(...)` → `exception_type=…runtime_error…`.

Parallel с existing `LogHelper::WithException` — но streaming form для userver-idiom `LOG_ERROR() << "..." << ex`. `WithException` оставлен без изменений.

### 4. `std::optional<T>` (Pri 3)

`template <T> operator<<(LogHelper&, const std::optional<T>&)` — `*opt` или литерал `"(none)"`. Рекурсирует через `lh << *opt` — user типы с существующим operator<< работают без специализации.

### 5. `std::error_code` (Pri 4)

`operator<<(LogHelper&, const std::error_code&)` — `category_name:value (message)`. `ec.message()` аллоц'ит на большинстве категорий → try/catch.

### 6. Pointer streaming (Pri 5)

Template `operator<<(LogHelper&, const T*)` с enable_if'ом на `!is_same<remove_cv<T>, char/signed char/unsigned char>` — `const char*` идёт через concrete member `operator<<(const char*)`.

Reinterpret_cast к `const void*` → `detail::StreamPointer`:
- `nullptr` → literal `"(null)"`.
- остальное → `Hex{reinterpret_cast<uintptr_t>(p)}` (16-hex-digit form).

`static_assert(!is_function_v<T>)` блокирует function-pointer streaming (userver style — cast to void* explicit для сырого code address).

### 7. Member template SFINAE + concrete `const char*`

Существующий `template <T> operator<<(const T&)` member ловил `int*` и шёл через `Put(bool)` (implicit conversion pointer→bool) → `"true"/"false"` — совершенно wrong. Fix: SFINAE `enable_if<!is_pointer_v<T>>` исключил pointer-ы из template'а, concrete `operator<<(const char*)` сохранил text-path для string literals.

Partial-ordering rationale: non-template `const char*` beats free template `const T*` on exact match → `lh << "literal"` остаётся в built-in Put(const char*) path, `lh << int_ptr` падает на free template через ADL.

---

## Проверка корректности

### Test suite

`./tests/ulog-tests.exe`: **192/192 passed** (было 180, +12 новых). Нет регрессий.

Новые тесты (`tests/log_helper_streaming_parity_test.cpp`):
- `DurationKnownSuffixes` — шесть known periods в одной записи: `7ns 8us 9ms 10s 11min 12h`.
- `DurationUnknownRatioFallsBack` — `duration<int, ratio<7,10>>{3}` → `"3 * 7/10s"`.
- `TimePointIso8601Utc` — `system_clock::now()` → запись содержит `text=20…T…` prefix.
- `ExceptionStreamsWhatAndTypeTag` — `throw runtime_error("boom")`; запись содержит `boom` + `exception_type=` + `runtime_error`.
- `OptionalPopulatedStreamsValue` — `optional<int>{42}` → `val=42`.
- `OptionalEmptyStreamsNoneLiteral` — пустой optional → `val=(none)`.
- `ErrorCodeFormatted` — `make_error_code(errc::timed_out)` → shape `ec=...:... (...)`.
- `NullPointerLiterally` — `int* p = nullptr` → `p=(null)`.
- `NonNullPointerAsHex` — valid `int*` → `p=0x…`.
- `CharPointerStillRendersAsText` — `const char* s = "hello"` → `s=hello` (НЕ `s=0x…`).
- `VoidPointerAsHex` — `const void*` → `p=0x…`.
- `WorksViaLogMacro` — полная LOG_INFO цепочка с duration + optional + error_code.

### Benchmarks

| Bench | Phase 45 baseline | Phase 47 | Δ |
|-------|-------------------|----------|---|
| `BM_SyncLoggerThroughput` median | 581 ns | 576 ns | -0.9% (CV 0.87%, в шуме) |
| `BM_AsyncLoggerThroughput` wall | 815 ns | 805 ns | в шуме |
| `BM_AsyncLoggerThroughput` CPU | 678 ns | 698 ns | +3.0% (CV 3.00%, в шуме) |

Все overload'ы — header-only, inline, `noexcept`. SFINAE на member template добавляет один `is_pointer_v<T>` trait eval'ation per instantiation — resolved at compile time, zero runtime cost. Bench confirms no hot-path regression.

---

## Критические / non-critical findings

### 🔴 Critical — none

### 🟡 Non-critical

**NC-1. `typeid(ex).name()` — implementation-defined shape.**
File: `include/ulog/log_helper_extras.hpp:104`. MSVC возвращает demangled `"class std::runtime_error"`, GCC/Clang — mangled `"St13runtime_error"` (без `cxxabi::__cxa_demangle`). userver использует `compiler::GetTypeName` (внутренний demangler). У нас нет — тест ловит это substring-ом `"runtime_error"` который общий для обоих.
**Action:** skip. Demangling — отдельный scope (добавил бы tiny dep на abi library).

**NC-2. Duration fallback `"3 * 7/10s"` — non-obvious shape.**
Unknown Period fallback печатает literal. Читается хуже чем просто `"count() (ratio 7/10)"`. userver ушёл от этого path — поддерживает только known ratios. Для совместимости можно `static_assert(DurationSuffix<Period>() != "")` — но block'ировать exotic durations на compile-time слишком жёстко.
**Action:** skip. Rare edge case.

**NC-3. `operator<<(const std::exception&)` не handles `TracefulException`.**
userver extends: если ex — `TracefulExceptionBase`, emit stacktrace. У нас нет эквивалента → ограничились `what` + `typeid`. Если boost::exception или own TracefulException позже добавится — overload'ит отдельным `operator<<(const TracefulException&)`.
**Action:** skip. Defer до появления TracefulException-like типа.

**NC-4. Pointer overload: `const T*` не матчит не-const pointer args clean.**
`int* p` deduces T=int, argument-type `int*` vs param `const int*` — стандартная конверсия. Template deduction работает через decay. Verified тестами: `NullPointerLiterally` с `int* p = nullptr` passes, `NonNullPointerAsHex` с `int* p = &value` passes.
**Action:** skip.

**NC-5. `std::exception` streaming modifies tag writer — не симметрично с typed tags.**
`LOG_ERROR() << ex` добавляет `exception_type` tag + text. Но streaming обычно не touches tag writer. Это intentional for parity с userver, но edge case: `<< ex << " more text"` → tag вставлен посередине между text chunks (всё же tag'ы emit'ятся до text via `writer.PutTag` после DoLog finalize — см. phase 43). Practically transparent для log format.
**Action:** skip.

**NC-6. `log_helper_extras.hpp` в public include path.**
Все overload'ы — free functions в namespace `ulog`. Они видимы при `#include <ulog/log_helper.hpp>` только через `<ulog/log.hpp>` (который auto-include'ает extras). Users, пулящие `log_helper.hpp` напрямую, не получат overload'ов. Breaking change риск низкий (log_helper.hpp прямой include редок).
**Action:** skip. Документировать в header что extras подключаются через log.hpp.

**NC-7. `time_point` streaming аллоц'ит `std::string` через `FormatTimestamp`.**
Каждое `lh << tp` — one heap allocation. Для hot logging с timestamps в body — мелкий perf cost. Альтернатива: вариант `FormatTimestamp` который append'ит в буфер без alloc'а (signature расходится с existing API). 
**Action:** skip. Prefer API consistency.

### 🟢 Positive notes

- **userver parity на 5 most-used overload'ов** — портируемый код `LOG_INFO() << ex << " took " << elapsed` компилируется out of the box.
- **SFINAE fix на member template** — закрывает давний bug: `lh << ptr` эмитил `"true"/"false"` вместо адреса. Silently wrong → now explicit hex.
- **Concrete `const char*` overload** — гарантирует text-path для string literals независимо от partial ordering heuristics.
- **Zero perf regression** — header-only inlining + compile-time SFINAE.
- **Все overload'ы noexcept + try/catch body** — consistent с Phase 43 invariant.
- **Duration suffix map compile-time** — нет runtime lookup overhead.

---

## Test coverage

### Покрыто

- Six known duration periods.
- Unknown Period fallback.
- Timestamp ISO shape sanity.
- Exception what+type through catch + upcast.
- Optional populated + empty.
- Error_code shape.
- Null pointer, non-null pointer, char pointer (text path preserved), void pointer.
- Integration через LOG_INFO() macro.

### Gaps

**TC-1. Nested optional.**
`optional<optional<int>>` — recursive `<<` dispatch. Теоретически работает (`lh << *opt` рекурсирует), но не протестировано.
**Action:** defer.

**TC-2. `operator<<(const T*)` с user-defined type T.**
`struct S; S* p;` — верhead через Hex. Не протестировано explicitly, но семантически identical с int* path.
**Action:** skip.

**TC-3. Function-pointer `static_assert` trap.**
Не проверено (compile-time assertion — негативный compile test требует отдельной CMake test fixture). Manual-inspect'но в source.
**Action:** skip.

**TC-4. `std::errc` enum → error_code implicit conversion.**
`LOG_INFO() << std::errc::timed_out` — errc is scoped enum. Член template `operator<<<errc>` обрабатывает через `Put<enum>` → print'ит underlying value. Для error_code-style output user должен `<< std::make_error_code(errc::...)`. Не breaking, но userver позволяет оба.
**Action:** defer; возможно добавить `operator<<(std::errc)` overload.

**TC-5. Duration fallback на `std::deci` / `std::centi`.**
Exotic periods. Тот же fallback path что и ratio<7,10>.
**Action:** skip.

---

## Решение

- **NC-1..NC-7** — skip / defer.
- **TC-1..TC-5** — defer.

Commit.
