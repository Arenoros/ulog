#include <ulog/log.hpp>

#include "../sites.hpp"

namespace ulog::test {

SourcePathSite LogFromSecond(Logger logger) {
  LOG_TO(logger, Level::kInfo, "second");
  return {ULOG_SOURCE_LOCATION(), SourceLocation::Current().GetFileName()};
}

}  // namespace ulog::test
