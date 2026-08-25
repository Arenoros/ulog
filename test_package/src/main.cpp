#include <cstddef>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>
#include <ulog/version.hpp>

std::string_view MissingPackagedMessageDefinition();

int main() {
  const ulog::Logger logger = ulog::GetDefaultLogger();
  const ulog::SourceLocation location = ulog::SourceLocation::Current();
  std::size_t message_evaluations = 0;

  logger.Log<ulog::Level::kInfo>(location, [] { return MissingPackagedMessageDefinition(); });
  logger.Log<ulog::Level::kCritical>(location, [&]() -> std::string_view {
    ++message_evaluations;
    return "must not be evaluated";
  });

  const bool valid = ulog::GetVersion() == ulog::kVersion && logger == ulog::GetNullLogger() &&
                     logger.GetLevel() == ulog::Level::kNone && message_evaluations == 0 &&
                     location.GetLine() != 0;
  return valid ? 0 : 1;
}
