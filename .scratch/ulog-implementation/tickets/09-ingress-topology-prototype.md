# 09 — Ingress topology prototype

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/10
Type: prototype
Status: open
Blocked by: Performance workload harness; Reservation ledger prototype; Structured Record storage prototype
Labels: ready-for-agent

## Goal

Compare bounded MPSC ring, chunked MPSC, and per-producer-lane ingress designs
using owned prototype Records, and identify which topology provides the best
bounded producer handoff while retaining a clear admission order.

## Acceptance criteria

- [ ] Producer publication has a documented bounded action count and does not
      scan routes, encode output, or perform I/O.
- [ ] Admission sequence is assigned at one documented publication
      linearization point.
- [ ] FIFO behavior and exact enqueue, dequeue, reject, and retained-byte
      accounting match a simple model under randomized schedules.
- [ ] Near-full and saturated stress runs complete without corruption,
      deadlock, or unbounded producer work.
- [ ] Candidates run under the common workload matrix and pass the available
      thread-sanitizer configuration.

## Out of scope

- Choosing the final ingress topology.
- Implementing route fan-out.
- Implementing drop-oldest or adaptive thinning.
- Publishing queue or lane types.

## Answer
