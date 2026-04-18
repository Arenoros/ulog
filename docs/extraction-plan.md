# ulog — standalone logging library extraction plan

Форк системы логирования userver в отдельную кросс-платформенную библиотеку. Подключается в любой C++ проект без зависимости на userver. Все фичи сохранены.

## Требования

- **Кросс-платформенность**: Linux, macOS, Windows (MSVC ≥ 2019, gcc ≥ 9, clang ≥ 11).
- **Макросы `LOG_*` сохраняются** как есть: `LOG_INFO`, `LOG_ERROR`, `LOG_DEBUG`, `LOG_TRACE`, `LOG_WARNING`, `LOG_CRITICAL`, `LOG_LIMITED_*`, `LOG_TO`, `LOG_*_TO`. Опциональный guard `ULOG_NO_SHORT_MACROS` — пользователь может отключить короткие имена при конфликте.
- Сохранить: async logger, все sinks, dynamic debug, форматтеры (tskv/json/ltsv/raw), LogExtra, rate limit, stacktrace, log rotation.
- Никаких зависимостей на userver. Boost/fmt/nlohmann/yaml-cpp/moodycamel — допустимы.

---

## Inventory зависимостей userver → замена

| userver dep | Использование | Замена (cross-platform) |
|---|---|---|
| `utils/assert` (UASSERT/UINVARIANT) | инварианты | `assert()` + свой `ULOG_ASSERT` |
| `utils/encoding/tskv` | escape TSKV | скопировать (~100 строк) |
| `utils/datetime_light` | timestamp format | `std::chrono` + `fmt::format` |
| `utils/SmallString` | 4KB стек-буфер | `boost::container::small_vector<char>` или свой |
| `utils/flags, underlying_value, trivial_map, algo, string_literal` | мелочёвка | скопировать минимум |
| `utils/traceful_exception` | exceptions со стеком | `std::runtime_error` + `boost::stacktrace` |
| `utils/PeriodicTask` | periodic flush | `std::thread` + `std::condition_variable::wait_for` |
| `utils/statistics::Writer` | метрики | дроп; plain atomic getters |
| `formats/json` (JsonString) | nested JSON в логе | `nlohmann::json` (opt-in `ULOG_WITH_NLOHMANN`) или validated string |
| `cache/lru_map` | stacktrace cache | своя мини LRU (~80 строк) |
| `compiler/demangle` | stacktrace symbols | `boost::core::demangle` (cross-platform) |
| `concurrent::impl::AsyncFlatCombiningQueue` | TpLogger очередь | `moodycamel::ConcurrentQueue` (single-header, MIT) |
| `concurrent::InterferenceShield` | cache-line pad | `alignas(std::hardware_destructive_interference_size)` или `alignas(64)` |
| `concurrent::SinglyLinkedBaseHook` | intrusive node | `boost::intrusive::slist` |
| `concurrent::AsyncEventSource` | OnLogRotate subscribers | простой callback list под `std::mutex` |
| `engine::TaskProcessor` | worker для async | `std::thread` |
| `engine::Mutex/CondVar/Task/Promise` | capacity wait, flush | `std::mutex/condition_variable/thread/promise` |
| `engine::io::Socket` | TcpSocketSink | POSIX `socket/send` + Winsock2 на Windows |
| `fs::blocking::CFile/FileDescriptor` | file sinks | `std::FILE*` (`fopen/fwrite/fflush`) + `_open/open` для FD |
| `os_signals::Component` | SIGUSR1 reopen | POSIX `sigaction` + atomic flag; Windows — API для reopen (signals нет, используем named event или ручной вызов) |
| `rcu::RcuMap` | extra_loggers | `std::shared_mutex` + `unordered_map` |
| `yaml_config` | YAML парсинг | `yaml-cpp` (opt-in `ULOG_WITH_YAML`) + C++ builder API |
| `dynamic_config::Snapshot` | dynamic debug updates | plain API `SetDynamicDebugLog(file,line,state)` |
| `components::RawComponentBase` | регистрация | дроп; plain `ulog::InitDefault(cfg)` |
| `USERVER_NAMESPACE` | все хедеры | заменить на `ulog` |

---

## Кросс-платформенные соображения

### Файловый I/O
- `std::FILE*` + `fopen/fwrite/fflush/fclose` — базовый путь (работает всюду).
- Для low-level FD sink: `<fcntl.h>` + `open/write/close` на POSIX, `_open/_write/_close` + `<io.h>` на Windows.
- Абстракция `FileHandle` (RAII wrapper) в `detail/file_handle.hpp` с `#ifdef _WIN32`.
- Log rotation: `freopen()` — стандартный C, работает на обеих платформах.

### Сокеты
- `detail/socket.hpp` — обёртка с `#ifdef _WIN32` → Winsock2 (`WSAStartup`, `closesocket`), иначе POSIX.
- `AF_UNIX` на Windows — доступен с Win10 1803+. Либо disable `UnixSocketSink` на Windows через `#if !defined(_WIN32) || _WIN32_WINNT >= 0x0A00`.
- TCP sink — полноценная кросс-платформа.

### Signals / rotation
- POSIX: `sigaction(SIGUSR1, ...)` → атомарно взвести флаг, worker thread подхватит.
- Windows: signals отсутствуют. Альтернативы:
  1. Public API `ulog::RequestReopen()` — пользователь вызывает из своего control-pipe/event/HTTP handler.
  2. Named Event (`CreateEvent` + отдельный watcher thread) — опциональный модуль `ulog-win-rotate`.
- Базовое API — просто метод `Logger::Reopen()`. Signal handler — опциональный header, подключается только на POSIX.

### Stacktrace
- `boost::stacktrace` — работает на Linux/macOS (через `backtrace`/`libunwind`) и Windows (через DbgHelp). Единый API.
- Demangling: `boost::core::demangle`.

### Threading
- `std::thread`, `std::mutex`, `std::condition_variable`, `std::atomic` — полный C++17 набор, всюду.
- Thread affinity / name — опционально через `#ifdef` (`pthread_setname_np` / `SetThreadDescription`).

### Timestamps
- `std::chrono::system_clock` + форматирование через `fmt::format("{:%Y-%m-%dT%H:%M:%S}", ...)` (fmt 9+).
- Timezone: UTC по умолчанию, опция локали.

### Compile-time
- C++17 минимум (structured bindings, `if constexpr`, `std::optional`).
- C++20 опционально — использовать `std::source_location` если доступен, иначе `__FILE__/__LINE__`.
- MSVC: `/permissive-` + `/Zc:__cplusplus` обязательны.
- Warning flags portable (gcc/clang `-Wall -Wextra -Wpedantic`, msvc `/W4`).

### CMake
- `cmake_minimum_required(VERSION 3.16)`.
- `target_compile_features(ulog PUBLIC cxx_std_17)`.
- Экспорт через `install(TARGETS ulog EXPORT ulog-targets)` + `ulog-config.cmake`.
- Поддержка `find_package(ulog)`, `FetchContent`, `add_subdirectory`.
- Packaging: Conan recipe, vcpkg port.

---

## Фазы

### Фаза 0 — Setup (1д)
- Репо `ulog`, CMake skeleton, `ulog::ulog` target, install rules, config.cmake.
- CI matrix: Ubuntu+gcc/clang, macOS+clang, Windows+msvc/clang-cl.
- License: Apache-2.0 (совместимо с userver), NOTICE с attribution.
- Conan + vcpkg recipes.

### Фаза 1 — detail/utils minimum (2-3д)
Скопировать минимум из userver utils и релицензировать:
- `detail/small_string.hpp`
- `detail/tskv_escape.hpp`
- `detail/flags.hpp`, `detail/trivial_map.hpp`, `detail/underlying_value.hpp`
- `detail/string_literal.hpp`, `detail/algo.hpp`
- `detail/timestamp.hpp` (chrono → string)
- `detail/file_handle.hpp` (cross-platform FILE/FD wrapper)
- `detail/socket.hpp` (cross-platform socket wrapper)
Unit-тесты на каждый файл (gtest).

### Фаза 2 — Core logging (3-5д)
- `level.hpp/cpp`, `format.hpp/cpp`
- `log_extra.hpp/cpp`
- `log_helper.hpp/cpp`
- `log.hpp` — макросы **`LOG_INFO/ERROR/DEBUG/WARNING/TRACE/CRITICAL/LIMITED_*/LOG_TO/LOG_*_TO`** сохранены 1:1 по семантике
- `logger_base.hpp/cpp`, `text_logger.hpp/cpp`
- `null_logger.hpp/cpp`, `mem_logger.hpp/cpp`
- `tag_writer.hpp/cpp`
- Feature flag `ULOG_NO_SHORT_MACROS` — отключает короткие имена, оставляет только `ULOG_LOG_*` для conflict resolution.
- Feature flag `ULOG_ERASE_LOG_WITH_LEVEL=N` — compile-time стирание низких уровней (аналог userver).
- Портировать pure-logic тесты: `log_extra_test`, `log_message_test`, `ratelimited_log_test`, `split_location_test`.

### Фаза 3 — Formatters (2д)
- `formatters/tskv.cpp/hpp`, `formatters/ltsv.cpp/hpp`, `formatters/raw.cpp/hpp`, `formatters/json.cpp/hpp`.
- `JsonString`: две реализации под feature flag.
  - `ULOG_WITH_NLOHMANN=ON` → nlohmann::json adapter.
  - `OFF` → validated string (парсинг на входе, хранение как есть).
- Тесты: `log_json_test`, `log_ltsv_test`.

### Фаза 4 — Dynamic debug, rate limit, stacktrace cache (1-2д)
- `dynamic_debug.hpp/cpp` — boost::intrusive::multiset.
- `rate_limit.hpp/cpp` — atomic counter per source line.
- `stacktrace_cache.hpp/cpp` — своя LRU или `boost::multi_index`.
- `boost::stacktrace` как публичная зависимость.

### Фаза 5 — Sinks (2-3д)
- `sinks/base_sink.hpp`
- `sinks/null_sink.hpp`
- `sinks/fd_sink.hpp/cpp` — через `detail/file_handle`.
- `sinks/file_sink.hpp/cpp` — unbuffered + buffered варианты.
- `sinks/stdout_sink.hpp`, `sinks/stderr_sink.hpp`.
- `sinks/tcp_socket_sink.hpp/cpp` — Winsock2 / POSIX через `detail/socket`.
- `sinks/unix_socket_sink.hpp/cpp` — POSIX + Windows AF_UNIX (compile guard).
- Тесты с tmp files + local server socket.

### Фаза 6 — Async logger (4-7д)
- `async_logger.hpp/cpp`:
  - Worker `std::thread`.
  - Очередь: `moodycamel::ConcurrentQueue<Action>` (vendored single-header).
  - Batch consume через `try_dequeue_bulk`.
  - Actions: `Log/Flush/Reopen/Stop`, promise через `std::promise<void>`.
  - Overflow: `kDiscard` / `kBlock` (block — cv на capacity).
  - State machine: `kSync → kAsync → kStopping`.
- Тесты-portы: `tp_logger_test` → `async_logger_test`, run через обычный `std::thread`, без engine.
- Benchmarks (Google Benchmark): throughput, latency percentile, contention.

### Фаза 7 — Log rotation (1д)
- `Logger::Reopen()` public API (cross-platform, работает всюду).
- POSIX header `ulog/posix/sigusr1_handler.hpp`:
  - `InstallSigUsr1Handler(Logger&)` — `sigaction`, atomic flag, worker poll.
  - Собирается только на POSIX (`#if !defined(_WIN32)`).
- Windows header `ulog/win/named_event_handler.hpp` (опционально, позже):
  - `InstallReopenEventHandler(Logger&, L"ulog-reopen")`.

### Фаза 8 — Config / init API (1-2д)
Plain C++ builder, без YAML-dep в ядре:
```cpp
struct LoggerConfig {
    std::string name;
    std::string file_path;  // "@stdout"|"@stderr"|"@null"|"/path"|"unix:/sock"|"tcp:host:port"
    Level level = Level::kInfo;
    Format format = Format::kTskv;
    Level flush_level = Level::kWarning;
    size_t queue_size = 65536;
    OverflowBehavior overflow = OverflowBehavior::kDiscard;
    bool truncate_on_start = false;
};

LoggerPtr MakeAsyncLogger(const LoggerConfig&);
LoggerPtr MakeSyncLogger(const LoggerConfig&);
void InitDefaultLogger(LoggerPtr);
LoggerRef GetDefaultLogger() noexcept;
```

Опциональный модуль `ulog-yaml` (`ULOG_WITH_YAML=ON`):
```cpp
LoggerConfig ParseYamlConfig(const YAML::Node&);
```

### Фаза 9 — Tracing integration hook (1д)
Никакого `tracing::Span`-кода не тащить. Публичный hook:
```cpp
using SpanTagsCallback = void(*)(TagWriter&, void* user_ctx);
void SetSpanTagsCallback(SpanTagsCallback cb, void* ctx);
```
LogHelper при сборке лога вызывает callback, если установлен. Пользователь wire'ит свою trace-систему (userver-tracing, opentelemetry-cpp, свою TLS).

`ShouldLog(Level)` — per-logger; span-level фильтрация через отдельный hook `SetLocalLevelProvider()`.

### Фаза 10 — OTLP (опциональный отдельный target, 2-3д)
- `ulog-otlp` — отдельная CMake цель.
- Зависит от `opentelemetry-cpp`.
- Sink, конвертирующий LogExtra → OTLP LogRecord.
- Не в ядре — линкуется отдельно.

### Фаза 11 — Packaging, docs, examples (2-3д)
- README, quickstart.
- Doxygen.
- Примеры: `examples/simple`, `examples/async_file`, `examples/custom_sink`, `examples/tracing_hook`, `examples/win32_rotate`.
- Benchmarks в `bench/`.
- `conanfile.py`, `vcpkg.json` финализировать.

---

## Структура репозитория

```
ulog/
  CMakeLists.txt
  cmake/
    ulog-config.cmake.in
  include/ulog/
    log.hpp                  # LOG_INFO, LOG_ERROR, ... (с сохранёнными именами)
    level.hpp
    format.hpp
    log_extra.hpp
    log_helper.hpp
    logger.hpp
    logger_base.hpp
    null_logger.hpp
    mem_logger.hpp
    config.hpp
    dynamic_debug.hpp
    stacktrace_cache.hpp
    sinks/
      base_sink.hpp
      null_sink.hpp
      fd_sink.hpp
      file_sink.hpp
      stdout_sink.hpp
      stderr_sink.hpp
      tcp_socket_sink.hpp
      unix_socket_sink.hpp
    formatters/
      tskv.hpp
      ltsv.hpp
      raw.hpp
      json.hpp
    async/
      async_logger.hpp
    posix/
      sigusr1_handler.hpp    # POSIX-only
    win/
      named_event_handler.hpp # Windows-only
    detail/
      small_string.hpp
      tskv_escape.hpp
      timestamp.hpp
      file_handle.hpp         # cross-platform FILE/FD
      socket.hpp              # Winsock2/POSIX
      flags.hpp
      trivial_map.hpp
      ...
  src/
    (импл .cpp файлы в зеркале include/)
  third_party/
    concurrentqueue/           # moodycamel, vendored single-header
  tests/
    gtest/
    ...
  bench/
    throughput.cpp
    latency.cpp
  examples/
    simple/
    async_file/
    custom_sink/
    tracing_hook/
    win32_rotate/
  docs/
    extraction-plan.md         # этот файл
    quickstart.md
    api.md
  conanfile.py
  vcpkg.json
  LICENSE
  NOTICE
```

---

## Жёсткие решения

1. **Корутины userver → `std::thread`**. Flush синхронный через `std::future.wait()`. Userver-специфичный `engine::Promise` отброшен.
2. **AsyncFlatCombiningQueue → `moodycamel::ConcurrentQueue`**. Vendored single-header, MIT, кросс-платформа.
3. **Component / DynamicConfig / YAML — вон из ядра**. Plain init API. YAML — опциональный модуль `ulog-yaml`.
4. **Span integration — public callback**. Не тащим `tracing::Span`. Пользователь сам регистрирует hook.
5. **Namespace `ulog`**. Global replace `USERVER_NAMESPACE` → `ulog`.
6. **Макросы `LOG_*` сохранены 1:1**. Feature flag `ULOG_NO_SHORT_MACROS` для conflict resolution (даёт только `ULOG_LOG_*`).
7. **Windows first-class**. Winsock2 для сокетов, `_open` для FD, `freopen` для rotation. AF_UNIX только на Win10 1803+. Signals → public `Reopen()` API, нет платформенных хаков в ядре.
8. **C++17 minimum, MSVC ≥ 2019**. C++20 — опциональные улучшения (`std::source_location`).

---

## Оценка трудозатрат

| Фаза | Дни |
|---|---|
| 0-1: setup + utils | 3-4 |
| 2-3: core + formatters | 5-7 |
| 4: dynamic debug / ratelimit / stacktrace | 1-2 |
| 5: sinks | 2-3 |
| 6: async logger | 4-7 |
| 7-8: rotation / config | 2-3 |
| 9: tracing hook | 1 |
| 10: OTLP optional | 2-3 |
| 11: pack / docs / bench | 2-3 |
| **Total (1 dev)** | **~22-35 дней** |

## Поэтапный релиз

- **v0.1** — Фазы 0-5. Sync logger + все sinks + все форматы. Уже юзабельно.
- **v0.2** — Фаза 6. Async logger.
- **v0.3** — Фазы 7-9. Rotation, span hook, builder config.
- **v1.0** — Фазы 10-11. OTLP + документация + бенчмарки.

## Риски

- **Perf regression vs TpLogger** — `moodycamel` близко к flat-combining, но обязательны бенчмарки на всех платформах.
- **Windows sinks edge cases** — отдельный CI job + unit tests.
- **Macro collisions `LOG_*`** — решается через `ULOG_NO_SHORT_MACROS` для пользователей с конфликтами (напр. glog).
- **Boost size** — heavy dep. Альтернатива в будущем — header-only only поднабор или свои замены для малых проектов.
- **License** — userver Apache-2.0 совместимо, NOTICE с attribution сохранить.
