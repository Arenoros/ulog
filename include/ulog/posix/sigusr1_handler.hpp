#pragma once

/// @file ulog/posix/sigusr1_handler.hpp
/// @brief POSIX SIGUSR1 → logger reopen bridge.
///
/// On POSIX systems log-rotation tools (e.g. logrotate) signal the process
/// with SIGUSR1 after rotating the on-disk file. This header installs a
/// signal handler that forwards the event to one or more AsyncLogger
/// instances via RequestReopen().
///
/// This header compiles to a no-op on Windows.

#if !defined(_WIN32)

#include <memory>

#include <ulog/async_logger.hpp>

namespace ulog::posix {

/// Installs a SIGUSR1 handler that calls `logger->RequestReopen()` whenever
/// the signal is delivered. Safe to call multiple times — the most recently
/// registered logger wins. The logger is held by weak_ptr to avoid keeping
/// it alive past user scope.
void InstallSigUsr1ReopenHandler(const std::shared_ptr<AsyncLogger>& logger);

/// Removes the previously installed handler (restores SIG_DFL).
void UninstallSigUsr1ReopenHandler();

}  // namespace ulog::posix

#endif  // !_WIN32
