#include <ulog/log.hpp>

#include "../sites.hpp"

namespace ulog::test {

SourcePathSite LogFromFirstExtra(Logger logger) {
  LOG_INFO_TO(logger, "first_extra");
  return {ULOG_SOURCE_LOCATION(), SourceLocation::Current().GetFileName()};
}

}  // namespace ulog::test
