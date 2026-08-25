# 10 — Composed producer kernel

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/11
Type: prototype
Status: resolved
Blocked by: Reservation ledger prototype; Structured Record storage prototype; Ingress topology prototype
Labels: ready-for-agent

## Goal

Build one complete experimental path from admission reservation through Record
writing and publication to a consuming thread, so the individually promising
kernel choices are validated as a composition before production interfaces
depend on them.

## Acceptance criteria

- [x] Admission rejection occurs before any caller message expression or
      context callback is evaluated.
- [x] Every accepted writer can finish within its reservation and publishes one
      fully owned immutable Record.
- [x] Admission sequence is assigned only at publication and the consumer
      observes the documented FIFO order.
- [x] Logical and physical retained high-water remain within their declared
      limits during long randomized saturation runs.
- [x] The composed prototype passes allocation, accounting, stress, and common
      benchmark workloads as one system.

## Out of scope

- Shipping the prototype as production code.
- Defining Logger, Runtime, Encoder, or Sink public interfaces.
- Adding route fan-out or I/O.
- Selecting final release performance thresholds.

## Answer

Implemented in `61f2f08` with the GCC warnings-as-errors follow-up `2885889`.
The private `ComposedProducerPath` now claims a producer lane and reserves the
worst-case byte budget before invoking either caller callback, publishes one
owned contiguous Record, assigns its admission sequence at publication, and
holds the consumer claim until Record ownership and credit are released.

Verification covers rejection laziness, callback failure rollback, immutable
owned contents, publication-time sequence/FIFO, held consumer ownership,
exact one-block backing allocation, zero warm-path allocations, accounting,
the common benchmark matrix, and a watchdog-bounded 32-producer randomized
stress. The stress first fills all 64 lane cells to realize the exact logical
and physical structural ceilings, then validates every randomized attempt,
accepted/consumed Record, sequence, conservation identity, and final drain.

Execution evidence:

- local MSVC Release build with warnings-as-errors, 29/29 CTest in 42.76 s,
  130/130 Python tests, clang-format, cmake-format, and clean diff checks;
- GitHub Actions run
  [32898071823](https://github.com/Arenoros/ulog/actions/runs/32898071823) for
  exact SHA `28858895cc197ffa748baa2426720b0e9320ee82`:
  Linux 1:31, Linux TSan 1:28, quality 1:21, macOS 2:15, Windows 4:00;
- `arenoros@dockervm`, Debian x86_64, exact SHA `28858895...`, GCC 12 with
  warnings-as-errors, Docker limited to 2 CPUs/3 GiB and a 600-second external
  deadline: targeted build plus 6/6 composed CTest passed; test time 0.29 s.

Hosted execution is fail-closed: all 37 Actions steps have explicit timeouts,
jobs are capped at 15-20 minutes, all 29 maintained CTests have timeouts, and
the composed stress also has an 8-second internal watchdog. Hosted CI runs
only the 120-row smoke protocol; the 840-row controlled registry is list-only,
and controlled benchmark bodies were intentionally not run for this ticket.
