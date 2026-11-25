# Native frontend

The installed package exposes the native Ulog frontend through
`<ulog/level.hpp>`, `<ulog/source_location.hpp>`, `<ulog/logger.hpp>`,
`<ulog/log.hpp>`, `<ulog/fields.hpp>`, and `<ulog/timestamp.hpp>`.

`Level` is ordered from `kTrace` through `kCritical`; `kNone` is the suppressing
threshold and is never an enabled message level. `IsLevelEnabled` provides the
same predicate used by Logger.

`SourceLocation::Current()` must be called at the application call site. It
captures file, function, line, and column without allocation. `Custom` accepts
borrowed file and function names for adapters; those names must remain valid
until the synchronous Logger call returns. An accepted Runtime call copies
source data into its owned Record before returning.

`Logger` is a pointer-sized, trivially copied non-owning handle. A
default-constructed Logger, `GetNullLogger()`, and the initial
`GetDefaultLogger()` all refer to the same address-stable static Null Logger.
The Null Logger reports `kNone`, suppresses every level, and performs no message
evaluation, allocation, locking, reference counting, or reclamation.
`ShouldLog()` is an advisory preflight query; `Log()` checks the current level
again so a future concurrent level change cannot bypass filtering.

Message construction is explicitly lazy:

```cpp
logger.Log<ulog::Level::kInfo>(ulog::SourceLocation::Current(), [&]() -> std::string_view {
  return BuildMessageView();
});
```

The factory body is invoked synchronously at most once and only after
compile-time and runtime filtering. Put all message work and side effects inside
that body: C++ constructs the factory argument before calling `Log`, so an eager
init-capture such as `[message = BuildMessage()]` is outside the lazy guarantee.
The result must be usable as `std::string_view` and is consumed before the
factory result is destroyed. Caller exceptions can only occur if the factory is
admitted and remain outside Ulog's no-exception guarantee.

`Logger::LogUntil<Level>(deadline, source, ...)` takes the same factories, and
waits for admission capacity until `deadline` instead of shedding. It is the
only blocking call; see [Blocking admission](runtime.md#blocking-admission).

The concrete [Runtime](runtime.md) uses this same Logger
dispatch. It claims a producer-local ingress cell and reserves the complete
configured worst-case Record charge before evaluating the factory, then copies
the source and message into bounded owned storage. Rejected calls therefore
preserve the lazy contract. Each producing thread prepares its bounded slot by
calling `Runtime::GetLogger()` on that thread.

## Levels and thresholds

`ToString(level)` and `ToUpperCaseString(level)` return the canonical names
`trace`, `debug`, `info`, `warning`, `error`, `critical`, and `none`, or their
uppercase forms; a value that is not a `Level` enumerator has an empty name.
`ParseLevel(name)` accepts those seven names ignoring ASCII case and rejects
anything else, such as `warn` or a name with surrounding spaces; a rejected
result's `HowToFix()` lists every accepted name (`LVL-002`):

```cpp
const ulog::LevelParseResult parsed = ulog::ParseLevel(config_value);
if (!parsed) {
  ReportConfigurationError(parsed.HowToFix());
}
```

`Logger::SetLevel(threshold)` atomically replaces the threshold of the Logger
state the handle refers to, and `GetLevel()` and `ShouldLog()` query it
(`API-007`). A producer loads the threshold without a lock on every call, so its
next call observes the change; a call already past filtering finishes under the
threshold it loaded. `kNone` suppresses every level, including `kCritical`:
the Critical priority reserve is a delivery policy, not immunity from
filtering. `SetLevel` reports a `LevelChangeStatus`:

- `kChanged` for a Runtime Logger whose admission is open;
- `kNullLogger` for the Null Logger, which always stays at `kNone` (`API-008`);
- `kInvalidLevel` for a value that is not a `Level` enumerator; and
- `kClosed` after Shutdown or destruction closed admission. A closed Runtime
  Logger reports `kNone`, even when a change races with Shutdown.

`DefaultLoggerLevelScope` temporarily replaces the Default Logger threshold and
restores the previous value when it is destroyed, including during exception
unwinding (`DFL-002`):

```cpp
{
  const ulog::DefaultLoggerLevelScope quiet{ulog::Level::kError};
  RunNoisyStep();
}  // the previous threshold is back
```

The scope loads the Default Logger once and changes only that target's
threshold. It never exchanges the target, so it cannot affect the address
stability of published Default Logger states, and a later
`ExchangeDefaultLogger` does not redirect the restoration. Scopes nest in LIFO
order. On the Null Logger the scope's `Status()` is `kNullLogger` and nothing is
changed or restored.

## Default Logger exchange

`ExchangeDefaultLogger()` atomically installs a non-owning Logger and returns
the previous target. The exchange is one lock-free pointer operation: it takes
no lock, performs no reference counting or reclamation, and does not wait for
producers that already loaded the previous target. Such producers finish the
whole call against that still-live Logger; an ordinary Logger never reloads the
process-wide target or checks a forwarding pointer.

No ownership is transferred. Every Logger state ever installed as the Default
Logger must remain alive at a stable address until application termination,
including after replacement. The public Runtime tracer supplies a non-owning
Logger but does not install it automatically; an application that installs it
must keep that Runtime alive for the required application-long lifetime. The
initial target remains the static Null Logger.

An unnamed call uses one loaded handle for filtering and dispatch:

```cpp
const ulog::Logger target = ulog::GetDefaultLogger();
target.Log<ulog::Level::kInfo>(ulog::SourceLocation::Current(), []() -> std::string_view {
  return "one stable target for the complete call";
});
```

The unnamed `LOG*` macros preserve this single-load form after compile-time
erasure.

## Basic LOG macros

`<ulog/log.hpp>` provides generic, named-level, and explicit-target statement
forms:

```cpp
LOG(level, "plain text");
LOG_TO(logger, level, "value={}", value);

LOG_TRACE("trace");
LOG_DEBUG("debug={}", value);
LOG_INFO("info");
LOG_WARNING_TO(logger, "warning={}", value);
LOG_ERROR_TO(logger, "error");
LOG_CRITICAL("critical={}", value);
```

All six named levels have both the unnamed and `_TO` form. `_TO` accepts one
native `ulog::Logger` expression, evaluates it once, and does not consult the
Default Logger. Use `GetNullLogger()` for an explicit suppressing target.
Generic level expressions are also evaluated once.

A leading `ulog::Fields` or `ulog::FieldSet` operand attaches typed fields to
the Record; see [Typed fields](#typed-fields). A call with one message operand
treats it as native text usable as
`std::string_view`; braces in that operand are literal. A call with a format
string and one or more values uses fmt's compile-time-checked format strings
and writes directly into the bounded Record. Message operands are evaluated
only after the Record is reserved. Compile-erased, runtime-filtered, Null
Logger, unregistered-producer, and admission-rejected calls therefore do not
evaluate them. A failure raised after native conversion or fmt formatting
begins, including an invalid dynamic width or a custom formatter exception,
abandons the reserved Record without publishing it or emitting an exception. An
exception raised while evaluating a caller operand abandons the Record and
propagates.

Each macro expands as one statement and is safe in unbraced nested `if`/`else`
and loop bodies.

## Stream form and scope writer

Every named, `_TO`, and generic macro also accepts `<<` operands after its
call, with or without message operands (`API-002`, `API-003`):

```cpp
LOG_INFO() << "loaded " << count << " items in " << elapsed_ms << "ms";
LOG_WARNING_TO(logger) << error;               // a std::exception writes what()
LOG_ERROR("retry {} of {}", retry, maximum) << ", last error: " << code;
LOG_INFO(ulog::Fields{{"user", id}}) << "signed in";
LOG(level) << value;
```

A macro is a scope writer that lives for its statement. It loads its target,
filters the level, and reserves the Record before any message or `<<` operand
is evaluated, so compile-erased, runtime-filtered, Null Logger,
unregistered-producer, and admission-rejected statements evaluate none of them.
The Record is published when the statement ends. An exception thrown by an
operand abandons it and propagates. A logging statement inside an operand of
another one on the same thread is rejected as `rejected_lane_full`, because the
outer statement still holds its producer's publication slot.

`ulog::LogWriter` from `<ulog/log_writer.hpp>` is the same writer as an object
(`API-004`):

```cpp
{
  ulog::LogWriter writer{logger, ulog::Level::kInfo, ULOG_SOURCE_LOCATION()};
  writer("batch {}", batch_id);
  for (const Item& item : items) writer << ' ' << item.name;
  writer.AddField("count", items.size());
}  // publishes one Record here
```

It is neither copyable nor movable. `IsActive()` reports whether it holds an
admitted Record. `Submit()` publishes early, and the destructor publishes
otherwise. A writer that was filtered or rejected, whose formatting failed, or
that is destroyed while an exception unwinds publishes nothing. Writing through
it never throws and never allocates.

Streamed values use these text forms (`VAL-001`, `VAL-003`, `VAL-004`). The
native forms do not depend on the locale:

| Value | Text |
| --- | --- |
| `char` | the character |
| strings, `std::string_view`, C strings | the text |
| null pointers, `nullptr`, null C strings | `(null)` |
| other pointers | the address as `ulog::Hex` |
| signed and unsigned integers, including `signed char` and `unsigned char` | decimal |
| `float`, `double`, `long double` | fmt's shortest round-trip form, such as `1.5`, `1e+300`, `1e-07`, `nan`, `-inf` |
| `bool` | `true` or `false` |
| `std::exception` | `what()` and the dynamic type, as in `boom (std::runtime_error)` |
| `ulog::Hex{v}` | `0x` and 16 uppercase hexadecimal digits |
| `ulog::HexShort{v}` | shortest uppercase hexadecimal without a prefix, such as `1F` |
| `ulog::Quoted{text}` | the text in double quotes, with `"` and `\` escaped by a backslash |
| `std::atomic<T>` | one `load()` of the value |
| `std::optional<T>` | the value, or `(none)` |
| `std::error_code` | `category:value (message)` |
| `nanoseconds` through `hours` | the count and `ns`, `us`, `ms`, `s`, `min`, or `h` |
| `system_clock` time points | UTC, as `2000-01-02T03:04:05.123456+0000`; trailing fractional zeros are dropped |
| tuples and pairs | `(a, b)` |
| ranges | `[a, b]`, maps as `[k: v]`; string elements are quoted and ranges nest |
| `ulog::Fields`, `ulog::FieldSet` | added as typed fields, not text |

Any other value is written by the first form that applies:
1. a native overload `ulog::LogWriter& operator<<(ulog::LogWriter&, const T&)`, found
   by argument-dependent lookup;
2. text usable as `std::string_view`;
3. an integer;
4. an exception;
5. an ostream `operator<<`;
6. a range;
7. an `fmt::formatter`.

A type that is both ostream-writable and a range uses its stream operator, as in
the baseline. A range of `char` does not compile; convert it to
`std::string_view` or give it a native overload.

An invocable that takes `ulog::LogWriter&` is a lazy writer (`VAL-005`):

```cpp
LOG_DEBUG() << [&](ulog::LogWriter& writer) { writer << ExpensiveSummary(); };
```

It runs only while the writer holds an admitted Record, so a filtered or
rejected statement never calls it. Its exceptions propagate and abandon the
Record.

`wchar_t`, `char8_t`, `char16_t`, and `char32_t` do not compile; convert them to
UTF-8 text or cast them to an integer. Function pointers and stream manipulators
such as `std::endl` do not compile either. `writer(format, args...)` and
`writer.Format(format, args...)` append compile-time-checked fmt text that later
`<<` operands continue (`VAL-002`). Formatting writes the result into the
Record; no argument is retained.

These native forms make no heap allocation once warmed up:
- text, characters, integers, floating point, and booleans;
- pointers, `Hex`, `HexShort`, `Quoted`, atomics, and optionals;
- durations, time points, tuples, pairs, and ranges of these;
- exceptions;
- fmt text;
- typed fields and lazy writers that themselves do not allocate.

On GCC and Clang an exception's type is demangled into a per-thread buffer. The
buffer grows to the longest type name the thread has logged, is reused after
that, and is freed when the thread exits. It is not Record payload memory.
These forms may allocate:
- `std::error_code`, whose `message()` returns a `std::string`;
- ostream-writable values: Ulog writes them through a bounded `std::ostream`
  adapter, and the stream itself may allocate (MSVC does);
- user `fmt::formatter` and ostream operators that allocate.

The adapter's output is still cut at the Record bound like every other form.

Text past the Record bound is cut at a valid UTF-8 boundary, and the Record gets
exactly one `ulog.truncated=true` field; the Record never grows past its
reservation (`DEF-015`). A range stops at the bound instead of writing the
baseline's `...N more` suffix, which could not fit there. See
[Record bounds](#record-bounds).

## Compile-time cutoff

Define `ULOG_COMPILE_TIME_MIN_LEVEL` consistently for all translation units in
one target. It accepts an integer from `0` (`kTrace`) through `5`
(`kCritical`) and defaults to `0`. A templated `Logger::Log` below that minimum
still type-checks the factory contract but does not dispatch or invoke its body.
`kNone` calls compile and are always suppressed. Invalid cutoff values fail
compilation with a correction hint.

Named macros below the configured minimum are erased before evaluating an
explicit target or loading the process-wide Default Logger. Generic `LOG` and
`LOG_TO` are intentionally not erased by this control because their level is a
runtime expression. Critical remains active at the maximum value `5`.

## Source path prefixes

Call sites record the compiler file path unless a configured prefix matches it
(`API-006`). Configure the prefixes per target through CMake, in a source tree
or from an installed CMake or Conan package:

```cmake
find_package(ulog CONFIG REQUIRED)
target_link_libraries(app PRIVATE ulog::ulog)
ulog_target_source_path_prefixes(app PRIVATE "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}")
```

The function defines `ULOG_SOURCE_PATH_PREFIXES` as a comma-separated list of
string literals; a build that does not use CMake can define it the same way.
The first prefix that matches whole leading path components is removed together
with the separators that follow it, so `/src/app` turns `/src/app/net/io.cpp`
into `net/io.cpp` but leaves `/src/application/main.cpp` unchanged. A path equal
to the prefix is not trimmed. On Windows, `/` and `\` match each other, and
CMake writes prefixes with `/`.

Every `LOG*` macro evaluates its trimmed `SourceLocation` as a constant
expression, so trimming happens at compile time and the logging path does no
per-call work. The Record's source path is that constant's path. `ULOG_SOURCE_LOCATION()` yields the same constant for code that
calls `Logger::Log` directly; `SourceLocation::Current()` always keeps the
compiler path. Use the same prefixes in every translation unit that shares
inline code containing Ulog macros. The prefix list cannot change at run time.

## Typed fields

`<ulog/fields.hpp>` attaches ordered typed fields to a Structured Record. A
field holds a string, signed or unsigned integer, floating-point, boolean,
`ulog::kNull`, or raw-JSON value (`VAL-006`, `VAL-008`). Signed integers are
stored as `int64`, unsigned integers as `uint64`, and floating point as
`double`; `char` and the `charN_t` types are text units and are rejected.

Pass `ulog::Fields` or a `ulog::FieldSet` as the first operand of a `LOG*`
macro, before the message:

```cpp
LOG_INFO(ulog::Fields{{"status", 200}, {"path", path}, {"ok", true}}, "done in {}ms", elapsed);

ulog::FieldSet extra;
extra.Add("request_id", request_id);
extra.Add("region", "eu", ulog::FieldMode::kFrozen);
LOG_WARNING_TO(logger, ulog::Fields{extra, {"attempt", attempt}}, "retrying");
LOG_ERROR_TO(logger, extra, "failed");
```

`Fields` borrows its elements, so build it inside the macro call. Field operands
are evaluated after the Record is reserved, exactly like message operands:
compile-erased, runtime-filtered, Null Logger, unregistered-producer, and
admission-rejected calls evaluate none of them.

The Logger API takes a fields callback before the message factory:

```cpp
logger.Log<ulog::Level::kInfo>(
    ulog::SourceLocation::Current(),
    [&](ulog::FieldWriter& fields) {
      fields.Add("status", status);
      fields.Add(extra);
    },
    [&]() -> std::string_view { return message; });
```

Both callables run synchronously at most once, after filtering and admission.
`FieldWriter::Add` reports a `ulog::FieldStatus`; the macros ignore it.

`FieldSet` is the owned counterpart of `LogExtra`, built before the call. It
copies keys and values, so a set may be destroyed or changed after insertion.
`ulog::JsonString` owns one line of JSON text: CR and LF are removed and empty
text means `null`. Raw and the text formats write it like an escaped string.
JSON encoders insert one valid JSON value as a raw nested value without its
insignificant whitespace, and write any other text as a JSON string; see
[Formats](runtime.md#formats).

### Collision and reserved-key policy

One policy applies in `FieldSet` and in every Record (`VAL-007`, `DEF-004`):

- a later value for an existing replaceable key replaces it at its original
  position (`kReplaced`);
- a frozen field keeps its value and rejects every later value (`kFrozen`).
  `FieldMode::kFrozen` freezes on insertion; `FieldSet::Freeze(key)` freezes an
  existing field and returns false for a missing key;
- merging a FieldSet, or inserting one into a Record, adds its fields in order
  and keeps each field's freeze state;
- keys collide only when byte-for-byte equal, so repeated call-site keys keep
  one field;
- `timestamp`, `level`, `module`, `task_id`, `thread_id`, `text`, the YaDeploy
  JSON names `@timestamp`, `levelStr`, and `message`, and every key starting
  with `ulog.` are technical keys. They are compared ignoring ASCII
  case, because text formats lowercase keys, and are rejected with `kReserved`.
  `ulog::IsReservedFieldKey` exposes the check. Empty keys are rejected with
  `kInvalidKey`.

Encoders render each stored field once, in Record order; they never re-apply or
bypass the policy. Raw and TSKV lowercase ASCII keys when they render them.

### Record bounds

Fields are copied into the reserved Record without heap allocation, and their
bytes count toward `maximum_record_bytes` with the message and source
location. A field that does not fit is dropped with `kTruncated`. The Record then
carries `ulog.truncated=true` as its last field and never grows past its
reservation. To make room for that marker, Ulog first shortens the message at a
valid UTF-8 boundary and then drops the most recently written fields; a dropped
replacement restores the value it replaced. A replaced value keeps occupying its
bytes until the Record is released.

## Event timestamps

An accepted Runtime call stores one wall-clock event timestamp, in microseconds
since the Unix epoch, after admission reservation and before the message factory
runs. Rejected and filtered calls never read the clock. The Runtime clock seam is
described in [Runtime](runtime.md#event-timestamps).

`<ulog/timestamp.hpp>` provides portable formatting for that value:

```cpp
const ulog::TimestampText utc = ulog::FormatUtcTimestamp(951'827'696'789'012);
// utc.View() == "2000-02-29T12:34:56.789012"
const ulog::TimestampText local = ulog::FormatLocalTimestamp(951'827'696'789'012);
```

Both functions produce `YYYY-MM-DDTHH:MM:SS` plus six fractional microsecond
digits and no zone designator, into a fixed 26-byte `TimestampText` without
allocation. Both are thread-safe on every supported platform. UTC formatting is
pure calendar arithmetic and never calls the C library time functions. Local
formatting converts through the platform's reentrant conversion, `localtime_s`
on Windows and `localtime_r` elsewhere, behind this Ulog-owned interface, and
follows the process time zone, including a `TZ` change made with `tzset`.

The result is empty for instants outside years 0000 through 9999, and for local
time also when the platform cannot convert the instant; Windows converts only
instants from 1970 through 3000. Time-zone selection beyond the process-local
zone is not provided.
