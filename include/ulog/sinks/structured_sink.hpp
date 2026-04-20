#pragma once

/// @file ulog/sinks/structured_sink.hpp
/// @brief Sink interface that receives the raw record (level / text / tags)
/// instead of a pre-formatted text payload.
///
/// A `StructuredSink` avoids the parse-the-string round-trip for destinations
/// that consume a native-structured format on the far side — OTLP protobuf,
/// native logging APIs (Stackdriver, CloudWatch Logs), fluent-bit native
/// in-process input, etc. Each sink renders the record itself, so typed tag
/// values (ints, bools, doubles, JSON literals) reach the destination
/// without traversing a string form in between.
///
/// Relative to the existing `BaseSink` path: structured sinks pay the cost
/// of copying every tag into an owned vector per record; text sinks keep
/// their streaming-into-formatter-buffer fast path. Mixed loggers run both
/// paths — the extra cost is paid only when a structured sink is attached.

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <ulog/json_string.hpp>
#include <ulog/level.hpp>
#include <ulog/sinks/base_sink.hpp>

namespace ulog::sinks {

/// Native-typed tag value. Variant preserves the on-wire shape the
/// application supplied — a structured sink can route an int to a
/// protobuf int64 field without re-parsing a stringified form.
///
/// `JsonString` is carried as-is for cases where the caller already
/// has a serialized JSON value (object / array) and wants it embedded
/// verbatim in the downstream format.
using TagValue = std::variant<
    std::string,
    std::int64_t,
    std::uint64_t,
    double,
    bool,
    JsonString
>;

struct Tag {
    std::string key;
    TagValue value;
};

/// Structured log record — all strings are owned so the sink may read
/// through them for the duration of its `Write` call without any
/// lifetime surprises. `tags` are in emission order: streamed tags, then
/// tracing-hook contributions, then `RecordEnricher` contributions.
struct LogRecord {
    Level level{Level::kInfo};
    std::chrono::system_clock::time_point timestamp{};
    /// Call-site function + file/line. Empty when the logger was created
    /// with `emit_location = false`; otherwise mirrors what the text
    /// formatters emit in the `module` field.
    std::string module_function;
    std::string module_file;
    int module_line{0};
    /// The free-form message body — everything the caller streamed via
    /// `operator<<` after the LOG_* macro.
    std::string text;
    std::vector<Tag> tags;
    /// Populated only when a tracing hook emitted a trace context. Empty
    /// strings indicate "unset".
    std::string trace_id;
    std::string span_id;
};

/// Sink interface that consumes a `LogRecord` rather than a pre-rendered
/// byte string. Implementations decide their own wire format.
class StructuredSink {
public:
    virtual ~StructuredSink() = default;

    /// Called synchronously for SyncLogger sinks, on the worker thread
    /// for AsyncLogger sinks. The record's storage lives only until
    /// this call returns — copy out anything that must outlive the
    /// write.
    virtual void Write(const LogRecord& record) = 0;

    /// Flushes any buffered state (default: no-op).
    virtual void Flush() {}

    /// Reopens the underlying resource (log rotation). Default: no-op.
    virtual void Reopen(ReopenMode /*mode*/) {}

    /// Per-sink level gate. Mirrors `BaseSink::SetLevel` / `ShouldLog`.
    void SetLevel(Level level) noexcept { level_.store(level, std::memory_order_relaxed); }
    Level GetLevel() const noexcept { return level_.load(std::memory_order_relaxed); }
    bool ShouldLog(Level msg_level) const noexcept {
        const auto cur = GetLevel();
        if (cur == Level::kNone) return true;
        return static_cast<int>(msg_level) >= static_cast<int>(cur);
    }

protected:
    StructuredSink() = default;

private:
    std::atomic<Level> level_{Level::kNone};
};

using StructuredSinkPtr = std::shared_ptr<StructuredSink>;

}  // namespace ulog::sinks
