#pragma once

/// @file ulog/config.hpp
/// @brief LoggerConfig + factory functions for quick initialization.

#include <cstdint>
#include <memory>
#include <string>

#include <ulog/async_logger.hpp>
#include <ulog/format.hpp>
#include <ulog/fwd.hpp>
#include <ulog/level.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/sync_logger.hpp>

namespace ulog {

/// Describes a logger to build.
///
/// `file_path` is interpreted as:
/// - `@stdout`, `@stderr`       — FD sink to the corresponding stream
/// - `@null`                    — NullSink (discard)
/// - `tcp:host:port`            — TcpSocketSink
/// - `unix:/path/to/sock`       — UnixSocketSink (POSIX; Windows with ULOG_HAVE_AFUNIX)
/// - anything else              — FileSink at that path
struct LoggerConfig {
    std::string file_path = "@stderr";
    Format format = Format::kTskv;
    Level level = Level::kInfo;
    Level flush_level = Level::kWarning;
    bool truncate_on_start = false;

    /// When false, the `module` field (call-site function + file:line) is
    /// suppressed from every emitted record. The LOG_* macros still
    /// capture the location — it is simply not rendered. Default true
    /// preserves existing behaviour.
    bool emit_location = true;

    /// Only consulted by `MakeAsyncLogger`.
    std::size_t queue_capacity = 65536;
    OverflowBehavior overflow = OverflowBehavior::kDiscard;
};

/// Builds a sink from a `file_path` spec (see LoggerConfig above).
sinks::SinkPtr MakeSinkFromSpec(const std::string& spec, bool truncate_on_start = false);

/// Synchronous logger configured from the spec.
std::shared_ptr<SyncLogger> MakeSyncLogger(const LoggerConfig& cfg);

/// Asynchronous logger configured from the spec.
std::shared_ptr<AsyncLogger> MakeAsyncLogger(const LoggerConfig& cfg);

/// Shortcut: `MakeAsyncLogger(cfg)` + `SetDefaultLogger(...)`.
std::shared_ptr<AsyncLogger> InitDefaultLogger(const LoggerConfig& cfg);

}  // namespace ulog
