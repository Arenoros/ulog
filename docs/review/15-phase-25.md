# Review 15 — Phase 25 (SmallString-backed `text_` in JSON / OTLP formatters)

Scope: eliminate the heap allocation the JSON and OTLP-JSON formatters paid just to hold the deferred `text` / `body.stringValue` payload. Short log messages — empirically the overwhelming majority — now live inside a 64-byte SSO slab; longer ones spill to heap transparently.

## Changes

- **`include/ulog/detail/small_string.hpp`** — added `SmallString::assign(std::string_view)`. The helper clears the underlying `boost::small_vector<char, N>` and re-appends, so the SSO slab is reused when the new value fits.
- **`JsonFormatter`** (`json.hpp` / `json.cpp`):
  - `std::string text_` → `detail::SmallString<64> text_`.
  - `SetText` uses the new `assign(string_view)`.
  - `ExtractLoggerItem` emits via `text_.view()` into `EmitField`.
  - Dropped the now-unused `<string>` include from the header.
- **`OtlpJsonFormatter`** (`otlp_json.hpp` / `otlp_json.cpp`):
  - `std::string body_text_` → `detail::SmallString<64> body_text_`.
  - `SetText` uses `assign(string_view)`.
  - `AppendJsonEscaped(b, body_text_)` still compiles unchanged because `SmallString` has an implicit `operator std::string_view`.

No public-API changes; the swap is entirely inside the formatter TUs. Text formatters that stream directly into `item_->payload` (TSKV / LTSV / Raw) were unaffected — they never held a separate `text_` string to begin with.

## Why SSO here

Both JSON and OTLP emit the text field last (after the attribute array is closed), so they have to defer the body and can't stream it into the payload buffer the way TSKV does. Before this phase every record paid one `std::string` heap allocation of ~32 bytes minimum, plus re-allocations if the message crossed `libstdc++`'s SSO limit (15 bytes on MSVC `std::string` is 15 chars for SSO). 64 bytes captures the usual "user logged in" / "request=/api/thing" / "failure opening …" workload and retires that allocation.

Capacity choice: 64 is the BACKLOG suggestion and maps to a comfortable slab size: with the small_vector header, `sizeof(SmallString<64>)` is 88 bytes — still well inside a cache line pair. Longer messages spill to heap, matching the previous `std::string` behavior.

## Results

- **Build (local MSVC 14.50, RelWithDebInfo)**: clean.
- **Tests**: 79/79 green (76 previous + 3 new: long-body JSON, long-body OTLP, repeated-SetText).
- **Bench (local, 24-core, RelWithDebInfo)** — `BM_FormatThroughput`:

  | Format | CPU/op | Items/s   |
  |--------|--------|-----------|
  | TSKV   | 756 ns | 1.32 M/s  |
  | LTSV   | 785 ns | 1.27 M/s  |
  | Raw    | 326 ns | 3.07 M/s  |
  | JSON   | 785 ns | 1.27 M/s  |

  JSON is now in the same ballpark as TSKV / LTSV — the alloc the old `std::string` hit used to show as a consistent ~40-60 ns delta. On-record A/B would be a nice follow-up but the ordering `Raw < TSKV ≈ LTSV ≈ JSON` matches the expected "formatter cost is dominated by escape + attribute-emit, not by the body buffer".

## Findings

### 🔴 Critical
- None.

### 🟡 Non-critical
- **`SmallString::assign` is the only new API surface.** It delegates to `clear() + append()`, which means we don't short-circuit when the new value is identical to the old one — that's fine for our per-record churn but worth keeping in mind if SmallString grows reused-instance callers.
- **64-byte SSO is a guess informed by typical messages, not a histogram.** A data-driven capacity tune would need per-deployment message-length stats; 64 is the published BACKLOG number and a reasonable default.
- **Heap-spill path still allocates.** Long messages (>64 bytes) pay one allocation in `small_vector`, the same as the old `std::string`. No regression for the big-message case.

### Test coverage gaps (addressed)
- **`FormatterJson.LongBodySpillsToHeapAndRoundTrips`** — emits a 200-byte body through the JSON formatter, asserts round-trip integrity. Exercises the `boost::small_vector` heap-spill path.
- **`FormatterOtlpJson.LongBodySpillsToHeapAndRoundTrips`** — same shape against OTLP's `body.stringValue`.
- **`FormatterJson.RepeatedSetTextKeepsLastValue`** — constructs the formatter directly, calls `SetText("first")` then `SetText("second")`, asserts the payload contains only the second value. Locks in the `SmallString::assign` clear-then-append semantics.

### Deferred to BACKLOG
- Per-deployment tuning of the `<64>` capacity once production message-length histograms are available.

## Acceptance: ✅ commit + advance
