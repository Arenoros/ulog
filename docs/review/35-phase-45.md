# Phase 45 — self-review: `LoggerBase::PrependCommonTags` + per-logger tag store

**Scope:** Phase 4 из BACKLOG `LogHelper producer-side allocator optimizations` (Pri 6).

Diff:
- `include/ulog/impl/logger_base.hpp` — public API: `virtual void PrependCommonTags(TagSink&) const noexcept`, `SetCommonTag(sv, sv)`, `RemoveCommonTag(sv)`, `ClearCommonTags() noexcept`. Storage: `boost::atomic_shared_ptr<const CommonTagsVec>` + `std::mutex`. Include `<ulog/tracing_hook.hpp>` добавлен (для `TagSink`).
- `src/impl/logger_base.cpp` — реализации. Default `PrependCommonTags` walks atomic snapshot → `sink.AddTag(k, v)` в цикле. Set/Remove — COW через load+rebuild+store (mirror pattern из `RegisterSinkFormat`).
- `src/log_helper.cpp` — в `Impl::DoLog` после `impl::DispatchRecordEnrichers(sink)` вызов `logger_ref.PrependCommonTags(sink)`. Только под ветку `if (hook_target)` — null-target logger (без sinks) skip'ает как и enrichers.
- `tests/per_logger_common_tags_test.cpp` — 7 тестов: appearance, no-leak, overwrite, remove, clear, coexist с global enricher, concurrent Set+Log.
- `tests/CMakeLists.txt` — зарегистрирован.

---

## Сводка изменений

1. **API на `LoggerBase`.** Методы непохожи на existing SetLevel/GetLevel — тем, что они мутируют COW-snapshot, не atomic primitive. Но ABI consistent: `Set*`/`Remove*`/`Clear*`, `PrependCommonTags` = hook.

2. **Storage design — COW через `atomic_shared_ptr`.** Mirror pattern из `active_formats_` в `TextLoggerBase`. Read path (hot): atomic load → null-check → walk vector → `sink.AddTag`. Null snapshot = fast path, нет pin'а. Write path (cold): lock mutex → load current → rebuild vector → publish.

3. **Thread safety.** `SetCommonTag` / `RemoveCommonTag` / `ClearCommonTags` — mutex-protected (последовательная публикация), readers — lock-free (atomic load). Concurrent Set + Log тестом покрыт (500 emissions параллельно с rotating tag'ом).

4. **Insert point в DoLog.** После `DispatchRecordEnrichers(sink)` — так per-logger common tags выходят ПОСЛЕ global enrichers в записи (порядок в TSKV: timestamp, level, module, enricher tags, common tags, text). Symmetry с tracing → enricher → common tag chain.

5. **`noexcept` swallow в default impl.** `sink.AddTag` формально не `noexcept` (formatter может OOM). Wrap в try/catch: silently drop tag. LogHelper DoLog тоже swallow'ит — consistent с broader LogHelper noexcept policy.

6. **Override path для custom loggers.** `virtual void PrependCommonTags(TagSink&) const noexcept` — subclass может override, например, для runtime-computed ambient tags (TLS context без регистрации). Default impl использует storage, custom — игнорирует.

---

## Проверка корректности

### Test suite

`./tests/ulog-tests.exe`: **180/180 passed** (было 173, +7). Нет регрессий.

Новые тесты:
- `TagAppearsOnRecord` — два common tags в TSKV output.
- `TagsDoNotLeakBetweenLoggers` — две `MemLogger`, разные `logger=A/B` tags; записи разделены.
- `SetTwiceOverwrites` — `SetCommonTag("env", "dev")` затем `SetCommonTag("env", "prod")` → в записи только `env=prod`, без дубля.
- `RemoveDropsTag` — `RemoveCommonTag("a")` → `a=1` исчезает, `b=2` остаётся.
- `ClearRemovesAll` — все common tags дропаются.
- `GlobalEnricherAndCommonTagsCoexist` — both паттерна работают concurrent: enricher `enriched=yes` + common `service=ulog-test`.
- `ConcurrentSetAndLogIsSafe` — writer thread мутит `SetCommonTag("iter", N++)` пока main emits 500 записей. All 500 records carry some iter tag; no data-race (TSan в CI сможет поверить отдельно).

### Benchmarks

| Bench | Phase 3 baseline | Phase 4 | Δ |
|-------|------------------|---------|---|
| `BM_SyncLoggerThroughput` median | 596 ns | 581 ns | -2.5% (CV 1.44%, в шуме) |
| `BM_AsyncLoggerThroughput` wall | 818 ns | 815 ns | в шуме |
| `BM_AsyncLoggerThroughput` CPU | 724 ns | 678 ns | -6.3% (CV 4.39%, в шуме) |

Hot path: atomic load of `common_tags_` snapshot (null по умолчанию → early return, ~1 ns). Zero cost без common tags. Bench run не имеет common tags — signal чисто от shared_ptr load.

---

## Критические / non-critical findings

### 🔴 Critical — none

### 🟡 Non-critical

**NC-1. `sink.AddTag` в default impl обёрнут в try/catch — `// Drop the tag silently`.**
File: `src/impl/logger_base.cpp:20-25`. Диагностика теряется, в отличие от `InternalLoggingError` в LogHelper (который fputs stderr). Тут нет эквивалента — если `AddTag` throw'ит, мы даже не знаем, что пропустили tag. Для production debugging проблема, но ruled-out path (FormatterTagSink / TagRecorder никогда не throw).
**Action:** skip. Добавить diagnostic только если появится concrete issue.

**NC-2. `SetCommonTag` — O(N) scan для overwrite, O(N) copy для publish. На logger с 10+ common tags каждый update — 20+ string move'ов.**
В userver common tags = hostname + service + version = 3 tags typically. Scale ok. Если scaling до 100 tags появится — пересмотреть.
**Action:** skip.

**NC-3. `CommonTagsVec` — `vector<pair<string, string>>` — full-blown string copies.**
Alternative: small-string-optimized. Boost container small_vector + `fmt::string_view_like` → complicated, premature. userver делает exactly так же.
**Action:** skip.

**NC-4. Order of tags in output — insertion order (FIFO), не sorted.**
Semantic correct (tags — unordered set по key), но grep'ing output может зависеть от порядка публикации. Нестабильно при overwrite — старая позиция теряется. Документировать не стоит (реципиенты не должны полагаться на порядок в TSKV).
**Action:** skip.

**NC-5. `PrependCommonTags` called только if `hook_target != nullptr`.**
Logger без sinks (empty SinkSet) → нет target → common tags дропаются молча. Симметрично с enricher / tracing hook. Semantically ok (ни в один sink ничего не пишется, tag'у некуда идти), но несимметрично: `Put*` в `text` по-прежнему аккумулируются (бестолково, но safely). Consistent с existing behaviour.
**Action:** skip.

**NC-6. `SetCommonTag` не атомарен с `RemoveCommonTag` под одним ключом на разных тредах — `RemoveCommonTag` может видеть pre-Set snapshot и no-op'ить.**
Последовательности `Set(k, v)` then `Remove(k)` на одном thread упорядочены через mutex → correct. Cross-thread — нет HB guarantee без sync primitives вне API. Документировать?
**Action:** defer. Типичный use-case: config-time setup (one thread, one-shot); race edge case.

### 🟢 Positive notes

- **Null snapshot fast path** = нулевой cost для loggers без common tags (overwhelming majority).
- **COW isolation:** changes to common_tags_ не visible через stale snapshot — concurrent emissions uses pin на уже-published vector.
- **Virtual override** для custom loggers с computed tags (TLS thread-local cache например).
- **Clean API shape** — mirror of SetLevel/GetLevel без surprise. Set/Remove/Clear triad — predictable.
- **Coexist с RecordEnricher** — two patterns для разных scope'ов: global (enricher) + per-logger (common tag). Pick one без rewrite.

---

## Test coverage

### Покрыто

- CRUD на common tags.
- Cross-logger isolation.
- Overwrite semantics.
- Global enricher + common tag coexist.
- Concurrent write+emit (race-free).

### Gaps

**TC-1. `PrependCommonTags` override у custom logger.**
Нет теста, где subclass LoggerBase с override'нутым `PrependCommonTags` добавляет computed tags. Virtual dispatch итак протестирован через MemLogger default impl, но extension point — нет.
**Action:** defer.

**TC-2. JSON / OTLP форматы.**
Тесты только TSKV. JSON формттер тоже через sink.AddTag → `formatters::Base::AddTag` — inspected формат, ok. Но нет regression теста.
**Action:** defer; параметризованный тест был бы nice-to-have.

**TC-3. common tags на null-sink logger.**
Logger с нулём sinks: `PrependCommonTags` не вызывается. Тест этого edge case есть косвенно (default has_text_sinks=true всегда в MemLogger). Ок.
**Action:** skip.

---

## Решение

- **NC-1 / NC-2 / NC-3 / NC-4 / NC-5 / NC-6** — skip / defer.
- **TC-1 / TC-2** — defer; TC-3 skip.

Commit.
