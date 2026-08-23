# Domain Documentation

Ulog has one domain context.

- Canonical terminology lives in the root `CONTEXT.md`.
- Architectural decisions live in the root `docs/adr/` directory.
- Update the glossary when a Ulog-specific term is resolved.
- Add an ADR only for a hard-to-reverse, non-obvious trade-off whose rationale
  would otherwise be lost.
- Cross-reference the pinned userver baseline as external evidence; never make
  userver a shipped or regular-test dependency of Ulog.
