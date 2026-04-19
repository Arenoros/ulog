#pragma once

/// @file ulog/impl/formatters/base.hpp
/// @brief Formatter interface and opaque log-record item base.

#include <memory>
#include <string_view>

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

    /// Sets the "text" field — the free-form message body.
    virtual void SetText(std::string_view text) = 0;

    /// Finalizes the record and yields ownership of the payload.
    /// Subsequent calls return nullptr.
    virtual std::unique_ptr<LoggerItemBase> ExtractLoggerItem() = 0;
};

using BasePtr = std::unique_ptr<Base>;

}  // namespace ulog::impl::formatters
