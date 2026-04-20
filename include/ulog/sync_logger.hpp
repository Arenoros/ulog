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

    void Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) override;
    void LogMulti(Level level, impl::LogItemList items) override;
    void Flush() override;

private:
    struct SinkEntry {
        sinks::SinkPtr sink;
        std::size_t format_idx;  ///< Index into TextLoggerBase::GetActiveFormats().
    };

    std::mutex sinks_mu_;
    std::vector<SinkEntry> sinks_;
};

}  // namespace ulog
