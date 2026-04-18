#pragma once

/// @file ulog/fwd.hpp
/// @brief Forward declarations for logging types.

#include <memory>

namespace ulog {

namespace impl {
class LoggerBase;
class TagWriter;
}  // namespace impl

class TextLogger;
class LogExtra;

/// Reference-like type to a logger. Never null.
using LoggerRef = impl::LoggerBase&;

/// Shared ownership of a logger.
using LoggerPtr = std::shared_ptr<impl::LoggerBase>;

/// Shared ownership of a text logger.
using TextLoggerPtr = std::shared_ptr<TextLogger>;

}  // namespace ulog
