#pragma once

#include <atomic>
#include <cstdint>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

namespace ulog::detail {

using LogMessage = void (*)(void*, Level, const SourceLocation&, void*, MessageBuilder);

using SetLoggerLevel = LevelChangeStatus (*)(void*, Level) noexcept;

/// Admits one Record for a scope writer and opens it for writing; false when nothing was admitted.
using BeginRecord = bool (*)(void*, Level, const SourceLocation&, OpenRecord&) noexcept;

struct ProducerOperations final {
  LogMessage log_message;
  SetLoggerLevel set_level;
  BeginRecord begin_record;
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
