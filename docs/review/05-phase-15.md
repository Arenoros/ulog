# Review 05 — Phase 15 (CI matrix)

Scope: author a cross-platform GitHub Actions workflow so that every push / PR triggers a Conan install + CMake build + ctest run on Linux (gcc + clang), macOS (clang), and Windows (MSVC).

## Changes

- `.github/workflows/ci.yml` — matrix of four jobs (`linux-gcc`, `linux-clang`, `macos-clang`, `windows-msvc`), `fail-fast: false` so a single-platform flake doesn't cancel the rest.
- Each job:
  1. `actions/checkout@v4`.
  2. Installs Conan 2 via `turtlebrowser/get-conan@main` (version pinned to 2.8.0).
  3. Runs `conan profile detect --force` to pick up the runner's native toolchain.
  4. Ensures Ninja is available (package manager install on Linux/macOS; MSVC ships it on Windows).
  5. On Windows, sets up the Visual Studio dev prompt via `ilammy/msvc-dev-cmd@v1` so `cl` / `link` / `ninja` are on `PATH`.
  6. `conan install . --build=missing` with `-s build_type=Release -s compiler.cppstd=17` and the Ninja toolchain generator.
  7. `cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake`.
  8. `cmake --build build --parallel`.
  9. `ctest --output-on-failure` from the build tree.
- `README.md` — short "Continuous integration" section explaining the layout.

## Validation

- YAML syntax validated via `python -c "yaml.safe_load(...)"` → OK.
- Local MSVC 14.44 full suite re-run: **63/63 passing** (unchanged).
- Runtime semantics of the workflow cannot be observed until ulog is extracted to its own repository — GitHub Actions only picks up `.github/workflows/*.yml` at the repo root, so the file is dormant while nested inside userver. Documented both in the workflow header comment and in README.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Conan version pin to 2.8.0** — will drift. Plan: revisit quarterly, or switch to the `latest` tag once the behavior stabilizes across minor bumps.
- **macOS runner is `macos-13`** (Intel). Apple Silicon runners are `macos-14`/`15` (ARM). Adding an ARM job is a one-line matrix entry; skipped until there's a reason to verify that platform specifically.
- **Compiler.cppstd=17** in CI but the library compiles fine with C++20/23 as well. A separate matrix row asserting C++20 would catch accidental legacy-only idioms; follow-up.
- **No caching** of the Conan download cache between runs. Would speed CI significantly once traffic grows. Easy `actions/cache` add.
- **No artifact upload** of test output on failure — ctest `--output-on-failure` goes to stdout only. If flakes start to need post-mortem, attach the ctest log directory.

### Deferred
- Google Benchmark-based perf suite job — tracked in BACKLOG.
- Sanitizer builds (ASan / TSan / UBSan) — tracked in BACKLOG.

## Acceptance: ✅ commit + advance
