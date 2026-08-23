# 07 — Reservation ledger prototype

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/8
Type: prototype
Status: open
Blocked by: Performance workload harness
Labels: ready-for-agent

## Goal

Compare bounded reservation and byte-ledger designs, including a central pool
and producer-local credits backed by a central bound, and produce evidence for
the design that best protects producer tail latency without weakening exact
memory accounting.

## Acceptance criteria

- [ ] Open writers are charged against the hard payload bound from successful
      reservation until abandonment or ownership transfer.
- [ ] A successful worst-case reservation guarantees that its writer can finish
      the Record or apply the documented valid truncation without new capacity.
- [ ] A rejected reservation invokes no message-building callback.
- [ ] Commit, abandon, credit refill, and credit return transitions match a
      simple randomized accounting oracle.
- [ ] Every candidate is measured through the common workload matrix and
      reports logical and physical retained high-water separately.

## Out of scope

- Choosing the final reservation design.
- Publishing reservation types as public API.
- Implementing drop-oldest, thinning, or blocking admission.
- Encoding or delivering Records.

## Answer
