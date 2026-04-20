#pragma once

/// @file ulog/async_logger.hpp
/// @brief Text logger that hands records off to a worker thread.

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ulog/format.hpp>
#include <ulog/impl/formatters/text_item.hpp>
#include <ulog/impl/logger_base.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sinks/structured_sink.hpp>

namespace ulog {

enum class OverflowBehavior {
    kDiscard,  ///< Drop incoming records when the queue is full; count them.
    kBlock,    ///< Block the producer until space is available.
};

/// Asynchronous text logger. A worker thread consumes records from a
/// lock-free queue (moodycamel::ConcurrentQueue) and forwards them to sinks.
///
/// The worker is launched on construction and joined on destruction after
/// draining the queue. Flush() waits for all currently enqueued records to
/// be written to every sink.
class AsyncLogger final : public impl::TextLoggerBase {
public:
    struct Config {
        Format format = Format::kTskv;
        std::size_t queue_capacity = 65536;
        OverflowBehavior overflow = OverflowBehavior::kDiscard;
        /// When false, the formatter drops the `module` field (function,
        /// file:line call-site). Useful for minimal log shapes where the
        /// caller attaches its own semantic tags instead of source
        /// locations.
        bool emit_location = true;
        /// Timestamp rendering style for TSKV/LTSV/JSON/JsonYaDeploy.
        /// `kOtlpJson` always emits `timeUnixNano` per OTLP spec.
        TimestampFormat timestamp_format = TimestampFormat::kIso8601Micro;
        /// Number of worker threads draining the record queue in parallel.
        /// Default 1 preserves the historical single-consumer semantics
        /// (sinks see records in queue-arrival order per worker).
        ///
        /// **Thread-safety contract when `worker_count > 1`:** every
        /// attached `BaseSink::Write` / `StructuredSink::Write` /
        /// `Flush` / `Reopen` may be invoked concurrently from distinct
        /// worker threads. Sinks that are not internally synchronised
        /// (e.g. a stream that is not atomic on write) must either
        /// serialise themselves or the caller should keep
        /// `worker_count = 1`.
        std::size_t worker_count = 1;
    };

    AsyncLogger();
    explicit AsyncLogger(const Config& cfg);
    ~AsyncLogger() override;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void AddSink(sinks::SinkPtr sink);

    /// Attach a sink with a per-sink format override. The worker renders
    /// one payload per distinct format and routes each sink to its
    /// matching payload.
    void AddSink(sinks::SinkPtr sink, Format format_override);

    /// Attach a structured sink — receives the raw record on the worker
    /// thread. Adding at least one enables the structured path in
    /// LogHelper (record accumulation) for every subsequent log call.
    void AddStructuredSink(sinks::StructuredSinkPtr sink);

    /// Requests the worker to reopen every sink (log-rotation entry point).
    void RequestReopen(sinks::ReopenMode mode = sinks::ReopenMode::kAppend);

    /// Number of records dropped due to overflow (kDiscard mode only).
    std::uint64_t GetDroppedCount() const noexcept;

    /// Records currently buffered in the queue (not yet written to sinks).
    std::size_t GetQueueDepth() const noexcept;

    /// Total records successfully handed to sinks since construction.
    std::uint64_t GetTotalLogged() const noexcept;

    // LoggerBase
    void Log(Level level, impl::LoggerItemPtr item) override;
    void LogMulti(Level level,
                  impl::LogItemList items,
                  std::unique_ptr<sinks::LogRecord> structured = nullptr) override;
    void LogStructured(Level level, std::unique_ptr<sinks::LogRecord> record) override;
    void Flush() override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace ulog
