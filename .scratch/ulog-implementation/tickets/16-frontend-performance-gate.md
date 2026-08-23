# 16 — Frontend performance gate

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/17
Type: task
Status: open
Blocked by: Basic LOG macro family
Labels: ready-for-agent

## Goal

Turn the producer-frontend performance contract into permanent deterministic
checks and comparable benchmark output before Runtime delivery work builds on
the hot path.

## Acceptance criteria

- [ ] Compile-erased, runtime-filtered, Null Logger, admission-rejected, and
      ordinary accepted paths are measured separately.
- [ ] Deterministic instrumentation proves zero Ulog general-purpose heap
      allocations for promised disabled and warmed accepted paths.
- [ ] Checks demonstrate one Default Logger target load and no shared accepted
      statistics-counter traffic on an ordinary call.
- [ ] Benchmark output follows the common workload result protocol and includes
      latency quantiles and throughput metrics.
- [ ] Hosted-runner timing remains advisory, while allocation, evaluation, and
      accounting invariant failures are CI-blocking.

## Out of scope

- Setting final hardware-dependent release thresholds.
- Measuring Encoder, file, or network throughput.
- Implementing Runtime delivery.
- Adding statistics aggregation.

## Answer
