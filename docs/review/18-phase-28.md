# Review 18 — Phase 28 (CI infra: conan cache + C++20 row + artifact upload)

Scope: close three small CI-infra BACKLOG items in one pass. None change runtime behavior — the payoff is faster CI on warm runs (conan cache), earlier detection of accidental C++17-isms (cpp20 row), and salvageable post-mortem data when CI fails (ctest artifact upload).

## Changes

### `.github/workflows/ci.yml`

- **New matrix field `cppstd`** on every row (default `"17"`). Threaded into the Conan install step via `-s compiler.cppstd="$CPPSTD"`. Present on every include entry so downstream expressions (cache key) can reference it unconditionally.
- **New row `linux-clang-cpp20`** — Ubuntu 22.04 / clang++ / Release with `cppstd: "20"`. Exercises the full build + tests under C++20 so unintentional C++17-only idioms trip CI before they reach userver (which builds with `-std=c++20` downstream).
- **Conan package cache** via `actions/cache@v4`. Caches `~/.conan2/p` (Conan's package store) with a key fingerprinted by `os`, `cc`, `cppstd`, `build_type`, `sanitizer`, and `hashFiles('conanfile.txt')`. Fallback `restore-keys` drop the hash first (lets a source-identical rebuild ride the cache) then the sanitizer slice (deps aren't currently instrumented, so sanitizer rows can inherit non-sanitizer caches).
- **`ctest` log artifact upload on failure** via `actions/upload-artifact@v4` with `if: failure()`. Uploads `build/Testing/Temporary/LastTest.log` + `LastTestsFailed.log` per matrix row, retained 14 days. `if-no-files-found: ignore` so earlier-step failures (build, configure) don't blow up the artifact step when the logs were never produced.
- **`CMAKE_CXX_STANDARD` CLI override removed** from the configure step. Conan's toolchain already wires `compiler.cppstd` into the CMake standard setting — the CLI pass-through was redundant and risked masking.

### `CMakeLists.txt`

- The hardcoded `set(CMAKE_CXX_STANDARD 17)` now sits behind `if(NOT DEFINED CMAKE_CXX_STANDARD)`. The Conan toolchain defines the variable early (before this file runs), so the if-guard lets CI-driven cppstd values flow through. `target_compile_features(ulog PUBLIC cxx_std_17)` below still pins the floor for consumers.

## Why the cache key looks like that

A Conan package hash is a function of (compiler, compiler-version, cppstd, build_type, libcxx, …). If any of those change between CI runs, the package hash moves and `--build=missing` rebuilds from source. Our key encodes the axes we actually vary in CI (`os` picks up the runner image + default compiler version, `cc` distinguishes gcc/clang on the same os, `cppstd` covers the new C++20 row, `build_type` and `sanitizer` pick up Debug/RelWithDebInfo differences). `hashFiles('conanfile.txt')` rolls in dependency version bumps. Downstream `restore-keys` let a first-run on a new conanfile hash still warm-start from the prior run's packages wherever the hashes overlap.

## What the cppstd matrix actually catches

- Auto-deduced return types where C++17 behavior diverges from C++20.
- `std::string_view` constructor resolution changes (for example the int-to-char-pointer deprecation in C++20).
- Ambiguous overloads introduced by C++20 rewritten candidates for `operator==` / three-way comparison.
- Missing headers that happened to be transitively included in libstdc++'s C++17 mode but got cleaned up in C++20.

If any of those slip through, the cpp20 row turns red without affecting the rest of the matrix.

## Results

- **Local build** (MSVC 14.50, RelWithDebInfo, C++17 default path): clean, 81/81 tests green. The CMakeLists `if(NOT DEFINED ...)` guard passes because no override is set; same floor as before.
- **Remote CI**: will be exercised on the first push/PR run after merge. Unreviewable locally beyond the config smell test.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **Cache may go stale if the conan profile file changes** (default profile is generated on the fly by `conan profile detect --force`, so the hash doesn't cover profile-driven toolchain tweaks). In practice the profile is deterministic per runner image, which is encoded in `matrix.os`.
- **C++20 row is Linux-clang only.** Adding a Windows MSVC C++20 row would double the cold-cache cost for marginal additional coverage (the MSVC-specific C++20 issues tend to be either compiler-specific warnings or real bugs that the Release-C++17 row already catches). Deferred.
- **Artifact retention is 14 days.** Long enough for same-sprint triage, short enough to not accumulate gigabytes. Revisit if the team needs longer retention for release-branch post-mortems.
- **`CMAKE_CXX_STANDARD` if-guard relies on the Conan toolchain being loaded before** the `set(...)` runs. That ordering is the Conan 2 default; a downstream consumer that uses ulog as a plain CMake subproject (no Conan toolchain) will still get the 17 floor. Verified by the local build.

### Test coverage gaps
- **No unit test** — this phase is pure CI config. The only "test" possible is running CI itself; the local build run confirmed the CMakeLists change does not regress the default path. A follow-up can add a `tests/cmake/cxx_standard_override_test.cmake` fixture if we want scripted coverage, but it's overkill for one `if` statement.

### Deferred to BACKLOG
- `Google Benchmark в CI` — separate job uploading bench JSON as an artifact. Still open, this phase doesn't touch it.
- Windows C++20 matrix row, per the tradeoff above.

## Acceptance: ✅ commit + advance
