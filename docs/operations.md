# Operations

`ulog::Operation` is the move-only completion handle for ordered Runtime
actions. The first public Runtime will use it for Drain and Shutdown. Operation
state is allocated from a control reserve that is independent of producer
payload credits, so a control action can still start when retained Records fill
the payload budget.

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
