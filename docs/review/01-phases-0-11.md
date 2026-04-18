# Review 01 — Phases 0-11 (foundation + hardening)

Scope: весь скелет ulog (42 файла, 48 тестов), ветка `feat/ulog-standalone`, пост-фикс rev. Охватывает фазы 0-11 плана.

## Overview

Вытащил logging из userver в standalone lib. Содержит:
- Cross-platform ядро: level/format/LogExtra/LogHelper + все `LOG_*` макросы 1:1.
- 4 форматтера (TSKV/LTSV/RAW/JSON), 6 sink типов, Sync + Async logger (`std::thread` + `moodycamel::ConcurrentQueue`).
- Dynamic debug per (file, line), stacktrace cache, rate limiting.
- Config builder (`LoggerConfig` + `MakeSync/Async/InitDefaultLogger`).
- Tracing callback hook (no built-in tracing dep).
- POSIX `SIGUSR1` → `RequestReopen` bridge.
- Conan 2 toolchain, Ninja build, MSVC 14.44 подтверждён.

## Initial findings (review pass 1)

### 🔴 Critical

1. **Race `SetDefaultLogger` vs `GetDefaultLogger`** — reader читал atomic raw-pointer без удержания shared_ptr. Concurrent set мог повалить dangling ref.
2. **`impl::TagWriter` не экспонировался** — публичный `LogHelper::GetTagWriter()` возвращал ссылку на класс, определённый приватно в .cpp. Пользователь не мог вызвать методы.
3. **File sink на Windows + rotation** — `fopen_s("ab")` открывал без `FILE_SHARE_DELETE`. Реальный `fs::rename` на открытом файле падал; тесты использовали `copy+resize_file` костыль.

### 🟡 Non-critical (в BACKLOG)

- `GetDefaultLogger() -> LoggerRef` оставлен для совместимости — задокументирован как короткоживущий.
- `LogHelper(LoggerRef)` публичный ctor обходит dynamic-debug фильтр — задокументировано как ответственность caller'а при ручном использовании.
- Макрос `ULOG_LOG_TO` имел двойной eval `(logger)`.
- Async `Flush()` без таймаута.
- POSIX poller thread never joined.
- Verbose Windows-paths в `module=` field.
- `JsonFormatter::Variant::kYaDeploy` — stub.
- Async path — heap alloc per LogRecord.
- RateLimiter без агрегированной drop metric.
- Dynamic debug берёт shared_lock даже при пустом registry.
- `ULOG_IMPL_SHOULD_NOT_LOG` — dead macro.

### Test coverage gaps (в BACKLOG)

- `LOG_LIMITED_*` drop count не проверяется.
- `DefaultLoggerGuard` — нет теста.
- Concurrent `SetDefaultLogger` + logging stress test — отсутствует.
- TCP/Unix sink smoke tests против локального listener — отсутствуют.
- `LogExtra::Stacktrace()` — не тестируется.
- Compile-erase `ULOG_ERASE_LOG_WITH_LEVEL` — не проверен build'ом.

## Fixes applied (review pass 2)

### 1. `boost::atomic_shared_ptr` для default logger

- `src/log.cpp`: внутренний slot — `boost::atomic_shared_ptr<impl::LoggerBase>` через `<boost/smart_ptr/atomic_shared_ptr.hpp>`.
- Conversion shim `std::shared_ptr ↔ boost::shared_ptr` через aliasing deleter — refcount корректный.
- Новый публичный API `GetDefaultLoggerPtr() -> LoggerPtr`: возвращает refcounted snapshot (refcount++), race-free.
- Старый `GetDefaultLogger() -> LoggerRef` оставлен + задокументирован как short-lived.
- Макросы default-logger (`LOG_INFO`, `LOG_ERROR`, …) теперь маршрут идёт через `GetDefaultLoggerPtr()`.
- `ULOG_LOG_TO` перестроен на `if (auto&& logger__ = (expr); ...)` + `std::forward<decltype>` — один eval expression, forward сохраняет value category.
- `LogHelper(LoggerPtr, Level, LogRecordLocation)` — новый ctor, хранит shared_ptr в `Impl::logger_owner` на всё время жизни записи.

### 2. `impl::TagWriter` публичный

- Новый заголовок `include/ulog/impl/tag_writer.hpp` — класс + сигнатуры.
- Реализация в `src/impl/tag_writer.cpp`.
- Приватный дубликат из `log_helper.cpp` удалён.
- `Reset(formatter*)` — для LogHelper active/inactive свитча.

### 3. Windows FileSink + честный rotation

- `src/detail/file_handle.cpp` Windows ветка: `CreateFileA` с `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE` → `_open_osfhandle` → `_fdopen`.
- `DELETE` share flag позволяет внешнему процессу rename/remove файла пока ulog держит handle.
- `CFileHandle::Reopen` — close + fresh open (вместо `freopen`, который не даёт переопределить share flags).
- `FdHandle::OpenAppend` — аналогично.
- `tests/sink_test.cpp::FileSink.ReopenAfterRotate` — убран `copy+resize_file` костыль; используется реальный `fs::rename(tmp, rotated); sink->Reopen();`.

### 4. `docs/BACKLOG.md`

Зафиксированы deferred items:
- RateLimiter global drop metric (3 варианта: atomic counter / per-line registry / callback).
- Observability gaps (queue depth, record length histogram, sink write latency).
- Correctness: deprecation `GetDefaultLogger`, manual `LogHelper(LoggerRef)` использование, multi-hook tracing.
- Performance: TLS cache для default logger snapshot, async payload allocation, dynamic_debug empty fast-path, source-root filepath trim.
- Coverage: LOG_LIMITED drops, DefaultLoggerGuard, concurrent stress, TCP/Unix smoke, stacktrace cache, ERASE build assertion.
- Build/CI: GitHub Actions matrix, Google Benchmark suite.
- Features: YaDeploy JSON, yaml-cpp config loader, OTLP sink, Windows named-event reopen.

## Final state

- Сборка: зелёная (Windows MSVC 14.44, Ninja, Conan 2).
- Тесты: **48/48 passing** across 17 suites.
- Примеры: simple / async_file / tracing_hook / custom_sink — все работают.
- Windows rotation: подтверждён реальным `fs::rename` (без костылей).
- Критичные замечания из rev.1 — устранены.
- Всё deferred — в `docs/BACKLOG.md` для следующих фаз.

## Acceptance: ✅ ready to commit + advance to next phase
