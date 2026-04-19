# ulog — backlog

## Transport / delivery

### `OtlpGrpcSink` через opentelemetry-cpp (BACKLOG)
**Что:** полноценный OTLP/gRPC.
**Зачем:** canonical OTLP transport, стриминг, mTLS, compression.
**Как:** зависимость `opentelemetry-cpp` (требует grpc + protobuf — много).
**Effort:** большой (2-3 дня + boxing grpc через Conan).
**Impact:** низкий для большинства — HTTP/JSON покрывает 90%.

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

### Compile-erase binary assertion (из BACKLOG rev.1)
**Что:** build с `ULOG_ERASE_LOG_WITH_LEVEL=3` → dumpbin/objdump on Windows/Linux → assert strings "LOG_TRACE text" нет в binary.
**Зачем:** верифицировать что `ULOG_ERASE_*` действительно удаляет код.
**Effort:** средний (CMake fixture + platform-specific tool invocation).
**Impact:** низкий.

### Stacktrace с реальной символизацией (из BACKLOG)
**Что:** fully check `LogExtra::Stacktrace()` output содержит function names, file paths.
**Проблема:** зависит от PDB availability (MSVC) / debug symbols (gcc).
**Effort:** средний.
**Impact:** низкий.

---

## Priority matrix

### Средний impact
1. **Compile-erase assertion test**.
2. **YAML config loader**.

### Низкий приоритет
3. **OtlpGrpcSink**.
4. **Multi-worker async**.
5. **Hot config reload**.

---

## Рекомендация

Следующая фаза — **Phase 24: perf regression fix + CI polish**:
- Moodycamel per-producer token caching (фикс 8T regression).
- CI conan cache + C++20 matrix row + test-log artifact upload.

Все — маленькие, комплементарные: token caching возвращает перф на 8T, CI улучшения ускоряют CI pipeline (sanitizer matrix из Phase 23 сейчас пересобирает conan deps с нуля каждый запуск).
