# Testing and performance checks

The repository establishes seven independent test categories:

- `unit`: public version and native frontend seams, compile-time erasure,
  Null-Logger allocation/ownership guarantees, atomic Default Logger exchange,
  stale-target completion, bounded producer transactions, Operation polling,
  deadlines, callback dispatch, control-reserve exhaustion, structured and Raw
  in-memory Runtime lifecycle, saturation, exact encoding, Raw file-route
  append, truncate-on-start, the non-empty-file separator, parent-directory
  creation, Unicode paths, validation and open errors, injected partial writes and
  open/directory/write/close failures, level names, the runtime threshold matrix, Null
  Logger level rejection, the Default Logger level scope, typed fields with their collision, reserved-key,
  ownership, laziness, and truncation policy, the field-rich Raw corpus cases,
  multi-translation-unit source path prefix trimming, injected event clocks, UTC and process-local
  timestamp formatting across calendar boundaries and concurrent calls, and test
  memory resources;
- `integration`/`package`: install Ulog, configure a copied external project,
  link only `ulog::ulog`, and exercise the frontend, in-memory Runtime tracers,
  and a Raw file route writing a temporary file through installed public headers;
- `dependencies`: compile and run the production fmt integration together with
  the private libuv dependency;
- `stress`: deterministic concurrent checks for allocation instrumentation,
  linearizable Default Logger exchange, concurrent threshold changes, queries,
  and logging, stale-target dispatch, producer-lane
  saturation, FIFO publication and Raw delivery, byte conservation, retirement,
  bounded destruction, concurrent split file appends, Runtime destruction during
  active appends, Shutdown deadlines raced against producers and held-delivery
  completions, and race-free weak snapshots, plus completion/callback
  registration races, and the [admission model oracle](#admission-model-oracle);
- `tooling`: regression checks for dependency-graph enforcement and benchmark
  result collection, plus the frontend atomic-load and producer hot-path
  source-shape contract;
- `migration`: schema, provenance, manifest-ID, and integrity validation for
  the committed offline baseline corpus and non-format parity scenarios,
  without an external checkout;
- `benchmark`: run the complete short workload matrix, emit versioned Google
  Benchmark JSON, validate its deterministic counters without a timing gate,
  and run the five-row frontend allocation/evaluation/accounting gate.

Hosted CI benchmark timing is advisory. It confirms that benchmark executables
compile, run, and emit machine-readable output. Hard regression thresholds are
allowed only on controlled hardware and must follow
`docs/performance-contract.md`. The exact workload matrix, metrics, candidate
adapter seam, and controlled-run command are documented in
`docs/benchmarking.md`.

Hosted jobs are fail-closed against hangs and accidental long workloads. The
platform build/package jobs and ThreadSanitizer job have 20-minute hard limits;
quality has 15 minutes. Dependency preparation, static/shared package builds,
formatting, result collection, cache operations, and artifact upload also have
shorter per-step limits. Every maintained CTest has its own timeout, including
the standalone Conan package consumer. Full controlled benchmark execution is
never part of hosted CI. A timeout is a diagnosable failure, not permission to
silently raise the limit: first identify the stalled phase or move intentional
long-running evidence collection to the externally bounded controlled runner.
The production producer, Operation, structured/Raw Runtime allocation tests, and
randomized stress tests have 10-second CTest limits; stress executables also have
their own five-second watchdogs so a stalled thread fails with a focused diagnostic.
The Default Logger concurrency stress uses the same five-second watchdog and
10-second CTest limit. The frontend benchmark adds no hosted job: its five-row
smoke body runs inside the existing static package build with a 30-second CTest
limit. Its controlled-schedule listing is capped at 10 seconds by the script and
15 seconds by CTest, and its result validator is capped at 10 seconds. The full
controlled frontend body is external-only and has the three-minute supervisor
limit shown in `docs/benchmarking.md`.

## In-memory Runtime test seam

The installed [`InMemoryDestination`](runtime.md#observing-records) and
[`InMemoryEncodedDestination`](runtime.md#observing-raw-bytes) are deterministic
pull seams, not generic production Sinks. `TryTake()` returns the lowest ready
admission sequence without blocking, and each move-only observation pins its fixed
destination slot until moved from or destroyed. A full set of ready or held slots
therefore applies worker-side backpressure while producer admission remains
non-blocking.

Tests can construct the destination with `start_paused=true` to hold the worker
before its first destination write, fill bounded ingress deterministically, and
verify drop-newest rejection before caller evaluation. `Resume()` opens the
destination gate. The encoded destination's
[deferred delivery modes](runtime.md#deferred-raw-delivery) additionally complete
Raw deliveries inline, fail them inline, or hold them for the test to complete or
fail through `PendingEncodedDelivery`, which makes Drain watermarks, exact reports,
in-flight pressure, and cancellation observable without timing assumptions. Runtime unit, installed-consumer, and Conan package tests use
explicit Drain or Shutdown Operations and bounded deadlines; none relies on
destructor timing for successful delivery.

File-route tests create unique directories below the system temporary directory
and remove them afterwards. Deterministic file faults use a private Runtime factory
that injects open, directory-creation, write, partial-write, stuck-write, `fsync`,
and close outcomes on the I/O thread and counts filesystem operations per thread and `fsync`
calls, which proves that Drain and Shutdown never call `fsync`; that factory is not
part of the installed API. In shared builds those tests link a private static copy of the library,
because the shared library hides its internal symbols.

The presets build the library, architecture check, and installed consumer once
fmt 12 and libuv 1.51 are available to CMake. The pinned Conan setup is:

```shell
conan profile detect --force
conan build . -pr:h=conan/profiles/cpp20 -pr:b=default -s build_type=Release \
  --lockfile=conan.lock --build=missing --output-folder=out/conan-build-release
```

`conan build` resolves the generator-specific layout, builds Ulog, and runs
CTest without requiring a hard-coded toolchain path.

The complete suite uses Conan:

```shell
conan profile detect --force
conan create . -pr:h=conan/profiles/cpp20 -pr:b=default --build=missing \
  -s build_type=Release \
  -o "&:build_tests=True" \
  -o "&:build_stress_tests=True" \
  -o "&:build_benchmarks=True" \
  -o "&:dependency_smoke=True"
```

## Admission model oracle

`tests/model/admission_model.hpp` is a sequential reference model of the
single-route pipeline:
- producer slots, per-producer ingress lanes, and per-producer credit refilled
  from the shared payload pool, with the 64-byte charge quantum;
- drop-newest rejection by reason;
- worker progress through the encoded destination's slots;
- in-order retirement of held, completed, failed, and abandoned deliveries;
- the control reserve, with exact Drain and Shutdown reports.

`ulog-admission-oracle` (`tests/stress/admission_oracle.cpp`) generates a
seeded event trace and applies it to the model and to a real Runtime with an
`InMemoryEncodedDestination`. Events log from registered and unregistered
producer threads, change the delivery mode, complete, fail, or abandon held
deliveries, take and release frames, start Drains and Shutdowns, and release
Operation handles. A burst makes every producer thread log at once. It runs
concurrently only while the route has no free slot and the shared pool covers
every refill, so each thread has one outcome in every interleaving. Otherwise
the same attempts run one thread at a time.

After each event the oracle waits until the Runtime matches the model:
- every snapshot counter and rejection reason, retained records, and logical
  and physical retained bytes;
- message-factory evaluations;
- frame bytes and FIFO order, which is per producer inside a concurrent burst;
- the exact outcome and report of every Operation.

The default CTest run, `ulog.admission-oracle`, covers 16 seeds of 48 events.
It also fails when that set leaves any modeled behavior unexercised, such as a
rejection reason, a pending Shutdown, or an out-of-order completion. It runs
under ThreadSanitizer in the stress jobs.

A divergence prints the seed, the configuration, a minimized event trace, and a
`--replay` command that runs exactly that trace again. A longer soak runs more
seeds and longer traces:

```shell
ulog-admission-oracle --seeds=5000 --first-seed=1 --events=120
ulog-admission-oracle --events=48 --replay=228:0,3,7,12
```

Later shedding policies, reserves, and routes extend the model at its marked
extension points: `Admit()`, `RouteModel`, and the Operation report builder.

## Local Linux toolchains

`tools/docker/` builds a Debian image with GCC 12 / libstdc++ and Clang 16 /
libc++. It complements CI, which uses the Ubuntu 24.04 compilers with libstdc++,
and gives Windows hosts a Linux build. Sources go in through stdin, so a remote
Docker daemon works as well; the `ulog-conan` volume keeps built dependencies:

```shell
docker build -t ulog-linux - < tools/docker/Dockerfile
git ls-files -co --exclude-standard | tar c -T - | docker run -i --rm     -v ulog-conan:/root/.conan2/p ulog-linux     sh -c 'tar x && tools/docker/conan-create.sh clang16-libcxx'
```

`conan-create.sh <profile> [conan create args]` runs the static package with
unit, stress, dependency, and package-consumer tests; `BUILD_TYPE` selects the
configuration and extra `-o` options enable sanitizers.

ThreadSanitizer with libc++ reports races in the benchmark workload harness and
the benchmark prototypes. Their synchronization goes through `std::barrier`, and
the stock libc++ is not instrumented, so TSan cannot see it. The library stress
tests stay clean. Use the libstdc++ CI row for TSan, or an instrumented
libc++.

## Bootstrap verification record

The bootstrap handoff was verified locally on Windows with CMake 3.31, Conan
2.31, and MSVC 19.51. Both static and shared builds passed strict compilation,
the independence check, installation, an isolated CMake consumer, the complete
Conan test categories, and the separate Conan `test_package` consumer.

The exact GitHub-hosted Ubuntu GCC, Ubuntu Clang sanitizer/static-analysis,
macOS AppleClang, and Windows Server runner images remain CI-only validation.
Their static/shared jobs and benchmark artifacts must pass before a change is
merged; local Windows results do not substitute for those platform jobs.
