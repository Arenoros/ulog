#include <cstddef>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>
#include <ulog/version.hpp>

ulog::Logger MissingPackagedLoggerDefinition() noexcept;
std::string_view MissingPackagedMessageDefinition();

int main() {
  const ulog::Logger logger = ulog::GetDefaultLogger();
  const ulog::Logger previous_default = ulog::ExchangeDefaultLogger(logger);
  const ulog::SourceLocation location = ulog::SourceLocation::Current();
  std::size_t erased_target_evaluations = 0;
  std::size_t erased_message_evaluations = 0;
  std::size_t critical_message_evaluations = 0;
  std::size_t generic_target_evaluations = 0;
  std::size_t generic_level_evaluations = 0;
  std::size_t generic_message_evaluations = 0;

  LOG_INFO((++erased_message_evaluations, MissingPackagedMessageDefinition()));
  LOG_INFO_TO((++erased_target_evaluations, MissingPackagedLoggerDefinition()),
              (++erased_message_evaluations, MissingPackagedMessageDefinition()));
  LOG_CRITICAL("critical operand={}", ([&] {
                 ++critical_message_evaluations;
                 return 42;
               })());
  LOG_TO((++generic_target_evaluations, logger), (++generic_level_evaluations, ulog::Level::kInfo),
         "generic operand={}", ([&] {
           ++generic_message_evaluations;
           return 17;
         })());

  if (ulog::GetVersion() != ulog::kVersion) return 1;
  if (logger != ulog::GetNullLogger()) return 2;
  if (previous_default != logger) return 3;
  if (ulog::GetDefaultLogger() != logger) return 4;
  if (logger.GetLevel() != ulog::Level::kNone) return 5;
  if (location.GetLine() == 0) return 6;
  if (erased_target_evaluations != 0) return 7;
  if (erased_message_evaluations != 0) return 8;
  if (critical_message_evaluations != 0) return 9;
  if (generic_target_evaluations != 1) return 10;
  if (generic_level_evaluations != 1) return 11;
  if (generic_message_evaluations != 0) return 12;
  return 0;
}
