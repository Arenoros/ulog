# 15 — Basic LOG macro family

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/16
Type: task
Status: open
Blocked by: Non-format parity scenarios; Production bounded producer path; Default Logger exchange
Labels: ready-for-agent

## Goal

Deliver the complete basic level-specific and explicit-target LOG call-site
family identified by the parity manifest, with ULOG-owned compile controls and
the same disabled-path side-effect guarantees across supported compilers.

## Acceptance criteria

- [ ] Every basic macro feature ID in the manifest is implemented for unnamed
      and explicit-target forms, or is explicitly assigned to a named later
      advanced-frontend capability.
- [ ] Compile-erased, runtime-filtered, Null Logger, and admission-rejected
      calls do not evaluate message operands.
- [ ] An explicit target expression is evaluated once and unnamed forms perform
      one Default Logger load.
- [ ] Macro expansion behaves correctly in nested control flow and supports the
      native text and fmt-oriented forms available at this stage.
- [ ] Ulog compile controls use the ULOG prefix and no userver-owned
      configuration macro is exposed.
- [ ] Public package-consumer and behavior tests cover MSVC, GCC, and Clang.

## Out of scope

- Rate-limited macro variants and dropped-count reporting.
- Source-location dynamic debug.
- Optional stacktrace capture.
- Advanced range, map, chrono, or exception writers.

## Answer
