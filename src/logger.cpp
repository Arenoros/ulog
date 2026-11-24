#include <atomic>
#include <cstdint>
#include <ulog/logger.hpp>

#include "logger_state.hpp"

namespace ulog {

namespace {

void DiscardMessage(void*, Level, const SourceLocation&, void*, detail::MessageBuilder) {}

LevelChangeStatus RejectLevelChange(void*, Level) noexcept {
  return LevelChangeStatus::kNullLogger;
}

bool AdmitNothing(void*, Level, const SourceLocation&, detail::OpenRecord&) noexcept {
  return false;
}

constinit const detail::ProducerOperations kNullProducerOperations{
    &DiscardMessage, &RejectLevelChange, &AdmitNothing};
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

void Logger::LogMessage(Level level, const SourceLocation& source, void* builder_context,
                        detail::MessageBuilder build_message) const {
  const detail::LoggerState& state = *state_;
  if (!IsLevelEnabled(level, LoadLevel(state))) {
    return;
  }
  state.producer_operations->log_message(state.producer_context, level, source, builder_context,
                                         build_message);
}

detail::OpenRecord Logger::BeginRecord(Level level, const SourceLocation& source) const noexcept {
  const detail::LoggerState& state = *state_;
  detail::OpenRecord record;
  if (IsLevelEnabled(level, LoadLevel(state)) &&
      state.producer_operations->begin_record != nullptr &&
      state.producer_operations->begin_record(state.producer_context, level, source, record)) {
    return record;
  }
  return {};
}

LevelChangeStatus Logger::SetLevel(Level threshold) const noexcept {
  if (static_cast<std::uint8_t>(threshold) > static_cast<std::uint8_t>(Level::kNone)) {
    return LevelChangeStatus::kInvalidLevel;
  }
  return state_->producer_operations->set_level(state_->producer_context, threshold);
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
