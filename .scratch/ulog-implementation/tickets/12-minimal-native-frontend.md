# 12 — Minimal native frontend

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/13
Type: task
Status: open
Blocked by: Baseline capability manifest; Select the producer kernel
Labels: ready-for-agent

## Goal

Expose the smallest native Ulog frontend needed for applications to name
levels, capture source locations, hold a Logger handle, and safely call the
initial static Null Logger before a Runtime exists.

## Acceptance criteria

- [ ] Public Level, SourceLocation, and Logger concepts are available from an
      installed static or shared Ulog package.
- [ ] The process-wide initial Default Logger targets an address-stable static
      Null Logger.
- [ ] Compile-time-erased and Null Logger calls do not evaluate the message
      expression, allocate, take locks, or perform ownership reclamation.
- [ ] The public namespace and configuration names are Ulog-owned, with no
      userver namespace or USERVER_LOG configuration macros.
- [ ] Package-consumer tests exercise the initial frontend through public
      headers on supported compiler families.

## Out of scope

- Constructing a Runtime.
- Delivering an accepted Record.
- Implementing the complete LOG macro family.
- Exchanging the Default Logger target.

## Answer
