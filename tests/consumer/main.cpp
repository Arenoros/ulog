#include <chrono>
#include <cstddef>
#include <string_view>
#include <type_traits>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/source_location.hpp>
#include <ulog/version.hpp>

ulog::Logger LoadInstalledDefaultLogger() noexcept;
ulog::Logger MissingInstalledLoggerDefinition() noexcept;
std::string_view MissingInstalledMessageDefinition();

static_assert(!std::is_copy_constructible_v<ulog::Operation>);
static_assert(!std::is_copy_assignable_v<ulog::Operation>);
static_assert(std::is_nothrow_move_constructible_v<ulog::Operation>);
static_assert(std::is_nothrow_move_assignable_v<ulog::Operation>);

int main() {
  const ulog::Version version = ulog::GetVersion();
  const ulog::Logger logger = ulog::GetDefaultLogger();
  const ulog::Logger previous_default = ulog::ExchangeDefaultLogger(logger);
  const ulog::SourceLocation location = ulog::SourceLocation::Current();
  std::size_t erased_target_evaluations = 0;
  std::size_t erased_message_evaluations = 0;
  std::size_t critical_message_evaluations = 0;
  std::size_t generic_target_evaluations = 0;
  std::size_t generic_level_evaluations = 0;
  std::size_t generic_message_evaluations = 0;
  ulog::Operation empty_operation;
  const auto empty_poll = empty_operation.Poll();
  const auto empty_wait = empty_operation.WaitUntil(std::chrono::steady_clock::now());
  const auto empty_callback =
      empty_operation.OnComplete([](const ulog::OperationResult&) noexcept {});
  const ulog::OperationStartFailure exhausted{
      ulog::OperationStartErrorCode::kControlReserveExhausted, 1, 1};

  LOG_INFO((++erased_message_evaluations, MissingInstalledMessageDefinition()));
  LOG_INFO_TO((++erased_target_evaluations, MissingInstalledLoggerDefinition()),
              (++erased_message_evaluations, MissingInstalledMessageDefinition()));
  LOG_CRITICAL("critical operand={}", ([&] {
                 ++critical_message_evaluations;
                 return 42;
               })());
  LOG_TO((++generic_target_evaluations, logger), (++generic_level_evaluations, ulog::Level::kInfo),
         "generic operand={}", ([&] {
           ++generic_message_evaluations;
           return 17;
         })());

  if (version != ulog::kVersion) return 1;
  if (logger != ulog::GetNullLogger()) return 2;
  if (previous_default != logger) return 3;
  if (ulog::GetDefaultLogger() != logger) return 4;
  if (logger != LoadInstalledDefaultLogger()) return 5;
  if (logger.GetLevel() != ulog::Level::kNone) return 6;
  if (logger.ShouldLog(ulog::Level::kCritical)) return 7;
  if (location.GetLine() == 0) return 8;
  if (location.GetFileName().empty()) return 9;
  if (erased_target_evaluations != 0) return 10;
  if (erased_message_evaluations != 0) return 11;
  if (critical_message_evaluations != 0) return 12;
  if (generic_target_evaluations != 1) return 13;
  if (generic_level_evaluations != 1) return 14;
  if (generic_message_evaluations != 0) return 15;
  if (empty_poll.status != ulog::OperationPollStatus::kInvalidOperation) return 16;
  if (empty_wait.status != ulog::OperationWaitStatus::kInvalidOperation) return 17;
  if (empty_wait.Message().empty() || empty_wait.HowToFix().empty()) return 18;
  if (empty_callback.status != ulog::OperationCallbackStatus::kInvalidOperation) return 19;
  if (exhausted.Message().empty() || exhausted.HowToFix().empty()) return 20;
  return 0;
}
