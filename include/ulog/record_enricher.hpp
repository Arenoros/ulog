#pragma once

/// @file ulog/record_enricher.hpp
/// @brief Pre-emit hooks that attach tags to every log record.
///
/// Enrichers run on the producing thread for every emitted record, after
/// the tracing hook and before the user text. Typical use: auto-emit
/// thread id, thread name, hostname, process id, or any ambient context
/// the application wants on every log line without wiring it into each
/// LOG_* site.
///
/// Multiple enrichers can be registered; they fire in registration order.
/// Registration returns a token that can be used to deregister a specific
/// enricher (e.g. to unregister a library-installed one on shutdown).

#include <cstdint>
#include <string_view>

#include <ulog/tracing_hook.hpp>

namespace ulog {

/// Callback signature. Declared noexcept — enrichers run inside the
/// LogHelper destructor which already swallows exceptions, but crossing
/// the boundary at all is a code smell. Keep the body trivial.
using RecordEnricher = void (*)(TagSink& sink, void* user_ctx) noexcept;

/// Opaque handle returned by `AddRecordEnricher`. Pass back to
/// `RemoveRecordEnricher` to deregister. Handles are unique for the
/// lifetime of the process; a removed handle stays invalid even if the
/// same address is later reused by a fresh enricher.
using RecordEnricherHandle = std::uint64_t;

/// Registers an enricher. Thread-safe. Returns a handle that can be used
/// to deregister via `RemoveRecordEnricher`. The hook fires on every
/// subsequent log record until deregistered.
///
/// Cost model: the LOG_* hot path loads an atomic flag; when the flag is
/// unset (no enrichers registered) the dispatch cost is one relaxed load
/// per record. When enrichers ARE present the hot path acquires a
/// shared_ptr snapshot of the registry and walks it — O(N) calls where
/// N is usually 1–3.
RecordEnricherHandle AddRecordEnricher(RecordEnricher hook, void* user_ctx = nullptr);

/// Deregisters the enricher previously installed under `handle`. Safe to
/// call from inside an enricher callback. No-op if the handle is stale.
void RemoveRecordEnricher(RecordEnricherHandle handle) noexcept;

/// Removes every registered enricher.
void ClearRecordEnrichers() noexcept;

/// Convenience: installs an enricher that adds the current thread id to
/// every record under the given key. Returns the handle so callers can
/// deregister later if needed.
///
/// Thread id format: decimal std::hash<std::thread::id> value, stable
/// within one process. Use this instead of `std::thread::id`'s stream
/// operator which is formatted differently on each stdlib.
RecordEnricherHandle EnableThreadIdEnricher(std::string_view key = "tid");

/// Convenience: installs an enricher that adds the OS-assigned thread
/// name (Linux: pthread_getname_np; Windows: GetThreadDescription) to
/// every record. Empty when the thread was never named — the tag is
/// omitted in that case.
RecordEnricherHandle EnableThreadNameEnricher(std::string_view key = "thread_name");

namespace impl {
/// Invoked internally by LogHelper after the tracing hook. Walks the
/// registered enricher list. No-op on the fast path when the list is
/// empty.
void DispatchRecordEnrichers(TagSink& sink);
}  // namespace impl

}  // namespace ulog
