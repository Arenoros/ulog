# 11 — Select the producer kernel

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/12
Type: task
Status: open
Blocked by: Composed producer kernel
Labels: ready-for-agent

## Goal

Record the evidence-backed choice of reservation, Structured Record storage,
and ingress topology in an architectural decision that is precise enough for
production implementation without exposing private layout details.

## Acceptance criteria

- [ ] The decision cites committed workload and stress results for the selected
      composition and meaningful rejected alternatives.
- [ ] Trade-offs cover producer tail latency, sustained throughput, physical
      memory, logical accounting, fragmentation, and implementation complexity.
- [ ] The decision defines open-writer accounting, minimum charges, ownership
      transfer, publication linearization, and the initial drop-newest behavior.
- [ ] Relevant deterministic invariants and evidence-backed initial defaults
      are reflected in the Performance Contract.
- [ ] Future drop policies, routes, and progress reserve remain feasible without
      making the private Record representation public.

## Out of scope

- Implementing the selected design in production modules.
- Freezing a public ABI.
- Establishing final hardware-dependent performance thresholds.
- Selecting file, network, or route worker designs.

## Answer
