#include <cstddef>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>
#include <ulog/version.hpp>

ulog::Logger LoadInstalledDefaultLogger() noexcept;
std::string_view MissingInstalledMessageDefinition();

int main() {
  const ulog::Version version = ulog::GetVersion();
  const ulog::Logger logger = ulog::GetDefaultLogger();
  const ulog::SourceLocation location = ulog::SourceLocation::Current();
  std::size_t message_evaluations = 0;

  logger.Log<ulog::Level::kInfo>(location, [] { return MissingInstalledMessageDefinition(); });
  logger.Log<ulog::Level::kCritical>(location, [&]() -> std::string_view {
    ++message_evaluations;
    return "must not be evaluated";
  });

  const bool valid = version == ulog::kVersion && logger == ulog::GetNullLogger() &&
                     logger == LoadInstalledDefaultLogger() &&
                     logger.GetLevel() == ulog::Level::kNone &&
                     !logger.ShouldLog(ulog::Level::kCritical) && message_evaluations == 0 &&
                     location.GetLine() != 0 && !location.GetFileName().empty();
  return valid ? 0 : 1;
}
