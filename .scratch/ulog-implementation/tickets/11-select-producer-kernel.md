# 11 — Select the producer kernel

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/12
Type: task
Status: resolved
Blocked by: Composed producer kernel
Labels: ready-for-agent

## Goal

Record the evidence-backed choice of reservation, Structured Record storage,
and ingress topology in an architectural decision that is precise enough for
production implementation without exposing private layout details.

## Acceptance criteria

- [x] The decision cites committed workload and stress results for the selected
      composition and meaningful rejected alternatives.
- [x] Trade-offs cover producer tail latency, sustained throughput, physical
      memory, logical accounting, fragmentation, and implementation complexity.
- [x] The decision defines open-writer accounting, minimum charges, ownership
      transfer, publication linearization, and the initial drop-newest behavior.
- [x] Relevant deterministic invariants and evidence-backed initial defaults
      are reflected in the Performance Contract.
- [x] Future drop policies, routes, and progress reserve remain feasible without
      making the private Record representation public.

## Out of scope

- Implementing the selected design in production modules.
- Freezing a public ABI.
- Establishing final hardware-dependent performance thresholds.
- Selecting file, network, or route worker designs.

## Answer

[ADR 0017](../../../docs/adr/0017-use-producer-credits-contiguous-records-and-producer-lanes.md)
selects a private composition of producer credits backed by a central hard
bound, contiguous owned Records, and per-producer lanes. It defines rejection
before evaluation, worst-case open-writer reservation, a 64-byte minimum charge,
publication-time sequence linearization and ownership transfer, consumer-held
Record lifetime, initial drop-newest behavior, producer-slot retirement, and
separate payload, control, progress, and fixed-backing bounds.

The decision records the meaningful alternatives and their costs: a central
ledger avoids idle physical credit but loses selected accepted-concurrency tail
cells; chunked/hybrid storage has larger minimum charges and fragmentation;
the ring has a 70-action publication bound; and chunked ingress adds contention
rejection without stable timing benefit. The configurable initial profile is a
64-byte quantum, 16,384-byte maximum Record, 32 producer slots, and 64 ingress
cells. The 1 MiB benchmark capacity is explicitly not a production default.

The exact `28858895cc197ffa748baa2426720b0e9320ee82` composition passed the
bounded 32-producer/64-cell stress,
allocation checks, Clang ThreadSanitizer, and all hosted platforms. A controlled
`dockervm` run completed in 7:47 under a 1,200-second deadline and strictly
validated 840 rows and 84,000,000 attempts with zero deterministic errors. Raw
JSON SHA-256:
`0412a92d3b8b214d13e76d9e5cb25895748b1a14dacbc248bf2887331273f3ae`.
Hosted CI remains smoke-only and fail-closed through per-test, per-step, and
per-job timeouts.
