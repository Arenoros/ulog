# Review 11 — Phase 21 (direct-emit JSON formatter)

Scope: retire the staging `std::vector<Field>` in `JsonFormatter`. Tags now stream directly into the TextLogItem buffer the same way the TSKV / LTSV formatters already do.

## Changes

### Before

`JsonFormatter::AddTag` pushed into `std::vector<Field>{key, value, is_json}`. `AddJsonTag` did the same. `SetText` cached text. `ExtractLoggerItem` walked the vector, escaped each key and value on the fly, wrote them out. Per-record overhead included: vector growth, per-field `std::string` copies for both key and value, and a second escape pass at finalize.

### After

- `JsonFormatter` ctor writes the opening `{` plus the first field (timestamp). Subsequent fields are appended via `EmitField`, which writes `","key":value` into the TextLogItem buffer immediately.
- `SetText` still caches text locally because it must appear last regardless of when it was called; `ExtractLoggerItem` emits `"text":"<cached>"` + `}\n`.
- Escape path shared: `AppendJsonEscaped` is now templated over the sink type, used for both the TextLogItem buffer and the staged text string.
- YaDeploy variant: `TranslateKey` remains; every emission path (ctor, AddTag, Finalize) funnels its key through it.

### Layout

- `include/ulog/impl/formatters/json.hpp`: drop `struct Field`, drop `std::vector<Field> fields_`. Add private `EmitField` + `TranslateKey`.
- `src/impl/formatters/json.cpp`: rewrite to stream directly. Control-character escape uses `std::snprintf` into a tiny stack buffer (7 bytes) — avoids the `std::back_inserter`/fmt gymnastics that didn't compose cleanly with SmallString.

## Performance

MSVC 14.44 RelWithDebInfo, `BM_FormatThroughput` with two `LogExtra` tags:

| Format | Pre-Phase 21 | Post-Phase 21 | Delta |
|---|---|---|---|
| `kTskv` | 826 ns | 777 ns | ~same |
| `kLtsv` | 825 ns | 824 ns | ~same |
| `kRaw` | 346 ns | 324 ns | ~same |
| `kJson` | **1235 ns** | **808 ns** | **-35 %** |

JSON now on par with TSKV / LTSV. No change to core / async / sync benches (they all used TSKV in the discard-sink scenario).

## Results

- **Build**: green.
- **Tests**: **67/67 passing**, JSON-specific tests (`EmitsJsonObject`, `EscapesQuotesInValue`, `YaDeployVariantRenamesCoreFields`) all still pass against the new emission order.
- **Example**: `ulog-example-async` writes JSON to a file; smoke-check shows the output is well-formed.

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **`text_` still staged in std::string**. Required because `text` must appear last in the object and SetText can arrive before tags. A reorganization of the LogHelper dtor to call SetText *after* all tags have been emitted would let us direct-emit text too. Minor saving; deferred.
- **Control-character escape uses `std::snprintf`** into a 7-byte stack buffer. Portable and fast enough; the fmt-based earlier version was 1-2 ns faster in ideal cases but pulled in `<iterator>` + a templated back_inserter that fought `SmallString`.
- **No direct test for YaDeploy + explicit JsonString tag** (AddJsonTag path with variant). Existing `YaDeployVariantRenamesCoreFields` covers plain string tags only. Follow-up — BACKLOG.
- **JsonFormatter still allocates text_ as std::string once per log.** Could be `detail::SmallString<64>` to keep short texts inline. Marginal; deferred.

### Deferred to BACKLOG
- Test coverage for AddJsonTag inside a YaDeploy variant.
- Eliminate text_ staging via LogHelper dtor reorder.

## Acceptance: ✅ commit + advance
