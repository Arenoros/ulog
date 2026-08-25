#include <ulog/logger.hpp>

namespace ulog::test {

Logger LoadDefaultLoggerFromAnotherTranslationUnit() noexcept { return GetDefaultLogger(); }

}  // namespace ulog::test
