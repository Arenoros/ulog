# 06 — Performance workload harness

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/7
Type: task
Status: open
Blocked by: Baseline capability manifest
Labels: ready-for-agent

## Goal

Create a common benchmark and result protocol that lets reservation, Record
storage, and ingress candidates be compared under the same representative
producer workloads without committing their details to the public API.

## Acceptance criteria

- [ ] The workload matrix exercises 1, 2, 4, 8, 16, and 32 producers, multiple
      Record sizes, and empty, partial, near-full, and saturated occupancy.
- [ ] Machine-readable results include p50, p99, and p99.9 producer latency,
      Records per second, bytes per second, CPU use, allocation counts, and
      retained high-water.
- [ ] The harness exposes a short cross-platform smoke mode and a documented
      controlled-run mode for meaningful timing comparisons.
- [ ] Workload generation and metric collection are reusable by all three
      kernel prototypes without exposing a production interface.
- [ ] Hosted-runner timings remain advisory, while deterministic accounting and
      allocation failures can fail CI.

## Out of scope

- Selecting a winning kernel design.
- Establishing final release latency thresholds.
- Implementing public logging APIs.
- Benchmarking file or network I/O.

## Answer
