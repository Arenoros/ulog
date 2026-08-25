# 09 — Ingress topology prototype

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/10
Type: prototype
Status: resolved
Blocked by: Performance workload harness; Reservation ledger prototype; Structured Record storage prototype
Labels: ready-for-agent

## Goal

Compare bounded MPSC ring, chunked MPSC, and per-producer-lane ingress designs
using owned prototype Records, and identify which topology provides the best
bounded producer handoff while retaining a clear admission order.

## Acceptance criteria

- [x] Producer publication has a documented bounded action count and does not
      scan routes, encode output, or perform I/O.
- [x] Admission sequence is assigned at one documented publication
      linearization point.
- [x] FIFO behavior and exact enqueue, dequeue, reject, and retained-byte
      accounting match a simple model under randomized schedules.
- [x] Near-full and saturated stress runs complete without corruption,
      deadlock, or unbounded producer work.
- [x] Candidates run under the common workload matrix and pass the available
      thread-sanitizer configuration.

## Out of scope

- Choosing the final ingress topology.
- Implementing route fan-out.
- Implementing drop-oldest or adaptive thinning.
- Publishing queue or lane types.

## Answer

`per-producer-lanes` is the best handoff prototype under the maintained
workload. It matched the minimum 11-action producer bound, admitted every
topology attempt, and does not depend on a timing distinction. The ring also
admitted every attempt but publishes a 70-action bound. Chunked publication is
bounded to 11 actions but rejected 0.1124% of balanced active topology attempts
on mapped-chunk contention, concentrated at 16 and 32 producers. Advisory
timing did not identify a stable latency or throughput winner.

Exact revision `f3c7e017f71546c6e41ed6d99b89908fcd10d15c` passed GitHub Actions,
including 22/22 tests under Clang 18 ThreadSanitizer. The controlled 2,520-row
run on `dockervm` validated 252,000,000 measured attempts and completed in 35:22
under a 3,600-second hard limit. Raw JSON SHA-256:
`913021da09bb280117e808446ab86785efe08cbbb7ccacc2b74a53ab48c377f0`.

This recommendation does not select the production topology. Stable producer
identity, lane-capacity partitioning, and the consumer scan remain explicit
trade-offs for the later architecture decision.
