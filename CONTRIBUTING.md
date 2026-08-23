# Contributing to Ulog

Read `AGENTS.md`, `CONTEXT.md`, the relevant architectural decisions, and the
Performance Contract before changing code. Production behavior changes require
tests through the highest public seam. Export or visibility changes require
both static and shared package-consumer checks.

## Local checks

Before submitting a change:

1. Run `cmake --build --preset dev --target ulog-format-check`.
2. Build and test the affected preset.
3. Run `ulog-check-independence` after dependency or build changes.
4. Use the complete Conan workflow when tests, dependencies, packaging, or
   benchmarks are affected.

Do not commit build directories, dependency caches, benchmark artifacts, or
`CMakeUserPresets.json`. Do not update changelogs unless a task explicitly asks
for it.

Until an external tracker is configured, planning work follows
`docs/agents/issue-tracker.md`.
