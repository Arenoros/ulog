#pragma once

/// @file ulog/sync_logger.hpp
/// @brief Text logger that writes synchronously to a set of sinks.

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <ulog/format.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/impl/logger_base.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sinks/structured_sink.hpp>

namespace ulog {

/// Simple synchronous text logger. Dispatches every record to all attached
/// sinks on the calling thread. Thread-safe.
class SyncLogger final : public impl::TextLoggerBase {
public:
    explicit SyncLogger(Format format = Format::kTskv,
                        bool emit_location = true,
                        TimestampFormat ts_fmt = TimestampFormat::kIso8601Micro)
        : impl::TextLoggerBase(format, emit_location, ts_fmt) {}

    /// Attach a sink using the logger's base format.
    void AddSink(sinks::SinkPtr sink);

    /// Attach a sink with a per-sink format override. The logger materializes
    /// an extra formatter per distinct override so each sink receives a
    /// payload rendered in its chosen format.
    void AddSink(sinks::SinkPtr sink, Format format_override);

    /// Attach a structured sink — receives the raw record instead of a
    /// formatted string. Adding at least one enables the structured
    /// path in LogHelper (record accumulation), so text-only loggers
    /// pay nothing until a structured sink is attached.
    void AddStructuredSink(sinks::StructuredSinkPtr sink);

    void Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) override;
    void LogMulti(Level level,
                  impl::LogItemList items,
                  std::unique_ptr<sinks::LogRecord> structured = nullptr) override;
    void LogStructured(Level level, std::unique_ptr<sinks::LogRecord> record) override;
    void Flush() override;

    bool HasTextSinks() const noexcept override;
    bool HasStructuredSinks() const noexcept override;

private:
    struct SinkEntry {
        sinks::SinkPtr sink;
        std::size_t format_idx;  ///< Index into TextLoggerBase::GetActiveFormats().
    };

    // Mutable so the const `Has*Sinks()` queries can lock — the state
    // they peek at is shared across threads and must be read under lock.
    mutable std::mutex sinks_mu_;
    std::vector<SinkEntry> sinks_;
    mutable std::mutex struct_sinks_mu_;
    std::vector<sinks::StructuredSinkPtr> struct_sinks_;
};

}  // namespace ulog
