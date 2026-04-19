#pragma once

/// @file ulog/tracing_hook.hpp
/// @brief Public hook for injecting span / trace tags into log records.
///
/// ulog ships with no built-in tracing. Applications that want to attach
/// their trace/span IDs (userver tracing::Span, opentelemetry, any
/// TLS-backed context) register a callback that is invoked per record,
/// after the built-in header but before the text. The callback adds tags
/// via TagSink.

#include <string_view>

#include <ulog/json_string.hpp>

namespace ulog {

/// Abstraction over the active formatter's AddTag methods — passed to
/// tracing callbacks so they cannot accidentally affect other formatter state.
class TagSink {
public:
    virtual ~TagSink() = default;
    virtual void AddTag(std::string_view key, std::string_view value) = 0;
    virtual void AddJsonTag(std::string_view key, const JsonString& value) = 0;

    /// Dedicated path for W3C trace-context IDs. Structured formatters
    /// (OTLP) promote them to top-level `traceId` / `spanId`; plain-text
    /// formatters fall back to emitting `trace_id=` / `span_id=` tags.
    /// Prefer this over two `AddTag` calls with the magic keys.
    ///
    /// Contract mirrors `formatters::Base::SetTraceContext`: empty halves
    /// are skipped, call once per record (multiple calls yield duplicate
    /// tags on text formatters; OTLP last-write-wins per non-empty half).
    virtual void SetTraceContext(std::string_view trace_id_hex,
                                 std::string_view span_id_hex) {
        if (!trace_id_hex.empty()) AddTag("trace_id", trace_id_hex);
        if (!span_id_hex.empty())  AddTag("span_id",  span_id_hex);
    }
};

/// Signature of a tracing-context callback.
///
/// `user_ctx` is the opaque pointer passed to SetTracingHook.
/// `sink` lives for the duration of the call; do not retain it.
using TracingHook = void (*)(TagSink& sink, void* user_ctx);

/// Registers a callback invoked for every emitted record. Passing `nullptr`
/// disables the hook. Thread-safe. Only one hook is active at a time.
void SetTracingHook(TracingHook hook, void* user_ctx = nullptr) noexcept;

namespace impl {
/// Invoked internally by LogHelper.
void DispatchTracingHook(TagSink& sink);
}  // namespace impl

}  // namespace ulog
