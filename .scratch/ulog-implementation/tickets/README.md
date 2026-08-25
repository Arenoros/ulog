# Ulog implementation ticket catalog

This catalog is the approved first implementation wave for Ulog. It turns the
implementation specification into one-session tickets and stops at the first
public bounded in-memory Runtime tracer.

GitHub is canonical after migration:

- [Roadmap issue #1](https://github.com/Arenoros/ulog/issues/1)
- [Ulog implementation roadmap Project](https://github.com/users/Arenoros/projects/5)

The files in this directory are durable source snapshots for agent handoff.
Active status, claims, blockers, and completion live in GitHub.

## Working rules

- Claim only a ticket whose named blockers are resolved.
- Treat ticket numbers as catalog order, not as external tracker identifiers.
- Preserve blocker titles when tickets are migrated to another tracker.
- Record completed work under Answer before changing Status to resolved.
- Do not add a dependency on the userver source tree to regular Ulog builds,
  tests, packages, or CI.
- Revisit the downstream breakdown after the producer-kernel ADR, and again
  after the in-memory Runtime tracer. Those results intentionally gate the
  detailed file, route, network, and extension work.

## Ordered catalog

| Order | Ticket | GitHub | Blocked by |
| --- | --- | --- | --- |
| 01 | [Baseline capability manifest](01-baseline-capability-manifest.md) | [#2](https://github.com/Arenoros/ulog/issues/2) | None |
| 02 | [Offline baseline corpus harness](02-offline-baseline-corpus-harness.md) | [#3](https://github.com/Arenoros/ulog/issues/3) | Baseline capability manifest |
| 03 | [Text format golden corpus](03-text-format-golden-corpus.md) | [#4](https://github.com/Arenoros/ulog/issues/4) | Offline baseline corpus harness |
| 04 | [JSON format golden corpus](04-json-format-golden-corpus.md) | [#5](https://github.com/Arenoros/ulog/issues/5) | Offline baseline corpus harness |
| 05 | [Non-format parity scenarios](05-non-format-parity-scenarios.md) | [#6](https://github.com/Arenoros/ulog/issues/6) | Baseline capability manifest; Offline baseline corpus harness |
| 06 | [Performance workload harness](06-performance-workload-harness.md) | [#7](https://github.com/Arenoros/ulog/issues/7) | Baseline capability manifest |
| 07 | [Reservation ledger prototype](07-reservation-ledger-prototype.md) | [#8](https://github.com/Arenoros/ulog/issues/8) | Performance workload harness |
| 08 | [Structured Record storage prototype](08-structured-record-storage-prototype.md) | [#9](https://github.com/Arenoros/ulog/issues/9) | Text format golden corpus; JSON format golden corpus; Performance workload harness |
| 09 | [Ingress topology prototype](09-ingress-topology-prototype.md) | [#10](https://github.com/Arenoros/ulog/issues/10) | Performance workload harness; Reservation ledger prototype; Structured Record storage prototype |
| 10 | [Composed producer kernel](10-composed-producer-kernel.md) | [#11](https://github.com/Arenoros/ulog/issues/11) | Reservation ledger prototype; Structured Record storage prototype; Ingress topology prototype |
| 11 | [Select the producer kernel](11-select-producer-kernel.md) | [#12](https://github.com/Arenoros/ulog/issues/12) | Composed producer kernel |
| 12 | [Minimal native frontend](12-minimal-native-frontend.md) | [#13](https://github.com/Arenoros/ulog/issues/13) | Baseline capability manifest; Select the producer kernel |
| 13 | [Production bounded producer path](13-production-bounded-producer-path.md) | [#14](https://github.com/Arenoros/ulog/issues/14) | Select the producer kernel; Minimal native frontend |
| 14 | [Default Logger exchange](14-default-logger-exchange.md) | [#15](https://github.com/Arenoros/ulog/issues/15) | Production bounded producer path |
| 15 | [Basic LOG macro family](15-basic-log-macro-family.md) | [#16](https://github.com/Arenoros/ulog/issues/16) | Non-format parity scenarios; Production bounded producer path; Default Logger exchange |
| 16 | [Frontend performance gate](16-frontend-performance-gate.md) | [#17](https://github.com/Arenoros/ulog/issues/17) | Basic LOG macro family |
| 17 | [Operation state and control reserve](17-operation-state-and-control-reserve.md) | [#18](https://github.com/Arenoros/ulog/issues/18) | Production bounded producer path |
| 18 | [In-memory Runtime tracer](18-in-memory-runtime-tracer.md) | [#19](https://github.com/Arenoros/ulog/issues/19) | Frontend performance gate; Operation state and control reserve |

Resolving the producer-kernel decision enables the Minimal native frontend and
then the Production bounded producer path; both consume the private kernel
contract without exposing its prototype types or Record layout. GitHub issues
and the Project remain canonical for the active frontier and blocker state.
