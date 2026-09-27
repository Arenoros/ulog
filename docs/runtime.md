# Runtime

The installed package provides a concrete, bounded `ulog::Runtime` that moves
accepted native Records through one FIFO worker route. The production route is the
[Raw file route](#raw-file-route), which appends encoded frames to a file through
Ulog's private libuv loop. Two in-memory test destinations,
`ulog::testing::InMemoryDestination` and
`ulog::testing::InMemoryEncodedDestination`, make the same pipeline observable. The structured destination exposes
Record contents; the encoded destination runs the built-in Raw encoder on the
worker, submits each frame as one serialized delivery, and exposes exact bytes.
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

- `threshold` is `kTrace` through `kNone`;
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

The fixed destination backing and drop-newest behavior are the concrete tracer form
of [ADR 0010](adr/0010-bound-pipeline-memory-and-configure-shedding.md).

### Observing Raw bytes

`InMemoryEncodedDestination` has the same one-shot attachment, pause, FIFO, and
slot-pinning behavior. Its `maximum_record_bytes` is the Structured Record bound,
not an independently undersized output limit. Construction derives each encoded
slot as `2 * maximum_record_bytes + 11` bytes, checks all arithmetic before
allocation, and reports the value through `MaximumEncodedRecordBytes()`. This
reserve covers full Raw escaping and framing, so every valid Record either commits
one complete frame or triggers the terminal internal-failure path; it is never
silently truncated a second time.

`TryTake()` returns a move-only `ObservedEncodedRecord`. `Bytes()` remains valid
until that observation is moved from or destroyed. Raw frames begin with `tskv`,
append ordered fields as tab-separated `key=value` pairs, append `text` last, and
end with exactly one LF. NUL, tab, CR, LF, and backslash use TSKV escaping; `=` is
also escaped in keys. Raw intentionally omits timestamp, level, module, and source
metadata. Encoding executes synchronously inside the worker's consume callback;
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

## Raw file route

`Runtime::Create(RuntimeConfig, RawFileRouteConfig)` builds one immutable Raw route
that appends every accepted Record to a file:

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

The path is created when missing and always opened for append; existing bytes are
kept. It is passed to libuv as UTF-8 on Windows and as native bytes elsewhere, so
Unicode names work on every supported platform. An empty path, a path containing
NUL, a path without a file name such as `logs/`, or a name without a UTF-8
representation fails as `kInvalidFilePath`.

`write_buffers` is the number of Runtime-owned complete-frame buffers, from 1
through 64. Each holds one Raw frame of up to `2 * maximum_record_bytes + 11`
bytes, so the setting bounds queued and in-flight appends and their bytes. The
buffers, their metadata, and the loop state are allocated during `Create` and
reported in `fixed_backing_bytes`.

`Create` validates the configuration, allocates the buffers, starts the worker,
and starts a dedicated I/O thread that owns a private libuv loop. That thread
opens the file before `Create` returns. A loop that cannot start fails as
`kIoLoopStartFailed`; a file that cannot be opened fails as `kFileOpenFailed`.
Both failures carry the negative libuv error in `io_error`, and `HowToFix()` names
the usual corrections, such as creating the parent directory or granting write
permission. Ulog never reads or changes the process-wide `UV_THREADPOOL_SIZE`.

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
Drain and Shutdown Operations complete as `kFailed` with exact reports, and
`RuntimeSnapshot::route_failed` and `route_io_error` expose the failure. Ulog
reports it only through these values: it throws nothing and writes nothing to
stderr. A later Runtime with a new file route is the recovery path until Reopen
exists.

## Drain, Shutdown, and destruction

`Drain()` and `Shutdown()` return the public
[`OperationStartResult`](operations.md). Their state comes from the fixed control
reserve, independent of payload capacity. Starting either action can therefore
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
- The first `Shutdown()` closes admission before returning, so later logging through
  an existing Logger is rejected before evaluation. The worker finishes every
  accepted Record, including held deliveries. The file route then closes its file
  and loop; a close failure completes Shutdown as `kFailed` after delivery.
  Shutdown then completes and the worker exits. Shutdown is not a durable file
  flush.

Both actions succeed when their barrier is reached, even if some deliveries failed.
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
observe its successful Operation when delivery is required.

## Current boundary

Each Runtime intentionally has one immutable route, one worker, and drop-newest
admission. The private route selects the Raw file route, the structured in-memory
copy, or the Raw-encoded in-memory destination at construction. Its fixed topology follows
[ADR 0015](adr/0015-keep-route-topology-immutable.md). The following remain later
roadmap work:

- generic Encoder, Sink, and ContextProvider extension seams;
- file truncation, Reopen, rotation, and Durable Flush;
- network and IPC delivery;
- multiple routes, route reliability classes, and route-local budgets;
- batching, retry, reconnect, and durable flush; and
- alternative shedding policies.

Those future features must preserve the producer and memory contracts described in
the [Performance Contract](performance-contract.md) and
[ADR 0017](adr/0017-use-producer-credits-contiguous-records-and-producer-lanes.md).
