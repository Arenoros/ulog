# 03 — Text format golden corpus

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/4
Type: task
Status: open
Blocked by: Offline baseline corpus harness
Labels: ready-for-agent

## Goal

Commit a normalized golden corpus for the TSKV, LTSV, and Raw observable output
contracts so future Ulog encoders can be tested without access to userver.

## Acceptance criteria

- [ ] The corpus includes empty and simple messages, Unicode, embedded control
      characters, escaping boundaries, source metadata, and timestamp behavior
      for each text format.
- [ ] Ordered fields, duplicate fields, frozen fields, and representative
      scalar values are covered where the baseline format supports them.
- [ ] Every fixture maps to one or more parity-manifest feature IDs and records
      any deliberate Ulog semantic difference.
- [ ] Fixture normalization is deterministic across repeated captures.
- [ ] Corpus integrity tests pass in a standalone Ulog checkout.

## Out of scope

- Implementing TSKV, LTSV, or Raw encoders.
- Capturing JSON or JSON YaDeploy fixtures.
- Benchmarking encoder throughput.
- Adding userver as a test dependency.

## Answer
