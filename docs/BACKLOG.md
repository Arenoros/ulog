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

### Priority 1: `ThreadLocalMemPool<LogHelper::Impl>` ✅ DONE (Phase 42)
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

### Priority 2: `noexcept operator<<` + `InternalLoggingError` ✅ DONE (Phase 43)
**Что:** все `operator<<` и `Put/PutFormatted` mark `noexcept`, внутри try/catch, при exception → fputs на stderr + mark Impl broken.
**Зачем:**
- Caller может вызывать `LOG_INFO()` из `noexcept` контекста (dtor, signal handler) без абортa программы.
- fmt::format может OOM-throw; сейчас это выходит через LogHelper::operator<< наружу, нарушая invariant "логирование не должно валить процесс".
**Как:** userver schema — `InternalLoggingError(msg)` + `pimpl_->MarkAsBroken()`. Broken Impl → dtor skips `DoLog`, все последующие `<<` — no-op.
**Effort:** маленький (2-3 часа).
**Impact:** высокий для reliability (не регресс на perf — try/catch zero-cost при no-throw).

### Priority 3: `kSizeLimit = 10000` truncation ✅ DONE (Phase 44)
**Что:** после 10KB текста в Impl — `<<` становится no-op, эмитится `truncated=true` tag.
**Зачем:** защита от runaway `<<` loops; злонамеренный / багованный caller не может исчерпать память.
**Как:** `LogHelper::IsLimitReached()` → `pimpl_->GetTextSize() >= kSizeLimit`. Put'ы проверяют перед append'ом.
**Effort:** маленький (1 час).
**Impact:** низкий в обычном случае, высокий в edge case.
**Review:** `docs/review/34-phase-44.md` — 173/173 tests pass (+3), bench neutral (+1.9% sync в шуме).

### Priority 4: `DoLog()` как explicit finalization point ✅ DONE (Phase 42, embedded)
**Что:** вынести финализацию (SetText → ExtractLoggerItem → logger.Log → Flush) в отдельный `void DoLog() noexcept` метод. `~LogHelper` вызывает `DoLog()` then `Push(Impl)` в pool.
**Зачем:** enables Priority 1 (без explicit finalize point pool Push не знает когда Impl готов к переиспользованию). Также — явная точка для unit-testing finalization без dtor ceremony.
**Effort:** рефактор, embedded в Priority 1. Без Priority 1 не даёт value.

### Priority 5: Combined Impl+formatter single heap alloc (опционально) 🟡 DEFERRED (Phase 46)
**Что:** arena — размещать `Impl` + primary `TskvFormatter` (или др.) + `TextLogItem` в одном heap block через placement-new.
**Зачем:** после Priority 1 remaining alloc — formatter+item (~540 байт). Один большой alloc вместо двух малых.
**Блок:** формттер size варьируется (TSKV ~256, JSON больше, OTLP больше). Нужен runtime sizing или worst-case allocation. Усложняет `MakeFormatterInto` API.
**Effort:** средний (1 день с осторожным lifetime handling).
**Impact:** средний — ~50 ns save after Priority 1.
**Defer:** `docs/review/36-phase-46-deferral.md` — cross-thread TextLogItem lifetime (async queue owns past producer), marginal payoff на baseline sync 580 ns. Re-open on real-world allocator-pressure evidence.

### Priority 6: per-logger `PrependCommonTags` virtual (уже close) ✅ DONE (Phase 45)
**Что:** virtual hook `LoggerBase::PrependCommonTags(TagWriter&)` вызывается в `LogHelper` ctor, добавляет service-wide tags (hostname, service, version) в каждую запись.
**Статус:** у нас есть `RecordEnricher` — global hooks. Per-logger hook отсутствует; текущие enricher'ы runtime-регистрируются в одном global slot. Userver допускает multiple loggers с разными common tags.
**Effort:** маленький (2 часа).
**Impact:** средний для multi-tenant services.
**Review:** `docs/review/35-phase-45.md` — 180/180 tests pass (+7), bench neutral (sync -2.5% в шуме).

### Summary benchmark target

Цель фазы: `BM_SyncLoggerThroughput` 711 ns → **≤ 600 ns** (userver parity), `BM_AsyncLoggerThroughput` 806 → **≤ 700 ns** producer-side. Bench before/after обязателен; если Priority 1 даёт <5% — пересмотр подхода (возможно Impl слишком heavy для pool).

---

## OTLP tracing integration via `LogClass::kTrace`

Дополняет Phase 22 (OTLP logs). Userver пропускает spans через ту же `LogHelper` машинерию что и логи — различает их tag'ом `LogClass`. Даёт unified emission path: logs → OTLP `/v1/logs`, spans → OTLP `/v1/traces`, один sink batch'ит оба.

**Как в userver:** `logging::LogClass { kLog, kTrace }`. Constructor `LogHelper(logger, level, LogClass, location)`. `tracing::Span::~Impl` эмитит запись со `kTrace` + accumulated span tags (trace_id/span_id/start_time/duration/parent_link). `Struct` formatter хранит `log_class` в LogItem — structured sink маршрутизирует.

**Зачем:** без этого span emission через logging pipeline невозможен — пользователю приходится городить parallel tracing SDK. Для OTLP-first deployments (Grafana Tempo / Honeycomb / Jaeger) каждая корутина = span, spans должны эмититься автоматически на scope exit без ручного `LOG_INFO << "span done"`.

### Scope фазы

#### Step 1: `enum class LogClass { kLog, kTrace }`
- В `include/ulog/log_helper.hpp` добавить enum + default parameter `LogClass::kLog`.
- `LogHelper` ctor принимает optional 4-й аргумент — обратно совместимо с существующими LOG_* макросами.

#### Step 2: pipeline carries log_class
- `formatters::Base::Base(Level, LogClass, const LogRecordLocation&)` — передача через MakeFormatter.
- `LoggerBase::MakeFormatter(Level, LogClass, const LogRecordLocation&)` — extend signature.
- `sinks::LogRecord` (structured) добавить поле `LogClass log_class{LogClass::kLog};`.

#### Step 3: `tracing::Span`-like helper (optional)
- Минимальная реализация `ulog::SpanScope` / `tracing::Span` — RAII helper с dtor'ом, эмитящим LogHelper kTrace. Не обязательно в ulog core — может быть отдельный `ulog-tracing` target с dep на pattern, позволяющий пользователю писать:
```cpp
{
    ulog::SpanScope sp("db_query");
    sp.AddTag("query_id", qid);
    // work...
} // dtor emits kTrace record
```
- Собирает trace_id/span_id (через user's tracing system или внутренний randomID), duration, link.

#### Step 4: OTLP формттер routes по `LogClass`
- `OtlpJsonFormatter` смотрит `log_class` в ctor, выбирает schema:
  - `kLog` → `opentelemetry.proto.logs.v1.LogRecord` (текущий output).
  - `kTrace` → `opentelemetry.proto.trace.v1.Span` (новый):
    ```json
    {
      "traceId": "...",
      "spanId": "...",
      "parentSpanId": "...",
      "name": "db_query",
      "startTimeUnixNano": "...",
      "endTimeUnixNano": "...",
      "attributes": [...],
      "status": {"code": "STATUS_CODE_OK"}
    }
    ```
- Один format emits либо LogRecord либо Span per line. Или отдельные формттеры `OtlpLogsJsonFormatter` / `OtlpTracesJsonFormatter` + dispatcher по LogClass.

#### Step 5: OTLP batch/HTTP sink splits по класс
- `OtlpBatchSink` (из BACKLOG) при HTTP POST разделяет accumulated records:
  - `{kLog}` batch → `POST /v1/logs` с `ExportLogsServiceRequest` envelope.
  - `{kTrace}` batch → `POST /v1/traces` с `ExportTraceServiceRequest` envelope.
- Без OtlpBatchSink: filelog-collector читает JSONL с mixed kLog/kTrace, receiver'ы различают по schema shape.

### Coverage
- Test: emit span запись через `SpanScope` → assert правильный Span JSON в mem_logger.
- Test: Log + Span через один sync logger → оба корректные, разные schema.
- Test: `tracing::Span` nested (parent/child) — parent_span_id корректно заполнен.

### Без чего можно

- SpanScope helper — опционально. Без него пользователь вручную: `LogHelper lh(logger, lvl, kTrace, loc); lh << ulog::LogExtra{{"trace_id", ...}, {"span_id", ...}, {"duration_ns", ...}};`. Ugly но работает.
- LogClass пропуск через pipeline без SpanScope также даёт value — tracing SDK третьей стороны может эмитить spans через LogHelper напрямую.

### Effort / Impact

**Effort:** большой (3-4 дня).
- Step 1-2 (signature extension через pipeline): 1 день, рефактор + обновление 4 формттеров + 4 loggers.
- Step 4 (OTLP Span schema): 1 день, Span JSON отличается от LogRecord в нескольких полях + nested attributes / events.
- Step 3 (SpanScope): 0.5 дня, если минимальный (без parent context propagation).
- Step 5 (OtlpBatchSink split): зависит от OtlpBatchSink из BACKLOG — вместе 2 дня.

**Impact:** высокий для OTLP-first deployments. Без span emission ulog = "только logs" в OpenTelemetry экосистеме; со Step 1-4 — полный logs+traces.

### Связано

- Phase 22 (OTLP logs) — прошлая фаза, эта достраивает tracing половину.
- `SetTracingHook` — текущий механизм инъекции trace_id в обычные логи. **Не заменяется** — по-прежнему нужен для logs↔traces correlation в kLog записях. SpanScope — ортогональный путь для explicit span emission.
- BACKLOG "`trace_id` / `span_id` → top-level в OTLP LogRecord" (review 12) — будет использован когда ulog эмитит kLog ассоциированные со span scope (hook видит active span в TLS).
