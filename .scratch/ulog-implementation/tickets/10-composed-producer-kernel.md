# 10 — Composed producer kernel

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/11
Type: prototype
Status: open
Blocked by: Reservation ledger prototype; Structured Record storage prototype; Ingress topology prototype
Labels: ready-for-agent

## Goal

Build one complete experimental path from admission reservation through Record
writing and publication to a consuming thread, so the individually promising
kernel choices are validated as a composition before production interfaces
depend on them.

## Acceptance criteria

- [ ] Admission rejection occurs before any caller message expression or
      context callback is evaluated.
- [ ] Every accepted writer can finish within its reservation and publishes one
      fully owned immutable Record.
- [ ] Admission sequence is assigned only at publication and the consumer
      observes the documented FIFO order.
- [ ] Logical and physical retained high-water remain within their declared
      limits during long randomized saturation runs.
- [ ] The composed prototype passes allocation, accounting, stress, and common
      benchmark workloads as one system.

## Out of scope

- Shipping the prototype as production code.
- Defining Logger, Runtime, Encoder, or Sink public interfaces.
- Adding route fan-out or I/O.
- Selecting final release performance thresholds.

## Answer
