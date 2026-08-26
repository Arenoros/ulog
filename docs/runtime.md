# In-memory Runtime tracer

The installed package provides a concrete, bounded `ulog::Runtime` that moves
accepted native Records through one FIFO worker route into
`ulog::testing::InMemoryDestination`. This is the first executable Runtime seam:
it makes admission, ownership, ordering, control, and shutdown behavior observable
without introducing a generic destination abstraction or asynchronous I/O.

Include the public interfaces with:

```cpp
#include <ulog/log.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_destination.hpp>
```

## Construction and ownership

Construct the destination first, then pass a copy to `Runtime::Create`. Destination
copies share the same bounded state, so the application can keep its copy for
observation while Runtime owns another:

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
[ADR 0012](adr/0012-own-runtime-and-libuv-lifecycle-explicitly.md) without adding the
later libuv loop.

The current tracer accepts these Runtime bounds:

- `threshold` is `kTrace` through `kNone`;
- `maximum_record_bytes` is a 64-byte multiple from 128 through 16,384;
- `payload_capacity_bytes` is a 64-byte multiple large enough for one maximum-sized
  Record;
- `producer_slots` is from 1 through 32, and `ingress_cells` is from
  `producer_slots` through 64;
- `control_operations` is from 1 through 64;
- `worker_threads` is exactly 1;
- startup and destruction timeouts are positive and no greater than 24 hours; and
- the destination has non-zero capacity and a `maximum_record_bytes` at least as
  large as the Runtime value.

`RuntimeSnapshot` exposes weakly consistent admission, delivery, rejection, retained
payload, and lifecycle counters. `fixed_backing_bytes` reports the capacity-scaled
ingress Record slots, destination payload and slot metadata, control nodes, and
action table separately from live retained bytes. It is stable for a Runtime but is
not a process-memory total: constant-size Runtime objects, allocation bookkeeping,
platform synchronization objects, and thread stacks are excluded.

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

`start_paused` is a deterministic test control. While paused, the worker cannot claim
any destination slot; `Resume()` releases that gate permanently. This makes ingress
saturation and pre-evaluation rejection reproducible without timing assumptions. It
is not a production flow-control API.

## Drain, Shutdown, and destruction

`Drain()` and `Shutdown()` return the public
[`OperationStartResult`](operations.md). Their state comes from the fixed control
reserve, independent of payload capacity. Starting either action can therefore
succeed when payload ingress is saturated, although it fails explicitly when every
control slot is already retained.

- `Drain()` captures the accepted-record watermark at the call and completes after
  every Record through that watermark has been copied into the destination. It does
  not require the application to remove Records that already fit, and it leaves
  admission open. Ready and observed Records still occupy destination slots: when
  the watermark exceeds the available slots, Drain waits for the application to
  call `TryTake()` and release enough observations for the remaining copies.
- The first `Shutdown()` closes admission before returning, so later logging through
  an existing Logger is rejected before evaluation. The worker delivers all accepted
  Records, completes the action, and exits. Shutdown is not a durable file flush; the
  distinction remains defined by
  [ADR 0013](adr/0013-separate-drain-from-durable-file-flush.md).

Runtime destruction is bounded best-effort cleanup, not an implicit successful
Drain. It closes admission, stops any destination wait, cancels pending Operations,
and discards retained ingress Records. It waits at most `destruction_timeout` for the
worker; if the worker has not reported completion, the thread detaches while retaining
shared internal state so that it cannot access the destroyed Runtime object. Call
`Shutdown()` and observe its successful Operation when delivery is required.

## Current boundary

This tracer intentionally has one immutable route, one worker, drop-newest admission,
and the bounded in-memory test destination. Its fixed topology follows
[ADR 0015](adr/0015-keep-route-topology-immutable.md). The following remain later
roadmap work:

- generic Encoder, Sink, and ContextProvider extension seams;
- filesystem, network, IPC, or libuv-backed delivery;
- multiple routes, route reliability classes, and route-local budgets;
- batching, retry, reconnect, and durable flush; and
- alternative shedding policies.

Those future features must preserve the producer and memory contracts described in
the [Performance Contract](performance-contract.md) and
[ADR 0017](adr/0017-use-producer-credits-contiguous-records-and-producer-lanes.md).
