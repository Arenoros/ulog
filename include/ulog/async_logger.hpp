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
    };

    AsyncLogger();
    explicit AsyncLogger(const Config& cfg);
    ~AsyncLogger() override;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

    void AddSink(sinks::SinkPtr sink);

    /// Requests the worker to reopen every sink (log-rotation entry point).
    void RequestReopen(sinks::ReopenMode mode = sinks::ReopenMode::kAppend);

    /// Number of records dropped due to overflow (kDiscard mode only).
    std::uint64_t GetDroppedCount() const noexcept;

    /// Records currently buffered in the queue (not yet written to sinks).
    std::size_t GetQueueDepth() const noexcept;

    /// Total records successfully handed to sinks since construction.
    std::uint64_t GetTotalLogged() const noexcept;

    // LoggerBase
    void Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) override;
    void Flush() override;

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace ulog
