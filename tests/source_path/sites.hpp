#pragma once

#include <string_view>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

namespace ulog::test {

// What one translation unit reports about a call site it logged from.
struct SourcePathSite final {
  SourceLocation identity;    // ULOG_SOURCE_LOCATION() in the logging function
  std::string_view raw_path;  // the untrimmed compiler path of the same translation unit
};

// Each logs one Info Record to `logger` from its own translation unit.
SourcePathSite LogFromFirst(Logger logger);
SourcePathSite LogFromSecond(Logger logger);
SourcePathSite LogFromFirstExtra(Logger logger);

}  // namespace ulog::test
