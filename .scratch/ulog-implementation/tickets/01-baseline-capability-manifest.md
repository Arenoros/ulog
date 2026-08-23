# 01 — Baseline capability manifest

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/2
Type: research
Status: open
Blocked by: None
Labels: ready-for-agent

## Goal

Create a durable parity manifest that maps the observable logging capabilities
of the pinned userver baseline to their intended Ulog ownership and verification
strategy. A later agent must be able to select any feature ID and know what
behavior to preserve, where the evidence came from, and whether the capability
belongs in Ulog core, an optional package, or the downstream userver adapter.

## Acceptance criteria

- [ ] The manifest covers public logging APIs and macros, levels, formats,
      value-writing forms, destinations, lifecycle operations, Default Logger,
      startup capture, limited logging, dynamic debug, and stacktrace behavior.
- [ ] Every entry has a stable feature ID, baseline symbol or source reference,
      an observable behavior statement, an intended owner, and a future
      verification method.
- [ ] Known baseline defects and deliberate Ulog differences are distinguished
      from parity requirements and linked to the existing architectural
      decisions.
- [ ] Userver-specific capabilities are assigned to the downstream adapter
      without introducing a Ulog-to-userver dependency.
- [ ] Repository independence checks still pass and no regular build, test, or
      package step needs the userver source tree.

## Out of scope

- Implementing any logging behavior.
- Capturing golden output fixtures.
- Choosing private producer-kernel data structures.
- Adding compatibility APIs in the userver namespace.

## Answer
