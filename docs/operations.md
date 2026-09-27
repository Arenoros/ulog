# Operations

`ulog::Operation` is the move-only completion handle for ordered Runtime
actions. The public [Runtime](runtime.md) uses it for Drain and Shutdown. Operation state is allocated from a control reserve that is
independent of producer payload credits, so a control action can still start
when retained Records fill the payload budget.

Include the interface with:

```cpp
#include <ulog/operation.hpp>
```

## Observing completion

`Poll()` never waits. It returns `kPending` until one immutable terminal result
is published and `kCompleted` thereafter. A default-constructed or moved-from
handle returns `kInvalidOperation`.

`WaitUntil()` accepts a `std::chrono::steady_clock::time_point`. A deadline
result leaves the Operation active: the caller may poll or wait again, and a
registered callback remains armed. If completion and the deadline meet at the
boundary, an already published completion is observed. Ulog-owned worker, I/O,
and callback threads receive `kForbiddenThread` instead of blocking.

Every non-success wait result provides `Message()` and `HowToFix()`. These are
static non-owning strings and do not allocate.

## Completion report

`OperationResult::Outcome()` reports the terminal state of the action, and
`OperationResult::Report()` returns an `OperationReport` captured with it. The
report is exact single-route accounting for every admitted Record whose sequence
is below the Operation watermark:

- `watermark_records` is the number of admitted Records covered;
- `processed_records` and `processed_bytes` count Records the route took from
  ingress and, for an encoding route, encoded;
- `delivered_*` and `failed_*` count terminal delivery outcomes; and
- `unfinished_records` and `unfinished_bytes` cover Records without a terminal
  outcome when the action completed, such as cancelled deliveries or discarded
  ingress Records.

`delivered_records + failed_records + unfinished_records` always equals
`watermark_records`. Bytes are encoded route bytes, so a Record that was never
encoded contributes an unfinished Record but no unfinished bytes, and a route
without an encoder reports zero bytes. Records rejected before admission have no
sequence and appear only in weakly consistent Runtime statistics, as required by
[ADR 0013](adr/0013-separate-drain-from-durable-file-flush.md).

The result, including its report, is fixed-size and stored inline in the control
node; publishing, polling, waiting for, or delivering it to a callback performs no
general-purpose heap allocation.

## Completion callback

`OnComplete()` accepts one callable with this shape:

```cpp
operation.OnComplete([](const ulog::OperationResult& result) noexcept {
  if (result.Outcome() == ulog::OperationOutcome::kSucceeded) {
    // Observe completion without blocking a Runtime worker.
  }
});
```

The callable is owned inline in the preallocated control node. Its stored type
must fit `kOperationCallbackInlineBytes`, use no over-alignment, be nothrow
constructible and movable, have a nothrow destructor, and be invocable as
`void(const OperationResult&) noexcept`. A null function pointer is rejected as
`kInvalidCallback` without consuming the callback slot.

Exactly one callback may be registered. Registration before or after completion
is accepted and remains asynchronous. Completion only queues the ready task;
user callback code, including its move and destructor behavior, runs outside
every Operation and reserve lock. A second registration returns
`kAlreadyRegistered`.

Captured references and the module that instantiated the callback must remain
valid until callback delivery finishes. Before unloading a plugin or shared
library, finish or cancel its Operations and wait for their callbacks.

## Bounded reserve behavior

Each accepted action retains one control slot until all three possible owners
are finished: the internal action, the public Operation handle, and callback
delivery. A completed handle intentionally retains its slot until the handle is
destroyed or move-assigned.

When no slot is available, the action does not start. Its
`OperationStartFailure` reports `kControlReserveExhausted`, the configured
capacity, current occupancy, and actionable `Message()` / `HowToFix()` text.
Callers can release completed handles, wait for in-flight actions, or increase
the Runtime control-operation capacity.

The reserve and dispatcher allocate their fixed backing during construction.
After warm-up, starting, polling, registering a small callback, completing,
dispatching, and recycling an Operation perform no general-purpose heap
allocation. None of these types or dependencies is present in Logger or
producer source files; the frontend structural gate enforces that direction.

## Runtime actions

`Runtime::Drain()` captures an accepted-record watermark and completes after the
single worker has retired a terminal delivery outcome for every Record through
that watermark in its Raw file route or in-memory test destination. On the file
route that outcome is local write completion, not `fsync`. It leaves admission
open. `Runtime::Shutdown()` closes admission, finishes all
already accepted Records, completes, and stops the worker. Both succeed once
their barrier is reached; failed deliveries are counted in the report rather
than turning the barrier into `kFailed`. A successful Drain or Shutdown does not
mean an application has taken or released the destination's observed Records.

`kFailed` reports a route that stopped after an internal encoding failure or a
file write failure; its report counts the failing Record as failed and the
remaining work as unfinished. Every pending Drain, including one whose watermark
the failing Record just reached, completes as `kFailed`. A Shutdown whose file
fails to close also completes as `kFailed`, after reporting every Record as
delivered.

Runtime destruction is a separate bounded cancellation path. Pending actions
complete as `kCancelled` with reports of the work that finished first; callers
that require delivery must explicitly start Shutdown and observe its successful
terminal result before destroying Runtime.
