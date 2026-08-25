# Testing and performance checks

The bootstrap establishes seven independent test categories:

- `unit`: public version seam and test memory resources;
- `integration`/`package`: install Ulog, configure a copied external project,
  link only `ulog::ulog`, and run it;
- `dependencies`: compile and run fmt/libuv integration without linking those
  dependencies into the shipped bootstrap library;
- `stress`: deterministic concurrent checks for allocation instrumentation;
- `tooling`: regression checks for dependency-graph enforcement and benchmark
  result collection;
- `migration`: schema, provenance, manifest-ID, and integrity validation for
  the committed offline baseline corpus and non-format parity scenarios,
  without an external checkout;
- `benchmark`: run the complete short workload matrix, emit versioned Google
  Benchmark JSON, and validate its deterministic counters without a timing gate.

Hosted CI benchmark timing is advisory. It confirms that benchmark executables
compile, run, and emit machine-readable output. Hard regression thresholds are
allowed only on controlled hardware and must follow
`docs/performance-contract.md`. The exact workload matrix, metrics, candidate
adapter seam, and controlled-run command are documented in
`docs/benchmarking.md`.

The dependency-free presets build the library, architecture check, and installed
consumer:

```shell
cmake --preset release-static
cmake --build --preset release-static
ctest --preset release-static
```

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
