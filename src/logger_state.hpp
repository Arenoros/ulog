#pragma once

#include <atomic>
#include <cstdint>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

namespace ulog::detail {

using LogText = void (*)(void*, Level, const SourceLocation&, void*, TextBuilder);

struct ProducerOperations final {
  LogText log_text;
};

struct LoggerState final {
  std::atomic<std::uint8_t> threshold;
  const ProducerOperations* producer_operations;
  void* producer_context;
};

struct LoggerAccess final {
  [[nodiscard]] static Logger FromState(const LoggerState* state) noexcept { return Logger{state}; }
};

}  // namespace ulog::detail
