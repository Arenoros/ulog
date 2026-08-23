# 17 — Operation state and control reserve

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/18
Type: task
Status: open
Blocked by: Production bounded producer path
Labels: ready-for-agent

## Goal

Provide the bounded Operation completion state and separate control reserve that
the first Runtime tracer can use for ordered Drain and Shutdown actions even
when the payload budget is fully occupied.

## Acceptance criteria

- [ ] Operation supports non-blocking polling, one completion callback, and
      explicit deadline-bounded waiting.
- [ ] Completion and callback registration races produce exactly one observable
      completion and at most one callback invocation.
- [ ] Callback execution does not block the owning worker or I/O thread.
- [ ] A control node can be acquired from its separate bounded reserve while
      the payload budget is exhausted.
- [ ] Operation state adds no lock, reference counting, allocation, or shared
      counter traffic to ordinary logging calls.
- [ ] Timeout and control-reserve exhaustion return explicit actionable
      failures.

## Out of scope

- Implementing Reopen or Durable Flush.
- Producing exact multi-route barrier reports.
- Remote acknowledgement.
- Starting a libuv loop.

## Answer
