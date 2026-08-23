# 13 — Production bounded producer path

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/14
Type: task
Status: open
Blocked by: Select the producer kernel; Minimal native frontend
Labels: ready-for-agent

## Goal

Move the selected reservation, Structured Record, and ingress design behind the
native Logger frontend so one producer call can filter, reserve, build, and
publish into the real bounded handoff without temporary public abstractions.

## Acceptance criteria

- [ ] Runtime level filtering and byte reservation happen before evaluation of
      the caller message expression.
- [ ] An accepted native scalar, string, or fmt-oriented write within documented
      limits performs no general-purpose heap allocation after warm-up.
- [ ] Abandoning a writer returns its complete reservation, while committing it
      transfers ownership and accounting exactly once.
- [ ] One wall-clock event timestamp is captured and admission sequence is
      assigned only at the publication linearization point.
- [ ] Producers do not call Encoder, Sink, libuv, filesystem, network, or route
      logic.
- [ ] Deterministic accounting, allocation, randomized stress, and available
      thread-sanitizer tests pass for the production path.

## Out of scope

- Starting Runtime workers.
- Encoding or delivering a Record.
- Implementing Default Logger exchange.
- Adding admission policies other than drop-newest.

## Answer
