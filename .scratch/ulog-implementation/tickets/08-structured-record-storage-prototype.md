# 08 — Structured Record storage prototype

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/9
Type: prototype
Status: open
Blocked by: Text format golden corpus; JSON format golden corpus; Performance workload harness
Labels: ready-for-agent

## Goal

Compare contiguous, chunked, and hybrid private Structured Record storage under
the maintained workloads and parity examples, preserving full ownership and
ordered typed data without making the private representation a public format.

## Acceptance criteria

- [ ] Each candidate owns message, source information, event timestamp, and
      ordered typed fields with no surviving reference to caller storage.
- [ ] A published Record is immutable and can be consumed safely after the
      producer returns.
- [ ] Native scalar, string, and fmt-oriented writing paths are represented,
      together with a viable UTF-8 truncation boundary.
- [ ] Warm ordinary writes within documented prototype limits make no
      general-purpose heap allocation.
- [ ] Measurements report payload, metadata, fragmentation, minimum accounting
      charge, and benchmark results for every candidate.

## Out of scope

- Choosing the final Record layout.
- Exposing Record storage or binary parsing as public API.
- Implementing all advanced value writers.
- Implementing any output encoder.

## Answer
