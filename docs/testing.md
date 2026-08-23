# Testing and performance checks

The bootstrap establishes five independent test categories:

- `unit`: public version seam and test memory resources;
- `integration`/`package`: install Ulog, configure a copied external project,
  link only `ulog::ulog`, and run it;
- `dependencies`: compile and run fmt/libuv integration without linking those
  dependencies into the shipped bootstrap library;
- `stress`: deterministic concurrent checks for allocation instrumentation;
- `benchmark`: smoke-run Google Benchmark and emit JSON without a timing gate.

Hosted CI benchmark timing is advisory. It confirms that benchmark executables
compile, run, and emit machine-readable output. Hard regression thresholds are
allowed only on controlled hardware and must follow
`docs/performance-contract.md`.

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
