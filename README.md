# ulog

Standalone cross-platform C++17 logging library extracted from [userver](https://github.com/userver-framework/userver). No runtime dependency on userver.

## Features

- **Macros preserved 1:1 from userver**: `LOG_INFO`, `LOG_ERROR`, `LOG_DEBUG`, `LOG_TRACE`, `LOG_WARNING`, `LOG_CRITICAL`, every `LOG_*_TO(logger, …)` variant, every `LOG_LIMITED_*` variant. Compile-erase via `-DULOG_ERASE_LOG_WITH_LEVEL=N`. Conflict-resolution alias `-DULOG_NO_SHORT_MACROS=ON` exposes only `ULOG_LOG_*`.
- **Three-tier filtering**: logger level (atomic), dynamic-debug per (file, line), and a user-installable tracing hook.
- **Formatters**: TSKV, LTSV, RAW, JSON (+ YaDeploy variant stub).
- **Sinks**: file (buffered stdio, rotation via `Reopen`), fd (`stdout`/`stderr`/adopted), TCP, Unix-domain socket (POSIX + Windows 10 1803+), null.
- **Async logger**: `std::thread` worker + lock-free `moodycamel::ConcurrentQueue`. `Discard` or `Block` overflow policies, synchronous `Flush` through `std::promise`.
- **Config builder**: `ulog::LoggerConfig` + `MakeSyncLogger` / `MakeAsyncLogger` / `InitDefaultLogger`. Path specs: `@stdout`, `@stderr`, `@null`, `tcp:host:port`, `unix:/path`, or any local filesystem path.
- **Tracing hook**: register a callback to inject trace/span IDs per record. No built-in tracing dependency.
- **Rotation on POSIX**: `ulog/posix/sigusr1_handler.hpp` installs a `SIGUSR1` → `RequestReopen` bridge. Windows users call `AsyncLogger::RequestReopen()` directly.
- **Rate limiting**, **stacktrace cache**, **LogExtra** structured tags.
- **Cross-platform**: Linux, macOS, Windows (MSVC 2019+, gcc 9+, clang 11+).

## Quickstart

```cpp
#include <ulog/config.hpp>
#include <ulog/log.hpp>

int main() {
    ulog::LoggerConfig cfg;
    cfg.file_path = "@stderr";            // or "/var/log/app.log" / "tcp:host:514"
    cfg.format    = ulog::Format::kTskv;  // or kJson, kLtsv, kRaw
    cfg.level     = ulog::Level::kDebug;
    auto logger   = ulog::InitDefaultLogger(cfg);

    LOG_INFO() << "started pid=" << ::getpid();
    LOG_ERROR() << "boom " << ulog::LogExtra{{"code", 42}, {"op", std::string("open")}};

    ulog::LogFlush();
}
```

## Build

Conan 2 + Ninja + MSVC on Windows:

```
conan install ulog --output-folder=ulog/build -s build_type=RelWithDebInfo -s compiler.cppstd=23 -o "&:build_tests=True" -c tools.cmake.cmaketoolchain:generator=Ninja
cmake -S ulog -B ulog/build -G Ninja -DCMAKE_TOOLCHAIN_FILE=conan_toolchain.cmake -DCMAKE_POLICY_DEFAULT_CMP0091=NEW -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build ulog/build
ctest --test-dir ulog/build
```

Dependencies: `fmt` + Boost `container` & `stacktrace` (via Conan or any package manager). Optional: `nlohmann_json`, `yaml-cpp`, `cpp-httplib`, `GTest` + `benchmark` (tests / benches only).

## Consuming ulog as a Conan package

ulog ships a `conanfile.py` recipe at the repo root — both as the
dev-time dependency declaration and as a publishable package recipe.

### Build and cache the package locally

```
# From inside the ulog checkout:
conan create . --build=missing
```

This runs the recipe's `build()` + `package()`, producing a versioned
binary (`ulog/0.1.0`) in your local Conan cache. Consumers can then:

```python
# consumer's conanfile.py
def requirements(self):
    self.requires("ulog/0.1.0")
```

or via `conanfile.txt`:

```
[requires]
ulog/0.1.0
```

and CMake-side:

```cmake
find_package(ulog CONFIG REQUIRED)
target_link_libraries(myapp PRIVATE ulog::ulog)
```

### Recipe options

| Option | Default | Effect |
|---|---|---|
| `shared` | `False` | Build as shared lib (default: static) |
| `with_nlohmann` | `False` | Validate `JsonString` via nlohmann::json |
| `with_yaml` | `False` | Enable yaml-cpp config loader |
| `with_http` | `False` | Enable `OtlpBatchSink` + `otlp:` spec (pulls cpp-httplib) |
| `with_afunix` | `False` | Enable AF_UNIX sink on Windows 10 1803+ |
| `no_short_macros` | `False` | Hide `LOG_*` short names; keep `ULOG_LOG_*` only |
| `erase_log_with_level` | `0` | Compile-erase levels below this (1..4) |
| `build_tests` | `False` | Build gtest suite (dev use) |
| `build_bench` | `False` | Build benchmarks (dev use) |

Example opting into HTTP + hiding short macros:

```
conan create . -o "&:with_http=True" -o "&:no_short_macros=True"
```

### CMake options

| Option | Default | Description |
|---|---|---|
| `ULOG_BUILD_TESTS` | `ON` | Build gtest-based unit suite |
| `ULOG_BUILD_EXAMPLES` | `ON` | Build the example binaries |
| `ULOG_BUILD_BENCH` | `OFF` | Build benchmarks (reserved) |
| `ULOG_WITH_NLOHMANN` | `OFF` | Validate `JsonString` payloads via nlohmann::json |
| `ULOG_WITH_YAML` | `OFF` | Enable yaml-cpp-based config loader (reserved) |
| `ULOG_NO_SHORT_MACROS` | `OFF` | Hide `LOG_*` short names; keep `ULOG_LOG_*` only |
| `ULOG_ERASE_LOG_WITH_LEVEL` | `0` | Compile-erase levels below this (1..4) |
| `ULOG_SOURCE_ROOT` | `<source dir>` | Prefix stripped from `__FILE__` in records |
| `ULOG_INSTALL` | `ON` | Generate install/export rules |

### Trimming `__FILE__` in consumer builds

`ULOG_SOURCE_ROOT` is the prefix that `LOG_*` strips from `__FILE__` so
records carry `foo/bar.cpp` rather than
`/home/runner/work/project/ulog/foo/bar.cpp`. The root is baked into
ulog at configure time as the public macro `ULOG_SOURCE_ROOT_LITERAL`.

**`add_subdirectory` consumers** — set the option before pulling ulog
in, and every consumer target that links ulog inherits the consumer's
source root:

```cmake
set(ULOG_SOURCE_ROOT "${CMAKE_SOURCE_DIR}" CACHE STRING "")
add_subdirectory(third_party/ulog)
```

**`find_package(ulog)` / pre-built Conan consumers** — the root was
fixed when ulog was built, so prefer this path when you control the
ulog build. If the binary ships with ulog's own root baked in and you
cannot rebuild, override on your target and expect a
"macro redefinition" warning for the translation units you care about:

```cmake
find_package(ulog REQUIRED)
add_executable(myapp ...)
target_link_libraries(myapp PRIVATE ulog::ulog)
target_compile_definitions(myapp PRIVATE
    ULOG_SOURCE_ROOT_LITERAL="${CMAKE_SOURCE_DIR}")
```

The cleaner long-term story is to configure `ULOG_SOURCE_ROOT` at
package-build time (e.g. via Conan `options`) so downstream targets
don't have to redefine anything.

## Layout

```
ulog/
  include/ulog/
    log.hpp                  # Macros + default logger
    level.hpp                # enum Level
    format.hpp               # enum Format
    log_extra.hpp            # structured KV
    log_helper.hpp           # stream-like builder
    logger.hpp (fwd)
    null_logger.hpp
    mem_logger.hpp           # capture to memory (tests)
    sync_logger.hpp          # caller-thread writes to sinks
    async_logger.hpp         # worker-thread writes to sinks
    config.hpp               # builder API
    json_string.hpp
    dynamic_debug.hpp
    stacktrace_cache.hpp
    tracing_hook.hpp
    sinks/                   # base_sink, null, fd, file, tcp, unix
    impl/
      logger_base.hpp
      formatters/            # base, tskv, ltsv, raw, json
    detail/                  # small_string, tskv_escape, timestamp, ...
    posix/                   # sigusr1_handler (POSIX only)
  src/                       # mirrors include/
  tests/                     # gtest unit suite
  examples/
    simple/                  # one-file quickstart
  third_party/
    concurrentqueue/         # moodycamel (vendored header-only)
  docs/extraction-plan.md    # full porting plan
```

## Continuous integration

`.github/workflows/ci.yml` defines a cross-platform matrix (Ubuntu gcc/clang, macOS clang, Windows MSVC). Each job runs Conan install → CMake configure → build → ctest. The workflow activates once ulog is extracted to its own repository; while nested inside userver, GitHub does not pick up workflows outside the repo root and the file lies dormant.

## API reference

A minimal Doxygen config lives at [`docs/Doxyfile`](docs/Doxyfile).
Regenerate the HTML reference with:

```
doxygen docs/Doxyfile
```

Output lands under `build/docs/html/index.html`. `dot` is not required.

## License

Apache-2.0 (inherited from userver).
