# Review 30 — Phase 40 (`LogRecordLocation` optimization from userver)

Scope: port userver's `utils::impl::SourceLocation` trick to ulog — precomputed decimal line string + `Current()` capture via compiler builtins + default-arg hook on `LogHelper` so the macro stops mentioning `__FILE__` / `__LINE__` / `__func__`.

## Changes

### Struct → class upgrade (`include/ulog/log_helper.hpp`)

```cpp
class LogRecordLocation {
    const char* file_{""};
    const char* function_{""};
    std::uint_least32_t line_{0};
    std::uint_least32_t line_digits_{0};
    char line_string_[8]{};  // decimal ASCII, precomputed in ctor

    static constexpr LogRecordLocation Current(
        EmplaceEnabler = {},
        const char* file = ULOG_IMPL_BUILTIN_FILE(),
        const char* function = __builtin_FUNCTION(),
        std::uint_least32_t line = __builtin_LINE()) noexcept;
};
```

- Three-argument constructor `(file, line, function)` retained for back-compat (direct construction in tests / synthetic records).
- `Current()` uses the **default-argument evaluation at call site** trick — `__builtin_FILE` / `__builtin_FUNCTION` / `__builtin_LINE` snapshot the caller's source position, not `log_helper.hpp`'s.
- `EmplaceEnabler` tag prevents callers from accidentally invoking `Current(file, function, line)` with three positional arguments.
- Line number is converted to decimal ASCII at construction time — `line_string()` returns a `string_view` into the inline 8-byte buffer. Zero runtime cost per record in formatters, which now splice the bytes directly.

### LogHelper default-arg hook

`LogHelper` constructors take `const LogRecordLocation& = LogRecordLocation::Current()`. Macros no longer build a `LogRecordLocation{...}` explicitly — the compiler evaluates the default arg at the macro's expansion point.

Old macro fragment (removed):

```cpp
#define ULOG_IMPL_LOG_LOCATION() \
    ::ulog::LogRecordLocation{ULOG_IMPL_TRIM_FILE(__FILE__), __LINE__, \
                              static_cast<const char*>(__func__)}
```

### Formatter ctor change — `(Level, const LogRecordLocation&, tp, ...)`

TskvFormatter / LtsvFormatter / JsonFormatter / OtlpJsonFormatter all collapse `std::string_view module_function, std::string_view module_file, int module_line` into a single `LogRecordLocation&`. The `module` field is assembled without `fmt::format`:

```cpp
detail::SmallString<128> buf;
buf += location.function_name();
buf += " ( ";
buf += location.file_name();
buf += ':';
buf += location.line_string();  // precomputed — no fmt dispatch
buf += " )";
```

Saves one `fmt::format` + one `std::string` heap allocation per record (per format, with per-sink-format active).

### Source-root trimming now runs inside `Current()`

`ULOG_IMPL_BUILTIN_FILE()` expands to `TrimSourceRoot(__builtin_FILE(), ULOG_SOURCE_ROOT_LITERAL)` when the CMake knob is set, else raw `__builtin_FILE()`. Constexpr-friendly path — same existing trim, just moved to the default-arg site.

## Tests

7 tests in `tests/log_record_location_test.cpp`:

- `CurrentCapturesCallerFile` / `CurrentCapturesCallerLine` — the builtin capture really sees the caller's position, not log_helper.hpp.
- `LineStringIsPrecomputedDecimal` / `ZeroLineStillRendersZero` / `EightDigitLineFitsBuffer` — precomputed decimal buffer correctness at boundaries.
- `HasValueDistinguishesEmpty` — default-constructed location (empty file+function) returns false; any non-empty one returns true.
- `TskvModuleUsesPrecomputedLine` — end-to-end: LOG_INFO inside the test, assert the emitted `module=…:<line> )` suffix matches `__LINE__ + 1` at the call site.

Existing tests touched:

- `tests/formatter_test.cpp`: three direct formatter constructions updated for the new ctor shape.
- `tests/async_logger_test.cpp`: two explicit `LogHelper(logger, level, {})` calls dropped the trailing `{}` — the default arg now handles it (the `{}` was ambiguous between `LogRecordLocation{}` and `NoLog{}`).

## Findings (self-review)

### Addressed

- **Formatter API consistency** — all four text formatters share the same ctor shape now; no mixed signatures left.
- **`MakeFormatterInto` / `MakeFormatterForFormat`** on `TextLoggerBase` collapse the three module args into a `LogRecordLocation&` — downstream loggers that override these see the simpler interface.
- **`NullLogger` override** signature synced to new virtual.

### Open / acceptable

- **`LogHelper::Impl::location` stored by value (`LogRecordLocation`)** — the class is a value type (~24 bytes: two pointers + two u32s + 8-byte buffer). Cheap copy; matches previous behaviour. Keeping the field by value avoids dangling-ref concerns if `LogHelper` outlives the caller's `LogRecordLocation::Current()` temporary (it does, because the temp is materialized into the default-arg and bound to the const-ref parameter only during construction).
- **`MakeFormatterInto` / `MakeFormatterForFormat` empty-location fallback** uses `static constexpr LogRecordLocation kEmptyLocation{}` to drop location when `emit_location_ == false`. Returning a const ref to a static is safe and doesn't churn.
- **`LogRecordLocation` line buffer fixed at 8 bytes** matches userver. Files with 9+ digit source lines (>99,999,999) would truncate silently — but that's one hundred million lines in a single translation unit, not a realistic concern. `RenderLineDigits` writes `min(n, 8)` bytes.

### Not addressed (scope creep)

- userver has three-prefix path trim (source / build / relocated). ulog still has the single `ULOG_SOURCE_ROOT_LITERAL` knob — good enough for in-tree builds, can extend later if relocated-install paths become a concern.
- No benchmark yet. The win is visible at the formatter level (one fewer fmt::format + alloc per record per format). Adding a micro-bench could go in the next phase.

## Test coverage status

164/164 tests pass (7 new + 157 prior). No regressions.
