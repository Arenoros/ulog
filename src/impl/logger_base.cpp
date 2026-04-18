#include <ulog/impl/logger_base.hpp>

namespace ulog::impl {

LoggerBase::~LoggerBase() = default;

formatters::BasePtr TextLoggerBase::MakeFormatter(Level /*level*/, std::string_view /*text*/) {
    // Concrete formatter implementations live in the formatters/ subtree and
    // are wired in during Phase 3. Until then, derived loggers that need
    // text output should override MakeFormatter themselves.
    return nullptr;
}

}  // namespace ulog::impl
