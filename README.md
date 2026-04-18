# ulog

Standalone cross-platform C++17 logging library extracted from [userver](https://github.com/userver-framework/userver). No runtime dependency on userver.

## Status

**Work in progress.** Phases 0-2 scaffolded (core types + macros + null/default logger infrastructure). Formatters, sinks, async logger, dynamic debug, and tracing hook — to follow.

See [docs/extraction-plan.md](docs/extraction-plan.md) for the full roadmap and rationale.

## Features (target)

- `LOG_INFO` / `LOG_ERROR` / `LOG_DEBUG` / `LOG_TRACE` / `LOG_WARNING` / `LOG_CRITICAL` + `LOG_LIMITED_*` + `LOG_*_TO` macros (names preserved 1:1 from userver).
- Levels, three-tier filtering, dynamic debug by file/line.
- Formatters: TSKV, LTSV, RAW, JSON.
- Sinks: file (buffered + unbuffered), stdout/stderr, TCP, Unix socket, null.
- Asynchronous logger: `std::thread` worker + lock-free queue.
- Rate limiting.
- Stacktrace cache.
- Log rotation (`Logger::Reopen()` + optional POSIX SIGUSR1 handler).
- Cross-platform: Linux, macOS, Windows (MSVC 2019+, gcc 9+, clang 11+).
- Optional integrations: nlohmann::json, yaml-cpp, OpenTelemetry (OTLP).

## Build

```
cmake -S ulog -B ulog/build
cmake --build ulog/build
ctest --test-dir ulog/build
```

Dependencies: `fmt`, `boost::stacktrace`. Optional: `nlohmann_json`, `yaml-cpp`, `GTest` (for tests).

## License

Apache-2.0 (inherited from userver).
