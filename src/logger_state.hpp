#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

namespace ulog::detail {

using LogMessage = void (*)(void*, Level, const SourceLocation&, void*, MessageBuilder);

/// Like LogMessage, but waits for admission capacity until the deadline (Logger::LogUntil).
using LogMessageUntil = void (*)(void*, Level, std::chrono::steady_clock::time_point,
                                 const SourceLocation&, void*, MessageBuilder);

using SetLoggerLevel = LevelChangeStatus (*)(void*, Level) noexcept;

/// Admits one Record for a scope writer and opens it for writing; false when nothing was admitted.
using BeginRecord = bool (*)(void*, Level, const SourceLocation&, OpenRecord&) noexcept;

struct ProducerOperations final {
  LogMessage log_message;
  SetLoggerLevel set_level;
  BeginRecord begin_record;
  /// Null when the target never admits, as for the Null Logger.
  LogMessageUntil log_message_until{nullptr};
};

struct LoggerState final {
  std::atomic<std::uint8_t> threshold;
  const ProducerOperations* producer_operations;
  void* producer_context;
};

struct LoggerAccess final {
  [[nodiscard]] static Logger FromState(const LoggerState* state) noexcept { return Logger{state}; }
  [[nodiscard]] static const LoggerState* GetState(Logger logger) noexcept { return logger.state_; }
};

}  // namespace ulog::detail
