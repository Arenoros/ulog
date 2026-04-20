#pragma once

/// @file ulog/impl/formatters/base.hpp
/// @brief Formatter interface and opaque log-record item base.

#include <cstdint>
#include <memory>
#include <string_view>

#include <fmt/format.h>

#include <ulog/json_string.hpp>

namespace ulog::impl::formatters {

/// Opaque payload returned by a formatter, handed to the logger.
struct LoggerItemBase {
    virtual ~LoggerItemBase() = default;
};
using LoggerItemRef = LoggerItemBase&;

/// Deleter that routes destruction through `TextLogItemPool::Release`
/// instead of `delete`. Declaration only — definition in
/// `text_item_pool.cpp` so `base.hpp` does not have to pull the pool
/// header. Concrete subclass of `LoggerItemBase` is assumed to be
/// `TextLogItem` (sole subtype today); if a second subtype is added,
/// the deleter needs to dispatch on dynamic type.
struct LoggerItemDeleter {
    void operator()(LoggerItemBase* p) const noexcept;
};

/// Owning handle used across the `Log` / queue / sink pipeline.
using LoggerItemPtr = std::unique_ptr<LoggerItemBase, LoggerItemDeleter>;

/// Interface every concrete formatter implements. A formatter is created per
/// log record, receives tags and text via AddTag/SetText, then releases a
/// finalized record payload via ExtractLoggerItem().
///
/// Ownership model: `ExtractLoggerItem()` moves the finalized payload out
/// of the formatter and hands it to the caller. Loggers that consume the
/// item synchronously (SyncLogger, MemLogger, NullLogger) destroy it at
/// the end of their `Log` scope. AsyncLogger transfers the unique_ptr
/// into its queue so the worker thread can dispatch it later — this lets
/// the producer-side LogHelper destroy the formatter immediately after
/// extraction without copying the payload.
class Base {
public:
    virtual ~Base() = default;

    /// Adds a key/value field. Value is a pre-stringified representation.
    virtual void AddTag(std::string_view key, std::string_view value) = 0;

    /// Adds a key/value field whose value is pre-serialized JSON.
    virtual void AddJsonTag(std::string_view key, const JsonString& value) = 0;

    /// Typed overloads. Text-based formatters inherit the stringifying
    /// defaults; structured formatters (JSON / OTLP) override to preserve
    /// the native type on the wire.
    virtual void AddTagInt64(std::string_view key, std::int64_t value) {
        AddTag(key, fmt::format("{}", value));
    }
    virtual void AddTagUInt64(std::string_view key, std::uint64_t value) {
        AddTag(key, fmt::format("{}", value));
    }
    virtual void AddTagDouble(std::string_view key, double value) {
        AddTag(key, fmt::format("{}", value));
    }
    virtual void AddTagBool(std::string_view key, bool value) {
        AddTag(key, value ? "true" : "false");
    }

    /// Attaches W3C trace-context identifiers to the current record. OTLP
    /// carries `traceId` / `spanId` as dedicated top-level fields on the
    /// LogRecord (not as attributes) — structured formatters override to
    /// honor that shape. Text formatters inherit the default, which
    /// round-trips the hex strings through `AddTag` so plain-text logs
    /// still carry the IDs as ordinary key=value pairs.
    ///
    /// Contract:
    ///  * Either argument may be empty — that half is skipped, the other
    ///    half is still applied.
    ///  * Expected to be called at most once per record. If called more
    ///    than once, structured formatters keep last-write-wins semantics
    ///    on each non-empty half; text formatters emit duplicate tags
    ///    (one `AddTag` per call). Callers should collapse the context
    ///    into a single invocation.
    virtual void SetTraceContext(std::string_view trace_id_hex,
                                 std::string_view span_id_hex) {
        if (!trace_id_hex.empty()) AddTag("trace_id", trace_id_hex);
        if (!span_id_hex.empty())  AddTag("span_id",  span_id_hex);
    }

    /// Sets the "text" field — the free-form message body.
    virtual void SetText(std::string_view text) = 0;

    /// Finalizes the record and yields ownership of the payload. The
    /// returned handle carries `LoggerItemDeleter`, which routes
    /// destruction back through `TextLogItemPool::Release` — steady
    /// state on the hot path performs no heap allocations. Subsequent
    /// calls return a handle whose internal pointer is null.
    virtual LoggerItemPtr ExtractLoggerItem() = 0;
};

/// Deleter that knows whether the Base instance was constructed inline
/// into caller-provided scratch (destroy-only) or on the heap (delete).
/// Enables `MakeFormatterInto` to place the formatter directly inside a
/// LogHelper's stack buffer, shedding the otherwise-mandatory heap alloc
/// on the logging hot path.
struct BaseDeleter {
    bool heap{true};
    void operator()(Base* p) const noexcept {
        if (!p) return;
        if (heap) { delete p; }
        else      { p->~Base(); }
    }
};

using BasePtr = std::unique_ptr<Base, BaseDeleter>;

}  // namespace ulog::impl::formatters
