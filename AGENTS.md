# Ulog Agent Guide

## Scope

Ulog is a standalone, performance-oriented C++20 logging library. Keep Ulog
independent from userver in sources, build configuration, installation, and
regular tests. Userver may consume Ulog through adapters owned by userver.

## Engineering constraints

- Read `CONTEXT.md` and the relevant `docs/adr/` records before changing domain
  language or architecture.
- Read `docs/performance-contract.md` before changing the producer path,
  queues, memory ownership, routing, statistics, or I/O runtime.
- Keep public API under `include/ulog/` and include it as `<ulog/...>`.
- Use C++20, include what is used, and keep platform-specific code behind
  Ulog-owned interfaces.
- Update CMake and tests with every behavior change. Verify both supported
  library kinds when export or visibility code changes.
- Preserve application-lifetime address stability for every Logger state ever
  published as the Default Logger.
- Keep all Ulog-owned payload memory within configured budgets; document and
  test any memory excluded from that bound.
- Ask before deleting build directories, dependency caches, or generated
  package state.
- Leave changelogs unchanged unless the user explicitly requests an update.

## Completion

A change is complete when its public behavior is tested through the highest
available public seam, supported platform branches remain buildable, install
and external-consumer behavior remain valid when affected, and performance
invariants have deterministic checks or benchmark coverage.

## Agent skills

- For issue, specification, ticket, or wayfinding work, read
  `docs/agents/issue-tracker.md`.
- For triage work, read `docs/agents/triage-labels.md`.
- For glossary or architectural-decision work, read `docs/agents/domain.md`.
