Status: open
Type: task
Labels: ready-for-agent

# Ulog Repository Bootstrap

## Problem Statement

The target Ulog directory lacks the repository, build, packaging, quality, CI,
benchmark, and agent-navigation infrastructure required to safely begin a
performance-oriented behavioral reimplementation of userver logging. Beginning
production migration in that state would mix foundational tooling decisions
with logging behavior and make independence, portability, installability, and
performance invariants difficult to verify.

## Solution

Create a standalone public C++20 project skeleton for Ulog that builds and
installs as static or shared, has cross-platform dependency and CI workflows,
provides test and benchmark entry points, and gives agents durable repository
conventions and architectural context. Establish automated boundaries that keep
Ulog independent from userver and preserve the pinned userver revision only as
external migration evidence. Stop before implementing production logging.

## User Stories

1. As a Ulog contributor, I want a clean Git repository on the `develop` branch,
   so that development starts from an explicit history and branch convention.
2. As an open-source consumer, I want clear project, license, support, and
   contribution documentation, so that I can evaluate and use the project.
3. As a C++ consumer, I want Ulog to require C++20 explicitly, so that compiler
   and language assumptions are deterministic.
4. As a library integrator, I want a conventional namespaced CMake target, so
   that build-tree and installed-package consumption use the same target name.
5. As a static-linking consumer, I want static Ulog to be the default, so that
   the default build matches the selected distribution model.
6. As a shared-library consumer, I want export visibility to be validated, so
   that Windows DLL and ELF/Mach-O shared builds are first-class.
7. As a package consumer, I want install and package configuration support, so
   that an external project can use `find_package` without source-tree paths.
8. As a dependency maintainer, I want Conan 2 metadata and locked profiles, so
   that development and CI resolve compatible dependencies consistently.
9. As a Linux contributor, I want GCC and Clang workflows, so that compiler
   portability is checked early.
10. As a Windows contributor, I want an MSVC workflow, so that native Windows
    remains a release constraint rather than a later port.
11. As a macOS contributor, I want an AppleClang workflow, so that supported
    POSIX behavior is not inferred from Linux alone.
12. As a maintainer, I want unit, integration, package-consumer, stress, and
    benchmark target categories, so that later logging work has stable quality
    seams from its first implementation.
13. As a performance maintainer, I want allocation and bounded-memory test
    utilities plus benchmark result artifacts, so that the Performance Contract
    can become an enforceable release criterion.
14. As a pull-request author, I want deterministic performance invariants gated
    separately from advisory timing measurements, so that CI detects real
    regressions without flaky hosted-runner thresholds.
15. As an architect, I want a machine-checkable no-userver dependency boundary,
    so that accidental headers, targets, or package requirements fail quickly.
16. As an agent, I want concise repository instructions and context pointers, so
    that I can locate domain decisions, tickets, and performance rules without
    rediscovering conventions.
17. As a planning agent, I want a documented local issue workflow, so that work
    remains claimable and reviewable before an external tracker is configured.
18. As a migration engineer, I want the extraction baseline recorded, so that
    later parity work compares against one immutable userver revision.
19. As a maintainer, I want formatting, warnings, and static-analysis entry
    points, so that source changes have consistent mechanical quality checks.
20. As the project owner, I want bootstrap and production logging migration kept
    separate, so that this phase can be reviewed as infrastructure only.

## Implementation Decisions

- Initialize a new repository with `develop` as its initial branch and avoid
  committing generated build or dependency state.
- Use one root domain context and root architectural-decision directory, with
  local Markdown issue tracking until an external tracker is explicitly adopted.
- Publish a minimal compiled library target and version/export surface solely to
  exercise static/shared builds, visibility, installation, and consumption;
  avoid speculative logging API.
- Use the standard CMake shared-library switch with static as its default and a
  Ulog-owned export macro for platform visibility.
- Export a single canonical namespaced target identically from the build tree and
  installed package.
- Use Conan 2 for development dependency resolution. Runtime dependencies remain
  private unless their types become part of a future public interface. Avoid
  adding unused Boost modules before a production feature requires them.
- Use GoogleTest for deterministic tests and Google Benchmark for benchmark
  executables, both as development-only dependencies.
- Provide CMake presets and reusable project options rather than requiring
  contributors to remember command-line flag combinations.
- Keep warnings strict and portable. Treat project warnings as errors in CI
  without applying those flags to consumers or third-party dependencies.
- Run formatting and static-analysis through explicit targets or scripts that
  are also callable locally.
- Verify independence by scanning shipped source, build metadata, package
  metadata, and regular tests for userver references. Architecture documents and
  explicitly marked migration evidence are allowed to name the baseline.
- Record the userver baseline revision as metadata only; do not add userver as a
  submodule, package requirement, source dependency, or regular test fixture.
- Build and test the supported platform matrix, static/shared variants, and an
  external installed-package consumer. Smoke-run benchmark binaries everywhere.
- Publish benchmark JSON as advisory CI artifacts. Hard wall-clock regression
  gates require controlled hardware and are not part of hosted CI bootstrap.
- Keep repository and documentation language in English.

## Testing Decisions

- Test through the installed namespaced target as the highest seam: an external
  consumer must configure, compile, link, and run without referencing the source
  tree.
- Verify the minimal public export surface from both static and shared builds.
- Add deterministic checks for export definitions, version metadata, and absence
  of userver dependencies.
- Add a counting-allocation test utility and a bounded-memory test utility as
  infrastructure; production-path allocation assertions begin with the first
  logging implementation that can exercise them.
- Compile and dry-run benchmark executables in CI. Timing results are advisory on
  hosted runners and must not fail a pull request.
- Validate CMake configure, build, test, install, and external-consumer flows on
  the available local toolchain before handoff. Document any platform jobs that
  can only be verified in CI.
- Treat configuration and packaging errors as user-facing behavior: failures
  must identify the invalid setting and provide a correction hint where useful.

## Out of Scope

- Production implementations of Runtime, Logger, RecordWriter, Dispatcher,
  Encoder, Sink, ContextProvider, queues, libuv I/O, formats, or macros.
- Changes to the userver repository or its build.
- Source/API compatibility layers for `userver::logging`.
- Differential behavior or performance measurements against userver.
- Final numeric pool, batching, sampling, retry, latency, or throughput defaults.
- Publishing packages, releases, tags, or CI credentials.
- A hard timing gate on shared hosted runners.

## Further Notes

- The pinned behavioral baseline is userver revision
  `72e07f717ae46a17822776df21ebd73dbc4ce728`.
- Performance-sensitive implementation must follow the maintained Performance
  Contract and applicable architectural decisions.
- Every Logger state published as Default Logger will eventually require
  application-long address stability; the bootstrap export surface must not
  accidentally commit the project to a conflicting ownership model.
