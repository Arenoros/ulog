# 18 — In-memory Runtime tracer

Parent roadmap: https://github.com/Arenoros/ulog/issues/1
GitHub issue: https://github.com/Arenoros/ulog/issues/19
Type: task
Status: open
Blocked by: Frontend performance gate; Operation state and control reserve
Labels: ready-for-agent

## Goal

Deliver the first public vertical logging scenario: an application creates a
validated Runtime, obtains a Logger, emits LOG_INFO through the bounded
producer path, observes the immutable Record at a Ulog test destination, and
completes through ordered Drain and Shutdown Operations.

## Acceptance criteria

- [ ] Runtime creation validates the complete initial configuration, reserves
      fixed pools, and starts its configured worker before returning a usable
      Logger.
- [ ] An installed-package consumer observes an accepted Record through a
      public Ulog test destination without accessing private Record storage.
- [ ] Saturation applies bounded drop-newest admission, does not evaluate a
      rejected message, and reports the deterministic outcome.
- [ ] Admission sequence and single-route FIFO are verified from publication
      through destination completion.
- [ ] Drain and Shutdown complete through Operation, including when the payload
      budget is saturated, and destruction never waits indefinitely.
- [ ] Static and shared builds, CMake and Conan consumers, sanitizers,
      independence checks, and supported-platform CI remain green.

## Out of scope

- Text encoding and built-in file, stdout, stderr, network, or IPC delivery.
- Multiple routes, batching, route-local budgets, and Best-Effort isolation.
- Drop-oldest, adaptive thinning, priority reserve, or explicit blocking.
- Public Encoder, Sink, or ContextProvider extension seams.

## Answer
