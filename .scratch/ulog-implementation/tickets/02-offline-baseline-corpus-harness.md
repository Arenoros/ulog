# 02 — Offline baseline corpus harness

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/3
Type: task
Status: open
Blocked by: Baseline capability manifest
Labels: ready-for-agent

## Goal

Provide an explicit offline workflow that can capture normalized behavioral
fixtures from exactly the pinned userver revision, while keeping committed Ulog
fixtures independently verifiable during every normal build and test run.

## Acceptance criteria

- [ ] The capture workflow requires an explicit userver source location and
      rejects a checkout whose revision does not match the migration baseline.
- [ ] Nondeterministic timestamp, source-path, process, thread, and platform
      values are normalized by documented, deterministic rules.
- [ ] Captured cases include schema version, provenance, feature IDs, and an
      integrity value sufficient to detect accidental fixture edits.
- [ ] A regular Ulog test validates the committed corpus schema and integrity
      without locating or executing userver.
- [ ] Normal CMake, Conan, packaging, installation, and CI paths remain
      independent of the external baseline.

## Out of scope

- Populating all format fixtures.
- Implementing a Ulog encoder.
- Running the capture workflow automatically in CI.
- Treating baseline defects as required behavior.

## Answer
