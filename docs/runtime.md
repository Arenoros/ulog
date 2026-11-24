# Runtime

The installed package provides a concrete, bounded `ulog::Runtime` that moves
accepted native Records through one FIFO worker route. The production route is the
[Raw file route](#raw-file-route), which appends frames encoded in one of the
[formats](#formats) to a file through Ulog's private libuv loop. Two in-memory test destinations,
`ulog::testing::InMemoryDestination` and
`ulog::testing::InMemoryEncodedDestination`, make the same pipeline observable. The structured destination exposes
Record contents; the encoded destination runs the built-in encoder of its format
on the worker, submits each frame as one serialized delivery, and exposes exact bytes.
Its deliveries can complete inline, fail inline, or stay held until the test
completes them. These are executable Runtime test seams: they make admission,
ownership, ordering, encoding, delivery completion, control, and shutdown
behavior observable without introducing a generic destination abstraction.

Include the public interfaces with:

```cpp
#include <ulog/log.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>
#include <ulog/testing/in_memory_destination.hpp>
```

## Construction and ownership

Construct one destination first, then pass a copy to the matching
`Runtime::Create` overload. Destination copies share the same bounded state, so
the application can keep its copy for observation while Runtime owns another:

```cpp
using namespace std::chrono_literals;

ulog::testing::InMemoryDestination destination{{
    .capacity_records = 64,
    .maximum_record_bytes = 16'384,
}};

auto created = ulog::Runtime::Create(
    ulog::RuntimeConfig{
        .threshold = ulog::Level::kInfo,
        .payload_capacity_bytes = 1U << 20U,
        .maximum_record_bytes = 16'384,
        .producer_slots = 32,
        .ingress_cells = 64,
        .control_operations = 8,
        .worker_threads = 1,
        .startup_timeout = 5s,
        .destruction_timeout = 1s,
    },
    destination);

if (!created) {
  ReportConfigurationError(created.failure->Message(), created.failure->HowToFix());
  return;
}
```

One destination state is a one-shot attachment for exactly one Runtime. Copies
are observation handles, not independent destinations: concurrent attachment or
reuse after Runtime destruction is rejected as `kInvalidDestination`. Construct
a fresh destination for each Runtime lifecycle.

`Create` validates the complete configuration, uses the destination's already
allocated fixed backing, allocates the fixed producer and control state, and starts
the worker before returning a usable Runtime. It reports creation failures through
`RuntimeCreateResult`; it does not throw. The separately constructed
`InMemoryDestination` rejects invalid destination configuration with
`std::invalid_argument`. This lifecycle follows
[ADR 0012](adr/0012-own-runtime-and-libuv-lifecycle-explicitly.md); only the file
route adds the private libuv loop.

The current Runtime accepts these bounds:

- `threshold` is `kTrace` through `kNone`; it is the initial Logger threshold,
  which `Logger::SetLevel` may change atomically while the Runtime accepts
  Records (see [Levels and thresholds](native-frontend.md#levels-and-thresholds));
- `maximum_record_bytes` is a 64-byte multiple from 128 through 16,384;
- `payload_capacity_bytes` is a 64-byte multiple large enough for one maximum-sized
  Record;
- `producer_slots` is from 1 through 32, and `ingress_cells` is from
  `producer_slots` through 64;
- `control_operations` is from 1 through 64;
- `worker_threads` is exactly 1;
- startup and destruction timeouts are positive and no greater than 24 hours; and
- an in-memory destination has non-zero capacity and a `maximum_record_bytes` at
  least as large as the Runtime value; and
- a file route has a non-empty file path and 1 through 64 `write_buffers`.

`event_clock` needs no validation: a null `now` selects the system clock. See
[Event timestamps](#event-timestamps).

`RuntimeSnapshot` exposes weakly consistent admission, completion, delivery,
rejection, retained-payload, and lifecycle counters. `processed_records` and
`processed_bytes` count Records the worker has taken from ingress and, for the Raw
route, encoded and submitted. `completed_records` counts Records whose route
attempt the worker has retired: `delivered_records` plus
`delivery_failed_records` plus `encoding_failed_records`. The worker retires
delivery outcomes in admission order, so a delivery completed ahead of an earlier
held delivery is not yet counted as completed. `delivered_bytes` and
`delivery_failed_bytes` are Raw frame totals and remain zero for the structured
route. `encoding_failed_records` counts internal encoding-invariant failures; such
a failure commits no partial frame, cancels unfinished deliveries, and fails
pending Drain or Shutdown Operations.

`route_failed` reports a route stopped by an encoding, write, or close failure, and
`route_io_error` holds the libuv error of a failed file route. Pass it to
`ulog::IoErrorName()` for a stable name such as `EIO`.

`fixed_backing_bytes` reports the capacity-scaled ingress Record slots, destination
payload or file write buffers with their slot metadata, file-sink loop state,
control nodes, and action table separately
from live retained bytes. It is stable for a Runtime but is not a process-memory
total: constant-size Runtime objects, allocation bookkeeping, platform
synchronization objects, and thread stacks are excluded.

## Logger registration and admission

Call `GetLogger()` on every application thread that will produce Records:

```cpp
const ulog::Logger logger = created.runtime->GetLogger();
LOG_INFO_TO(logger, "request_id={}", request_id);
```

The call returns the Runtime's non-owning Logger and prepares one of its fixed
producer slots for the calling thread. Repeated calls on an already registered
thread are harmless. A thread with no available producer slot is not silently given
unbounded state: its logging attempts are rejected before message-factory or macro
message/format operand evaluation. Each active producer owns one lane until Shutdown
or destruction closes admission and releases its registration.

The Logger remains valid only while its Runtime state remains alive. Runtime does not
install it as the process-wide Default Logger. An application that calls
`ExchangeDefaultLogger()` with this handle must keep the Runtime alive at a stable
address until application termination, as required by the
[Default Logger lifetime contract](native-frontend.md#default-logger-exchange).

Admission is bounded and non-blocking on producer threads. A call claims its
producer-local ingress cell and reserves its complete worst-case Record charge before
invoking the message factory or deferred macro operands. Producer-slot, lane, or
payload exhaustion applies
drop-newest, increments the corresponding snapshot rejection count, consumes no
admission sequence, and evaluates no caller message expression. Successful
publication assigns one global admission sequence. The single worker delivers
Records to the destination in that sequence order.

## Observing Records

`InMemoryDestination::TryTake()` never blocks. It returns the ready Record with the
lowest admission sequence as a move-only `ObservedRecord`. The observation is
immutable; its string views remain valid until it is moved from or destroyed.

An `ObservedRecord` pins one destination slot. Moving it transfers that pin, and
destroying it releases the slot for a later delivery. A ready Record not yet taken
also occupies its slot. When all slots are ready or held, the worker waits for a slot;
producer threads still do not wait and eventually reject new Records as their bounded
ingress fills.

`FieldCount()` and `Field(index)` expose the Record's typed fields in order as
`ObservedField{key, value, mode}`, including a final `ulog.truncated` marker; see
[Typed fields](native-frontend.md#typed-fields). The destination copies fields
into its slot within the same `maximum_record_bytes` bound.

`EventTimestamp()` returns the Record's stored event timestamp in microseconds
since the Unix epoch. The worker copies the value the producer captured; neither
the worker nor the destination reads the clock.

The fixed destination backing and drop-newest behavior are the concrete tracer form
of [ADR 0010](adr/0010-bound-pipeline-memory-and-configure-shedding.md).

### Observing Raw bytes

`InMemoryEncodedDestination` has the same one-shot attachment, pause, FIFO, and
slot-pinning behavior. Its `maximum_record_bytes` is the Structured Record bound,
not an independently undersized output limit. Construction derives each encoded
slot from the worst-case frame of its [format](#formats), `2 * maximum_record_bytes
+ 11` bytes for Raw, checks all arithmetic before allocation, and reports the value
through `MaximumEncodedRecordBytes()`. This reserve covers full escaping and
framing, so every valid Record either commits
one complete frame or triggers the terminal internal-failure path; it is never
silently truncated a second time.

`TryTake()` returns a move-only `ObservedEncodedRecord`. `Bytes()` remains valid
until that observation is moved from or destroyed. Raw frames begin with `tskv`,
append ordered fields as tab-separated `key=value` pairs, append `text` last, and
end with exactly one LF. NUL, tab, CR, LF, and backslash use TSKV escaping; `=` is
also escaped in keys, and ASCII key letters are lowercased. Fields render in
Record order: integers in decimal, floating point in shortest round-trip form,
booleans as `1` or `0`, null as `null`, and raw JSON as escaped text. Raw
intentionally omits timestamp, level, module, and source metadata. Encoding executes synchronously inside the worker's consume callback;
no `RecordView` or borrowed field survives that callback.

### Deferred Raw delivery

After the worker encodes a frame into a free slot, it submits that frame to the
destination as one delivery. Submissions are serialized by the single worker and
follow admission order. `SetDeliveryMode()` selects how later submissions
complete:

- `kCompleteInline` commits the frame for `TryTake()` during submission. This is
  the default.
- `kFailInline` fails the delivery during submission and discards the frame.
- `kHold` keeps the delivery pending until the test calls
  `TryTakePendingDelivery()` and then `Complete()` or `Fail()` on the returned
  move-only `PendingEncodedDelivery`.

```cpp
destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
LOG_INFO_TO(logger, "held");
auto drain = runtime->Drain();

auto delivery = destination.TryTakePendingDelivery();  // poll until the worker submits it
if (delivery && delivery->Complete() ==
                    ulog::testing::EncodedDeliveryCompletionStatus::kCompleted) {
  // Drain can now succeed; the frame is ready for destination.TryTake().
}
```

A held frame stays pinned in its slot until its delivery completes, and a slot is
reused only after the worker retires that outcome. Held, ready, and observed
frames therefore share the destination's fixed slot bound. When every slot is
occupied, the worker stops taking Records from ingress; producers still never
wait and reject later Records as drop-newest once their bounded ingress fills.
Completing or failing a held delivery wakes the worker, which retires the outcome
and resumes progress.

Each delivery completes exactly once. The first `Complete()` or `Fail()` returns
`kCompleted` and empties the handle; later calls, calls on a moved-from handle, or
calls on an empty handle return `kInvalidHandle`. Destroying or move-assigning over
an uncompleted handle fails its delivery. After Runtime destruction or a route
failure cancels a delivery, its completion returns `kCancelled` and changes no
accounting. A handle keeps the destination state alive, so `Bytes()` remains valid
until the handle completes, is moved from, or is destroyed, even after the Runtime
is gone. Completion runs no Runtime callback on the calling thread; it only
records the outcome and wakes the worker.

Deliveries may be completed in any order. The worker retires their outcomes
strictly in admission order, so ordered Operation reports never count a later
admission ahead of an earlier held one. A successful out-of-order completion is
ready for `TryTake()` immediately, which returns the lowest ready admission
sequence at that moment.

`start_paused` is a deterministic test control. While paused, the worker cannot claim
any destination slot; `Resume()` releases that gate permanently. This makes ingress
saturation and pre-evaluation rejection reproducible without timing assumptions. It
is not a production flow-control API.

## Event timestamps

Every admitted Record carries one wall-clock event timestamp with microsecond
resolution, as required by
[ADR 0004](adr/0004-reimplement-logging-as-a-pipeline.md). The producer reads the
clock exactly once per Record, after its ingress cell and worst-case byte charge
are reserved and before the message factory or macro operands run. The value is
stored in the immutable Record, so the worker, encoders, destinations, and
Operation reports all see that producer-captured value and never read the clock
again. Compile-erased, runtime-filtered, Null Logger, unregistered-producer,
admission-rejected, and post-Shutdown calls never read it.

`RuntimeConfig::event_clock` selects the clock when the Runtime is created; it
cannot change afterwards. The default, a null `now`, reads
`std::chrono::system_clock`. Tests inject deterministic time without sleeping:

```cpp
std::int64_t FixedNow(void*) noexcept { return 951'827'696'789'012; }

ulog::RuntimeConfig config;
config.event_clock = ulog::EventClock{nullptr, &FixedNow};
```

`now` runs on producer threads, so it must be thread-safe, non-blocking,
allocation-free, and must not log. `context` is passed through unchanged and must
outlive the Runtime. The seam is one indirect call through a pointer fixed at
creation: it adds no lock, allocation, or shared counter to the producer path.

`<ulog/timestamp.hpp>` formats a stored value as `YYYY-MM-DDTHH:MM:SS.ffffff`:
`FormatUtcTimestamp` for UTC and `FormatLocalTimestamp` for the process-local
zone. See [Native frontend](native-frontend.md#event-timestamps).

## Formats

`<ulog/format.hpp>` defines `ulog::Format` with the five baseline names `tskv`,
`ltsv`, `raw`, `json`, and `json_yadeploy`. `ParseFormat()` accepts exactly
those lowercase names; anything else is rejected, and `HowToFix()` lists the
accepted set. `ToString()` returns the canonical name.

Each encoded route selects its format at construction through
`RawFileRouteConfig::format` or `InMemoryEncodedDestinationConfig::format`. Both
default to `Format::kRaw`, so existing configurations keep their output. Every
format is available. A value that is not a `Format` enumerator fails `Create` as
`kUnsupportedFormat` for a file route, and the encoded destination constructor
throws `std::invalid_argument` for it.

A TSKV frame is one line:

```text
tskv<TAB>timestamp=2000-01-02T03:04:05.123456<TAB>level=INFO<TAB>module=Handle ( src/app.cpp:12 )<TAB>id=7<TAB>text=message<LF>
```

- `timestamp` is the Record's event timestamp in the process-local time zone with
  six microsecond digits, formatted as by `FormatLocalTimestamp()`. An instant the
  platform cannot convert, such as one before 1970 on Windows, leaves the value
  empty.
- `level` is the uppercase level name.
- `module` is `Function ( path:line )` from the Record's source location. The
  function name is the stored one: `SourceLocation::Current()` records the
  compiler's `std::source_location` name, which GCC, Clang, and MSVC render as a
  full signature, while `SourceLocation::Custom()` stores the name it is given.
- The Record's fields follow in Record order, then `text`.

LTSV writes the same pairs without the `tskv` prefix and separates each key from
its value with `:` instead of `=`. All three formats share the escaping and value
forms described under [Observing Raw bytes](#observing-raw-bytes); `:` is never
escaped. Raw is TSKV without `timestamp`, `level`, and `module`.

A JSON frame is one compact object followed by LF, with no whitespace outside
strings:

```text
{"timestamp":"2000-01-02T03:04:05.123456Z","level":"INFO","module":"Handle ( src/app.cpp:12 )","id":7,"text":"message"}<LF>
```

- `timestamp` is the event timestamp in UTC with six microsecond digits and a
  `Z` suffix, formatted as by `FormatUtcTimestamp()`. An instant outside years
  0000 through 9999 leaves the string empty.
- `level` and `module` are the same as in TSKV.
- Fields follow in Record order with their keys unchanged; JSON does not
  lowercase keys. Integers, finite floating-point values, booleans, and null
  stay JSON numbers, `true`/`false`, and `null`. NaN and infinities have no JSON
  form and are written as the strings `"nan"`, `"inf"`, and `"-inf"`.
- A raw-JSON field (`JsonString`) whose text is exactly one valid RFC 8259 value,
  nested at most 256 levels deep and with well-formed UTF-8, is inserted as that
  value with insignificant whitespace removed. Any other text, such as a
  truncated object, two values, or an invalid escape, is written as a JSON string
  of that text, so the frame stays valid JSON (`VAL-008`).
- `text` holds the message and comes last.

`json_yadeploy` writes the same object with `@timestamp`, `levelStr`, and
`message` in place of `timestamp`, `level`, and `text`.

Strings are escaped per RFC 8259: `"` and `\` get a backslash; backspace, form
feed, LF, CR, and tab use their short escapes; other bytes below 0x20 become
`\u00XX`; well-formed UTF-8 is copied unchanged. Each byte that does not start a
well-formed UTF-8 sequence becomes U+FFFD (`FMT-005`); overlong forms,
surrogates, and values above U+10FFFF are ill-formed. Messages are already cut at
their first ill-formed byte when the Record is published, so this rule applies to
keys, string values, and the source location.

Every Format shares one field-key namespace: the YaDeploy names `@timestamp`,
`levelStr`, and `message` are reserved like `timestamp`, `level`, and `text`, so
no user field can shadow a technical member in any format (`DEF-004`).

Encoding runs only on the worker, never allocates once warmed, and keeps
admission order. It sizes a frame before writing it, so a frame is written whole
or not at all. Each route buffer holds the worst-case frame of its format:

| Format | Worst-case frame bytes |
| --- | --- |
| Raw | `2 * maximum_record_bytes + 11` |
| TSKV | `2 * maximum_record_bytes + 87` |
| LTSV | `2 * maximum_record_bytes + 82` |
| JSON | `6 * maximum_record_bytes + 101` |
| YaDeploy JSON | `6 * maximum_record_bytes + 108` |

Every serialized byte of the message, source path, function name, field keys,
and string values encodes to at most two bytes. Each field carries 24 bytes of
serialized metadata, which cover its two separators and a number of at most 24
characters, so the variable part never exceeds twice the serialized Record. The
constant covers the prefix, the `text` key, the final LF, and for TSKV and LTSV
the `timestamp`, `level`, and `module` keys, a 26-byte timestamp, an 8-byte level
name, the module punctuation, and a 10-digit line number.

The JSON formats escape a byte to at most six bytes (`\u00XX`). A field's 24
metadata bytes cover its comma, key quotes, colon, value quotes, and a number of
at most 24 characters, and raw JSON is either copied without whitespace or
escaped as a string, so the variable part never exceeds six times the serialized
Record. The constant covers the braces, the final LF, the four technical
members' keys, quotes, colons, and commas, a 27-byte UTC timestamp, an 8-byte
level name, the module punctuation, and a 10-digit line number.

A frame that still did not fit would fail encoding with exact accounting and
never be written partially.

## Raw file route

`Runtime::Create(RuntimeConfig, RawFileRouteConfig)` builds one immutable route
that appends every accepted Record to a file in its `format`, Raw by default:

```cpp
auto created = ulog::Runtime::Create(
    ulog::RuntimeConfig{.threshold = ulog::Level::kInfo},
    ulog::RawFileRouteConfig{.path = "logs/app.log", .write_buffers = 8});
if (!created) {
  const ulog::RuntimeCreateFailure& failure = *created.failure;
  ReportConfigurationError(failure.Message(), failure.HowToFix(),
                           ulog::IoErrorName(failure.io_error));
  return;
}
```

The path is passed to libuv as UTF-8 on Windows and as native bytes elsewhere, so
Unicode names work on every supported platform. An empty path, a path containing
NUL, a path without a file name such as `logs/`, or a name without a UTF-8
representation fails as `kInvalidFilePath`.

The file is created when missing and opened for append, so existing bytes are
kept. Opening follows these rules:

- `create_parent_directories` (default `true`) creates missing parent directories
  when the open reports `ENOENT`. Only missing directories are created, with mode
  `0755` reduced by the process umask; existing directories keep their permissions.
  A creation failure is `kDirectoryCreateFailed`. With `false`, a missing parent
  fails the open as `kFileOpenFailed` with `ENOENT`.
- `truncate_on_start` (default `false`) empties an existing file when the route
  opens instead of appending after its content.
- When the opened file is not empty and its last byte is not `\n`, Ulog writes
  one `\n` before the first frame, so the first new Record starts on its own line.
  A file that ends with `\n`, an empty file, and a truncated file get no separator.
  If the last byte cannot be read, Ulog writes the separator. The separator is not a
  Record and is not counted in delivered bytes or Operation reports.

Every create, open, truncate, inspection, and separator step runs on the route's
I/O thread before `Create` returns.

`write_buffers` is the number of Runtime-owned complete-frame buffers, from 1
through 64. Each holds one frame of the route's [worst-case size](#formats), so
the setting bounds queued and in-flight appends and their bytes. The
buffers, their metadata, and the loop state are allocated during `Create` and
reported in `fixed_backing_bytes`.

`Create` validates the configuration, allocates the buffers, starts the worker,
and starts a dedicated I/O thread that owns a private libuv loop. That thread
opens the file before `Create` returns. A loop that cannot start fails as
`kIoLoopStartFailed`, a parent directory that cannot be created fails as
`kDirectoryCreateFailed`, and a file that cannot be opened, truncated, or separated
fails as `kFileOpenFailed`. These failures carry the negative libuv error in
`io_error`, and `HowToFix()` names the usual corrections, such as creating the
parent directory or granting write permission. Ulog never reads or changes the
process-wide `UV_THREADPOOL_SIZE`.

Producers only publish Records. The worker encodes each Record into a free buffer
and queues it; the I/O thread performs one `uv_fs_write` append at a time in
admission order and runs every filesystem callback. A short write continues from
the frame's known offset, so frames never interleave. When every buffer is queued,
being written, or waiting for the worker to retire its outcome, the worker stops
taking Records from ingress and producers shed later Records as drop-newest
without waiting.

A write error, or a write that makes no progress, stops the route. That Record is
counted as failed and is never retried or replayed, even though the file may keep
the prefix already written. Queued frames are cancelled, admission closes, pending
Drain, Durable Flush, and Shutdown Operations complete as `kFailed` with exact
reports, and
`RuntimeSnapshot::route_failed` and `route_io_error` expose the failure. Ulog
reports it only through these values: it throws nothing and writes nothing to
stderr. A later Runtime with a new file route is the recovery path until Reopen
exists.

## Drain, Durable Flush, Shutdown, and destruction

`Drain()`, `DurableFlush()`, and `Shutdown()` return the public
[`OperationStartResult`](operations.md). Their state comes from the fixed control
reserve, independent of payload capacity. Starting any of them can therefore
succeed when payload ingress is saturated, although it fails explicitly when every
control slot is already retained.

- `Drain()` captures the accepted-record watermark at the call and completes after
  every Record through that watermark has reached a terminal delivery outcome:
  delivered to the destination or failed. A held delivery before the watermark
  keeps Drain pending until it completes or fails. Drain does not require the
  application to remove Records that already fit, and it leaves admission open.
  Ready and observed Records still occupy destination slots: when the watermark
  exceeds the available slots, Drain waits for the application to call `TryTake()`
  and release enough observations for the remaining copies.
- On the file route, a Record reaches its terminal outcome when libuv reports that
  its whole frame was written to the file. Drain neither performs nor claims
  `fsync`, as defined by
  [ADR 0013](adr/0013-separate-drain-from-durable-file-flush.md).
- `DurableFlush()` captures a watermark like Drain. Once every Record through it has
  a terminal outcome, the I/O thread issues one `fsync` (`FlushFileBuffers` on
  Windows) for the file route, between appends, so it covers every completed
  write. The flush succeeds only when the OS reports success, and its report has
  `durability == OperationDurability::kSynced`. Concurrent flushes may share one
  `fsync`. Ulog promises nothing beyond what the OS reports for `fsync`.
- A failed `fsync` stops the file route like a write failure, because retrying it
  can report success after the OS dropped unwritten data. The flush and every other
  pending Operation complete as `kFailed`, and the libuv error is in
  `OperationReport::io_error` and `RuntimeSnapshot::route_io_error`.
- An in-memory destination has no durable semantics. `DurableFlush()` behaves as
  Drain there and reports `kNotApplicable`, never a durability claim. Later stream
  and network routes follow the same rule.
- A Durable Flush on a file route that is cancelled, fails, or starts after the
  route stopped reports `kNotSynced`; its outcome is never `kSucceeded`. Shutdown
  waits for flushes already started before it closes the file. Drain and Shutdown
  never call `fsync` and report `kNotRequested`.
- The first `Shutdown()` closes admission before returning, so later logging through
  an existing Logger is rejected before evaluation. The worker finishes every
  accepted Record, including held deliveries. The file route then closes its file
  and loop; a close failure completes Shutdown as `kFailed` after delivery.
  Shutdown then completes and the worker exits. Shutdown is not a durable file
  flush.
- `Shutdown(deadline)` takes a `std::chrono::steady_clock::time_point` and drains
  the same way until that deadline. If the barrier is not reached by then, the
  worker cancels the remaining work as destruction does: held deliveries and
  queued file appends are cancelled, retained ingress Records are discarded, and
  Shutdown completes as `kCancelled` with exact unfinished Records and bytes. A
  stalled destination, such as a held delivery or a stuck file write, therefore
  cannot keep Shutdown pending past its deadline. The worker checks the deadline
  at least every 10 ms. A Shutdown whose barrier is already reached succeeds even
  when its deadline has passed. `Shutdown()` without an argument has no deadline
  and keeps its previous behavior.
- An append already submitted to the operating system cannot be cancelled. After
  a cancelled Shutdown the I/O thread finishes that append, so the file may gain a
  frame that the report counted as unfinished, then closes the file.
- Every Shutdown of a Runtime completes with the same outcome and report: a later
  or concurrent call never starts a second stop, and its deadline can only bring
  the stop forward. Drain and Durable Flush started after a cancelled Shutdown
  complete as `kCancelled`.
- After Shutdown the Runtime's Logger reads threshold `kNone`, so later calls,
  including Default Logger calls that target it, are rejected before evaluation
  for the cost of one load. Calls that already passed the threshold when admission
  closed are rejected and counted as `rejected_no_producer`. The Logger state keeps
  its address while the Runtime is alive, as the
  [Default Logger lifetime contract](native-frontend.md#default-logger-exchange)
  requires.

Drain and Shutdown succeed when their barrier is reached, even if some deliveries
failed.
The [`OperationReport`](operations.md#completion-report) returned with the result
gives exact route accounting for every admitted sequence below the watermark:
processed, delivered, failed, and unfinished Records and encoded bytes. Records
admitted after the watermark are excluded, even when their held deliveries
completed earlier. Shutdown reports every admitted Record. A caller that needs the
outcome of one interval compares consecutive reports.

Runtime destruction is bounded best-effort cleanup, not an implicit successful
Drain. It closes admission, stops any destination wait, cancels held deliveries and
queued file appends, completes pending Operations as `kCancelled` with their exact
reports, and discards retained ingress Records. Cancelled deliveries and discarded
Records are reported as unfinished. An append already submitted to the operating
system cannot be cancelled: the I/O thread finishes that frame, closes the file,
and exits, so the file keeps only whole frames. Destruction waits at most
`destruction_timeout` for the worker and the I/O thread; a thread that has not
finished detaches while retaining shared internal state so that it cannot access
the destroyed Runtime object. Call `Shutdown()` and
observe its successful Operation when delivery is required, or
`Shutdown(deadline)` when the stop must also be bounded.

## Current boundary

Each Runtime intentionally has one immutable route, one worker, and drop-newest
admission. The private route selects the file route, the structured in-memory
copy, or the encoded in-memory destination at construction. Its fixed topology follows
[ADR 0015](adr/0015-keep-route-topology-immutable.md). The following remain later
roadmap work:

- generic Encoder, Sink, and ContextProvider extension seams;
- Reopen and rotation;
- network and IPC delivery;
- multiple routes, route reliability classes, and route-local budgets;
- batching, retry, reconnect, and durable flush; and
- alternative shedding policies.

Those future features must preserve the producer and memory contracts described in
the [Performance Contract](performance-contract.md) and
[ADR 0017](adr/0017-use-producer-credits-contiguous-records-and-producer-lanes.md).
