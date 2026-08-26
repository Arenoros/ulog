#include <atomic>
#include <cstdint>
#include <ulog/logger.hpp>

#include "logger_state.hpp"

namespace ulog {

namespace {

void DiscardText(void*, Level, const SourceLocation&, void*, detail::TextBuilder) {}

constinit const detail::ProducerOperations kNullProducerOperations{&DiscardText};
constinit detail::LoggerState kNullLoggerState{
    std::atomic<std::uint8_t>{static_cast<std::uint8_t>(Level::kNone)},
    &kNullProducerOperations,
    nullptr,
};
constinit std::atomic<const detail::LoggerState*> kDefaultLoggerState{&kNullLoggerState};

static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
              "Ulog requires lock-free byte atomics; select a supported target architecture.");
static_assert(std::atomic<const detail::LoggerState*>::is_always_lock_free,
              "Ulog requires lock-free pointer atomics; select a supported target architecture.");

[[nodiscard]] Level LoadLevel(const detail::LoggerState& state) noexcept {
  return static_cast<Level>(state.threshold.load(std::memory_order_relaxed));
}

}  // namespace

Logger::Logger() noexcept : state_(&kNullLoggerState) {}

Level Logger::GetLevel() const noexcept { return LoadLevel(*state_); }

bool Logger::ShouldLog(Level message_level) const noexcept {
  return IsLevelEnabled(message_level, LoadLevel(*state_));
}

void Logger::LogText(Level level, const SourceLocation& source, void* factory_context,
                     detail::TextBuilder build_text) const {
  const detail::LoggerState& state = *state_;
  if (!IsLevelEnabled(level, LoadLevel(state))) {
    return;
  }
  state.producer_operations->log_text(state.producer_context, level, source, factory_context,
                                      build_text);
}

Logger GetDefaultLogger() noexcept {
  return detail::LoggerAccess::FromState(kDefaultLoggerState.load(std::memory_order_acquire));
}

Logger GetNullLogger() noexcept { return detail::LoggerAccess::FromState(&kNullLoggerState); }

Logger ExchangeDefaultLogger(Logger stable_logger) noexcept {
  const detail::LoggerState* const previous = kDefaultLoggerState.exchange(
      detail::LoggerAccess::GetState(stable_logger), std::memory_order_acq_rel);
  return detail::LoggerAccess::FromState(previous);
}

}  // namespace ulog
