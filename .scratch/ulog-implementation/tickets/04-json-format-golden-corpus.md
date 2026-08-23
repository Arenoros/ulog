# 04 — JSON format golden corpus

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/5
Type: task
Status: open
Blocked by: Offline baseline corpus harness
Labels: ready-for-agent

## Goal

Commit a normalized golden corpus for JSON and JSON YaDeploy output semantics so
future structured encoders have an independent, reviewable behavioral oracle.

## Acceptance criteria

- [ ] Cases cover strings, signed and unsigned integers, floating-point values,
      booleans, nulls, Unicode, escaping, source metadata, and timestamps.
- [ ] Ordered, duplicate, frozen, and nested structured fields are represented
      where supported by the baseline.
- [ ] Comparisons preserve only documented ordering guarantees and do not hide
      meaningful structural differences through over-normalization.
- [ ] Every fixture maps to parity-manifest feature IDs and records deliberate
      Ulog differences explicitly.
- [ ] Corpus integrity tests pass without a userver checkout.

## Out of scope

- Implementing either JSON encoder.
- Choosing the private Structured Record layout.
- Introducing a general-purpose JSON dependency.
- Benchmarking structured encoding.

## Answer
