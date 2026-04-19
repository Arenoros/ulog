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

    /// Finalizes the record and yields ownership of the payload.
    /// Subsequent calls return nullptr.
    virtual std::unique_ptr<LoggerItemBase> ExtractLoggerItem() = 0;
};

using BasePtr = std::unique_ptr<Base>;

}  // namespace ulog::impl::formatters
