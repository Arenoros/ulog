#include <ulog/log.hpp>

#include "../sites.hpp"

namespace ulog::test {

SourcePathSite LogFromFirst(Logger logger) {
  LOG_INFO_TO(logger, "first");
  return {ULOG_SOURCE_LOCATION(), SourceLocation::Current().GetFileName()};
}

}  // namespace ulog::test
