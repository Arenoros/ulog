#pragma once

/// @file ulog/null_logger.hpp
/// @brief Null logger (silently discards all records).

#include <ulog/fwd.hpp>
#include <ulog/impl/logger_base.hpp>

namespace ulog {

/// Returns a shared null logger that discards all records.
LoggerRef GetNullLogger() noexcept;

/// Creates a new null logger instance.
LoggerPtr MakeNullLogger();

}  // namespace ulog
