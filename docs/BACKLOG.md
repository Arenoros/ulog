# ulog — backlog

## Observability / metrics

### RateLimiter per-source callback (из BACKLOG rev.1)
**Что:** `SetRateLimitDropHandler(fn)` — вызов per drop event.
**Зачем:** push to monitoring system без pull'а глобального счётчика.
**Effort:** маленький (2 часа).
**Impact:** низкий (global counter уже есть).

---

## Transport / delivery

### `OtlpBatchSink` через HTTP/JSON POST (из review 12)
**Что:** sink, собирающий N записей → оборачивает в `ExportLogsServiceRequest` → POST `http://collector:4318/v1/logs`.
**Зачем:** без otel-collector-sidecar, прямой push в backend (Grafana Cloud, Honeycomb).
**Как:** добавить HTTP client. Варианты:
- `cpp-httplib` (single-header, MIT, 150KB) — easy add.
- `libcurl` — fattt dep, but battle-tested.
- Свой winhttp + POSIX — портируемо но много кода.
**Effort:** средний (1 день с cpp-httplib).
**Impact:** средний — многие деплои уже используют collector-sidecar.

### `OtlpGrpcSink` через opentelemetry-cpp (BACKLOG)
**Что:** полноценный OTLP/gRPC.
**Зачем:** canonical OTLP transport, стриминг, mTLS, compression.
**Как:** зависимость `opentelemetry-cpp` (требует grpc + protobuf — много).
**Effort:** большой (2-3 дня + boxing grpc через Conan).
**Impact:** низкий для большинства — HTTP/JSON покрывает 90%.

### Windows AF_UNIX тест (BACKLOG)
**Что:** активировать `UnixSocketSink` на Win10 1803+ через `#define ULOG_HAVE_AFUNIX`, test.
**Effort:** маленький (30 мин).
**Impact:** нишевой (редкий deploy scenario).

---

## Performance

### Multi-worker async logger (из review 20)
**Что:** пул worker threads на один queue (backed by moodycamel SPMC).
**Зачем:** больше чем 4M rec/s single-consumer — если sink параллелится.
**Effort:** большой (2 дня — queue redesign, per-sink lock contention).
**Impact:** нишевой — мало кто упирается в 4M rec/s.

---

## Config / integrations

### yaml-cpp config loader (из BACKLOG rev.1)
**Что:** `ulog::LoggerConfig ParseYamlConfig(const YAML::Node&)`.
**Зачем:** YAML конфиг для services (userver-style).
**Блок:** `yaml-cpp` нет в conan cache — собрать потребует fresh build (легче boost'а).
**Effort:** маленький (1-2 часа).
**Impact:** низкий — C++ builder API уже есть.

### Logger configuration reload (new)
**Что:** watcher на config файл → hot reload уровня + sinks без перезапуска.
**Зачем:** change verbosity в production без restart.
**Effort:** средний.
**Impact:** средний.

---

## Coverage / testing gaps

### TCP reopen с multi-accept listener (из review 14)
**Что:** listener, принимающий >1 conn'a; тест `Reopen` + reconnect.
**Зачем:** current test покрывает только disconnect contract, не reconnect.
**Effort:** маленький (1 час — переделать LocalListener в event-loop).
**Impact:** средний coverage.

### Compile-erase binary assertion (из BACKLOG rev.1)
**Что:** build с `ULOG_ERASE_LOG_WITH_LEVEL=3` → dumpbin/objdump on Windows/Linux → assert strings "LOG_TRACE text" нет в binary.
**Зачем:** верифицировать что `ULOG_ERASE_*` действительно удаляет код.
**Effort:** средний (CMake fixture + platform-specific tool invocation).
**Impact:** низкий.

### AddJsonTag + YaDeploy combo test (из review 21)
**Что:** добавить тест `YaDeploy + AddJsonTag` — покрывается только строковая версия.
**Effort:** маленький (10 мин).
**Impact:** маленький coverage.

### LogHelper::WithException test (из BACKLOG rev.1)
**Что:** verify `WithException(ex)` кладёт `exception_type` + `exception_msg` в record.
**Effort:** маленький.
**Impact:** coverage.

### Stacktrace с реальной символизацией (из BACKLOG)
**Что:** fully check `LogExtra::Stacktrace()` output содержит function names, file paths.
**Проблема:** зависит от PDB availability (MSVC) / debug symbols (gcc).
**Effort:** средний.
**Impact:** низкий.

---

## Build / infrastructure

### Google Benchmark в CI (из BACKLOG)
**Что:** отдельный job запускает `ulog-bench`, сохраняет JSON result как artifact. Опционально compare with baseline.
**Effort:** средний.
**Impact:** средний — tracking perf regressions.

---

## API / polish

### `ULOG_PURGE_TLS_DEFAULT_CACHE()` escape hatch (из review 14)
**Что:** public API чтобы thread сбрасывал TLS cache после `SetDefaultLogger(nullptr)`.
**Зачем:** long-lived threads держат ref на старый logger до следующего LOG_*.
**Effort:** маленький.
**Impact:** нишевой.

### `[[deprecated]]` на `GetDefaultLogger() -> LoggerRef` (из review 01)
**Что:** annotate для предупреждения о короткоживущем контракте.
**Effort:** маленький.
**Impact:** маленький — API signals.

### Source-root trim: документировать PUBLIC consumer override (из review 02)
**Что:** README + config.cmake — как downstream консумер переопределяет `ULOG_SOURCE_ROOT`.
**Effort:** маленький.
**Impact:** маленький.

---

## Удобство разработчика

### `ULOG_EXAMPLES_DIR` пример с structured JSON consumer
**Что:** пример "tail -f ulog.jsonl | jq" — показать user'у что он получает.
**Effort:** маленький.

### Doxygen config
**Что:** `docs/Doxyfile`, генерация HTML reference.
**Effort:** маленький.

---

## Priority matrix

### Высокий impact, средний effort
1. **OtlpBatchSink HTTP** — real OTLP transport без sidecar.

### Средний impact
2. **Compile-erase assertion test**.
3. **TCP multi-accept reopen тест**.
4. **YAML config loader**.

### Низкий приоритет
5. **OtlpGrpcSink**.
6. **Multi-worker async**.
7. **Hot config reload**.
8. **Doxygen**.

---

## Рекомендация

Следующая фаза — **Phase 24: perf regression fix + CI polish**:
- Moodycamel per-producer token caching (фикс 8T regression).
- CI conan cache + C++20 matrix row + test-log artifact upload.

Все — маленькие, комплементарные: token caching возвращает перф на 8T, CI улучшения ускоряют CI pipeline (sanitizer matrix из Phase 23 сейчас пересобирает conan deps с нуля каждый запуск).
