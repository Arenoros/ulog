# Review 23 — Phase 33 (Windows AF_UNIX sink + test)

Scope: close the BACKLOG "Windows AF_UNIX тест" item. The `UnixSocketSink` source already had the `_WIN32 || ULOG_HAVE_AFUNIX` guard machinery in place; what was missing was the build option to turn it on, the test coverage that actually exercises the Winsock + afunix.h path, and a CI matrix row that compiles the configuration.

## Changes

### Build option

- New CMake option `ULOG_WITH_AFUNIX` (default `OFF`). When enabled on a Windows build, the top-level `CMakeLists.txt` pushes `ULOG_HAVE_AFUNIX=1` as a PUBLIC compile definition so both `src/` and every downstream TU that includes `<ulog/sinks/unix_socket_sink.hpp>` sees the sink surface.
- `src/CMakeLists.txt` compiles `sinks/unix_socket_sink.cpp` on Windows only when `ULOG_WITH_AFUNIX` is on. POSIX behavior unchanged.
- `tests/CMakeLists.txt` pulls `unix_sink_test.cpp` into the Windows test binary under the same condition.

### Test rewrite

- `tests/unix_sink_test.cpp` file-level guard loosened from `!_WIN32` to `!_WIN32 || ULOG_HAVE_AFUNIX`.
- Listener split into platform sockets: POSIX uses `<sys/socket.h>` / `<sys/un.h>` / `<unistd.h>`; Windows uses `<winsock2.h>` + `<afunix.h>` with the usual `closesocket` / `SOCKET` / `INVALID_SOCKET` aliases. The listener stores `sock_t` so the same control flow covers both.
- Cleanup uses `std::filesystem::remove` instead of POSIX `unlink` so the path cleanup works on Windows too (`bind()` creates a filesystem artifact on both platforms in AF_UNIX SOCK_STREAM mode).
- Constructor split into ctor + `Init()` so MSVC accepts gtest `ASSERT_*` macros (they expand to `return;` which the MSVC frontend rejects inside a constructor). POSIX got the same split for free — same function body, callable on both platforms.

### CI

- `.github/workflows/ci.yml` windows-msvc matrix row gains `afunix: "ON"`. The configure step reads `AFUNIX` from the matrix and appends `-DULOG_WITH_AFUNIX=ON` when set, so the Windows CI actually builds + runs the AF_UNIX sink (and the new test). Other rows skip the option.

## Why split `Init()` rather than throw

gtest's `ASSERT_*` on failure expands to a plain `return;` — fine in `void` functions, but MSVC's frontend rejects it inside a constructor (which can't return anything, value or otherwise). Throwing on failure would also work, but it bypasses gtest's test-scoped failure-capture plumbing. Split-phase keeps the assertion reporting in the normal channel and matches the existing TCP listener's constructor style (which, incidentally, gets away with `throw std::runtime_error` because its ctor body doesn't call `ASSERT_*`).

## Results

- **Local build** (MSVC 14.50, RelWithDebInfo, `-DULOG_WITH_AFUNIX=ON`): clean.
- **Tests**: 98/98 green (97 previous + 1 new `UnixSocketSink.DeliversRecordsToListener` on Windows — the same test body that POSIX has always run).
- **CI windows-msvc row** will exercise the new path on the first post-merge push.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **ULOG_WITH_AFUNIX is opt-in per build, not auto-detected.** The `afunix.h` header ships with the Windows 10 1803+ SDK; we could detect availability with `check_include_file_cxx(afunix.h HAS_AFUNIX)`, but opt-in is safer because the *runtime* support also requires Win10 1803+ (so the build succeeding on a modern SDK tells us nothing about where the binary can run). Keeping it explicit documents the intent at the call site.
- **Only one test case.** Same surface as the POSIX test (deliver two records, read them back). The sink's thin wrapper around `UnixSocket::Send` has no Windows-specific code path that a second test would exercise; the existing case is a smoke test.
- **Path is in `%TEMP%`**. If two parallel ctest runs collide on the same name, the second bind fails. We picked a shared name from the POSIX version; adding a pid suffix would avoid the collision but the POSIX side never had it either — deferred as a cross-platform polish item.

### Deferred to BACKLOG
- Optional auto-detect for `ULOG_WITH_AFUNIX` via a `check_include_file_cxx` probe + `option(... "${AFUNIX_DETECTED}")`. Low priority.
- PID-suffixed socket paths in the unix listener fixture to enable parallel `ctest -j` runs without file-system collisions.

## Acceptance: ✅ commit + advance
