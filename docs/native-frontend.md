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

The concrete [Runtime](runtime.md) uses this same Logger
dispatch. It claims a producer-local ingress cell and reserves the complete
configured worst-case Record charge before evaluating the factory, then copies
the source and message into bounded owned storage. Rejected calls therefore
preserve the lazy contract. Each producing thread prepares its bounded slot by
calling `Runtime::GetLogger()` on that thread.

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
and writes directly into the bounded Record. Message operands stay inside the
admission callback. Compile-erased, runtime-filtered, Null Logger,
unregistered-producer, and admission-rejected calls therefore do not evaluate
them. A failure raised after native conversion or fmt formatting begins,
including an invalid dynamic width or a custom formatter exception, abandons
the reserved Record without publishing it or emitting an exception. Exceptions
raised while evaluating caller operands before the formatter is entered remain
outside Ulog's guarantee.

Each macro expands as one statement and is safe in unbraced nested `if`/`else`
and loop bodies. The stream expression `LOG_INFO() << value` is not part of the
basic frontend. It is assigned to the later **Scope writer and streamed
values** capability (`API-004`, `VAL-001` through `VAL-005`), which must provide
a reservation-backed writer before evaluating streamed operands. Advanced
range, map, chrono, and exception writers belong to that capability as well.

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
are evaluated inside the admission callback, exactly like message operands:
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
text means `null`. Raw and the text formats write it like an escaped string;
JSON encoders insert it as a raw nested value.

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
- `timestamp`, `level`, `module`, `task_id`, `thread_id`, `text`, and every key
  starting with `ulog.` are technical keys. They are compared ignoring ASCII
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
