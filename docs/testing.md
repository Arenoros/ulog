# Testing and performance checks

The repository establishes seven independent test categories:

- `unit`: public version and native frontend seams, compile-time erasure,
  Null-Logger allocation/ownership guarantees, atomic Default Logger exchange,
  stale-target completion, bounded producer transactions, Operation polling,
  deadlines, callback dispatch, control-reserve exhaustion, and test memory
  resources;
- `integration`/`package`: install Ulog, configure a copied external project,
  link only `ulog::ulog`, and exercise the frontend across translation units;
- `dependencies`: compile and run the production fmt integration together with
  the planned libuv dependency boundary;
- `stress`: deterministic concurrent checks for allocation instrumentation,
  linearizable Default Logger exchange, stale-target dispatch, producer-lane
  saturation, FIFO publication, byte conservation, retirement, and race-free
  weak snapshots, plus completion/callback registration races;
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
The production producer and Operation allocation tests and randomized stress
tests have 10-second CTest limits; stress executables also have their own
five-second watchdogs so a stalled thread fails with a focused diagnostic. The
Default Logger concurrency stress uses the same five-second watchdog and
10-second CTest limit. The frontend benchmark adds no hosted job: its five-row smoke body
runs inside the existing static package build with a 30-second CTest limit. Its
controlled-schedule listing is capped at 10 seconds by the script and 15 seconds
by CTest, and its result validator is capped at 10 seconds. The full controlled
frontend body is external-only and has the three-minute supervisor limit shown
in `docs/benchmarking.md`.

The presets build the library, architecture check, and installed consumer once
fmt 12 is available to CMake. The pinned Conan setup is:

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

## Bootstrap verification record

The bootstrap handoff was verified locally on Windows with CMake 3.31, Conan
2.31, and MSVC 19.51. Both static and shared builds passed strict compilation,
the independence check, installation, an isolated CMake consumer, the complete
Conan test categories, and the separate Conan `test_package` consumer.

The exact GitHub-hosted Ubuntu GCC, Ubuntu Clang sanitizer/static-analysis,
macOS AppleClang, and Windows Server runner images remain CI-only validation.
Their static/shared jobs and benchmark artifacts must pass before a change is
merged; local Windows results do not substitute for those platform jobs.
