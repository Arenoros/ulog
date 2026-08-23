# 05 — Non-format parity scenarios

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/6
Type: research
Status: open
Blocked by: Baseline capability manifest; Offline baseline corpus harness
Labels: ready-for-agent

## Goal

Record deterministic setup, action, and observation scenarios for baseline
capabilities that cannot be specified by comparing one encoded output record.
The scenario catalog will serve as the behavioral oracle for later frontend,
lifecycle, and diagnostics tickets.

## Acceptance criteria

- [ ] Scenarios cover compile-time and runtime filtering, Default Logger
      behavior, limited logging and dropped counts, dynamic debug, reopen,
      flush, and startup forwarding.
- [ ] Each scenario states its inputs, observable result, applicable feature
      IDs, and whether Ulog intentionally differs from the baseline.
- [ ] Timing-sensitive behavior is expressed through deterministic state or
      barrier observations rather than sleeps.
- [ ] Ambiguous or defective baseline behavior is identified as an explicit
      decision point instead of silently becoming a requirement.
- [ ] Regular scenario validation does not execute userver.

## Out of scope

- Implementing the scenarios in Ulog.
- Selecting producer-kernel internals.
- Defining remote acknowledgement semantics.
- Implementing framework-owned userver integration.

## Answer
