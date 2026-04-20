#pragma once

/// @file ulog/sync_logger.hpp
/// @brief Text logger that writes synchronously to a set of sinks.

#include <mutex>
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

    void AddSink(sinks::SinkPtr sink);

    void Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) override;
    void Flush() override;

private:
    std::mutex sinks_mu_;
    std::vector<sinks::SinkPtr> sinks_;
};

}  // namespace ulog
