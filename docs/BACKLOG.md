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

---

## LogHelper producer-side allocator optimizations (userver parity)

Эти пункты закрывают gap с userver `LogHelper`. Main finding: userver убирает per-record heap alloc через `ThreadLocalMemPool<Impl>` и гарантирует `noexcept` на всём streaming pipeline. Наш LogHelper делает `make_unique<Impl>` + `make_unique<TextLogItem>` per record — 2 heap hit'а на горячем пути.

Реализовать одной фазой (1 день).

### Priority 1: `ThreadLocalMemPool<LogHelper::Impl>`
**Что:** TLS кеш на 16 slabs объектов `LogHelper::Impl`. Pop-placement-new в ctor, dtor-then-Push в `~LogHelper`. Первые 16 logs в thread аллоцируют, дальше — zero heap alloc для Impl.
**Зачем:** `make_unique<Impl>` + dtor = ~60-100 ns + malloc contention. На 1.3M rec/s bench = ~13% producer-side cost.
**Как:** см. userver `universal/src/logging/log_helper.cpp:60-107`. Реализация:
```cpp
template <typename T, std::size_t N = 16>
class ThreadLocalMemPool {
    struct alignas(T) Slot { std::byte raw[sizeof(T)]; };
    thread_local static std::array<Slot*, N> cache;
    thread_local static std::size_t count;

    template <typename... Args>
    static std::unique_ptr<T, Deleter> Pop(Args&&... args);
    static void Push(std::unique_ptr<T, Deleter> obj) noexcept;
};
```
Thread-safe by TLS. На thread exit residual slabs leak-freed через `thread_local` cleanup (destructor с проходом по `cache[0..count]`).
**Осторожно:** `LogHelper::Impl` сейчас содержит `SmallString<1024> text`, `std::optional<TagRecorder>`, `std::optional<FanoutFormatter>`, `std::vector<BasePtr> extras`. Pop reuse работает только если Impl после dtor восстанавливается до initial state — либо явный `Reset()` метод вместо dtor+ctor, либо destroy_at + placement-new заново (userver'овский путь). Последний чище.
**Effort:** средний (1 день с тестами).
**Impact:** высокий (~13% producer throughput).

### Priority 2: `noexcept operator<<` + `InternalLoggingError`
**Что:** все `operator<<` и `Put/PutFormatted` mark `noexcept`, внутри try/catch, при exception → fputs на stderr + mark Impl broken.
**Зачем:**
- Caller может вызывать `LOG_INFO()` из `noexcept` контекста (dtor, signal handler) без абортa программы.
- fmt::format может OOM-throw; сейчас это выходит через LogHelper::operator<< наружу, нарушая invariant "логирование не должно валить процесс".
**Как:** userver schema — `InternalLoggingError(msg)` + `pimpl_->MarkAsBroken()`. Broken Impl → dtor skips `DoLog`, все последующие `<<` — no-op.
**Effort:** маленький (2-3 часа).
**Impact:** высокий для reliability (не регресс на perf — try/catch zero-cost при no-throw).

### Priority 3: `kSizeLimit = 10000` truncation
**Что:** после 10KB текста в Impl — `<<` становится no-op, эмитится `truncated=true` tag.
**Зачем:** защита от runaway `<<` loops; злонамеренный / багованный caller не может исчерпать память.
**Как:** `LogHelper::IsLimitReached()` → `pimpl_->GetTextSize() >= kSizeLimit`. Put'ы проверяют перед append'ом.
**Effort:** маленький (1 час).
**Impact:** низкий в обычном случае, высокий в edge case.

### Priority 4: `DoLog()` как explicit finalization point
**Что:** вынести финализацию (SetText → ExtractLoggerItem → logger.Log → Flush) в отдельный `void DoLog() noexcept` метод. `~LogHelper` вызывает `DoLog()` then `Push(Impl)` в pool.
**Зачем:** enables Priority 1 (без explicit finalize point pool Push не знает когда Impl готов к переиспользованию). Также — явная точка для unit-testing finalization без dtor ceremony.
**Effort:** рефактор, embedded в Priority 1. Без Priority 1 не даёт value.

### Priority 5: Combined Impl+formatter single heap alloc (опционально)
**Что:** arena — размещать `Impl` + primary `TskvFormatter` (или др.) + `TextLogItem` в одном heap block через placement-new.
**Зачем:** после Priority 1 remaining alloc — formatter+item (~540 байт). Один большой alloc вместо двух малых.
**Блок:** формттер size варьируется (TSKV ~256, JSON больше, OTLP больше). Нужен runtime sizing или worst-case allocation. Усложняет `MakeFormatterInto` API.
**Effort:** средний (1 день с осторожным lifetime handling).
**Impact:** средний — ~50 ns save after Priority 1.

### Priority 6: per-logger `PrependCommonTags` virtual (уже close)
**Что:** virtual hook `LoggerBase::PrependCommonTags(TagWriter&)` вызывается в `LogHelper` ctor, добавляет service-wide tags (hostname, service, version) в каждую запись.
**Статус:** у нас есть `RecordEnricher` — global hooks. Per-logger hook отсутствует; текущие enricher'ы runtime-регистрируются в одном global slot. Userver допускает multiple loggers с разными common tags.
**Effort:** маленький (2 часа).
**Impact:** средний для multi-tenant services.

### Summary benchmark target

Цель фазы: `BM_SyncLoggerThroughput` 711 ns → **≤ 600 ns** (userver parity), `BM_AsyncLoggerThroughput` 806 → **≤ 700 ns** producer-side. Bench before/after обязателен; если Priority 1 даёт <5% — пересмотр подхода (возможно Impl слишком heavy для pool).
