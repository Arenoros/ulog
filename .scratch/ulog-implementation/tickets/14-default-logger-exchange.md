# 14 — Default Logger exchange

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/15
Type: task
Status: open
Blocked by: Production bounded producer path
Labels: ready-for-agent

## Goal

Let applications atomically install and replace the process-wide Default Logger
without adding locks, reference counting, reclamation, or quiescence waits to
ordinary unnamed logging calls.

## Acceptance criteria

- [ ] An unnamed logging call loads the Default Logger target exactly once and
      completes entirely against that loaded target.
- [ ] Installing a new target performs an atomic exchange with no lock,
      reference-count operation, or producer quiescence wait.
- [ ] A producer that loaded the previous target before exchange can safely
      finish against the still-live Logger state.
- [ ] The application-lifetime address-stability requirement for every
      published target is documented at the public API boundary.
- [ ] Race and stress tests cover repeated exchanges and stale producers without
      adding a forwarding check to ordinary Runtime Logger calls.

## Out of scope

- Reclaiming or destroying previously published Logger states.
- Bootstrap Logger forwarding.
- Runtime topology reconfiguration.
- Implementing rate-limited or dynamic-debug macros.

## Answer
