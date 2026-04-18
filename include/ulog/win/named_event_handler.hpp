#pragma once

/// @file ulog/win/named_event_handler.hpp
/// @brief Windows named-event → AsyncLogger reopen bridge.
///
/// Windows services cannot receive SIGUSR1 the way POSIX processes do.
/// This helper installs a watcher thread that waits on a named Win32 Event
/// object (`CreateEventA("Local\\ulog-reopen")` by default) and forwards
/// signal-to-reopen semantics to one `AsyncLogger`.
///
/// External log-rotation tooling signals the service by running
/// `powershell "(New-Object -ComObject ...)"` or the equivalent native
/// `OpenEventA + SetEvent` from an admin-side script.
///
/// This header compiles to a no-op outside Windows.

#if defined(_WIN32)

#include <memory>
#include <string>

#include <ulog/async_logger.hpp>

namespace ulog::win {

/// Installs a named-event watcher for `logger->RequestReopen()`. Calling a
/// second time replaces the current watcher (the prior one is stopped
/// before the new one starts). `event_name` defaults to
/// `"Local\\ulog-reopen"`; pass a fully-qualified `"Global\\..."` name to
/// cross session boundaries.
void InstallNamedEventReopenHandler(const std::shared_ptr<AsyncLogger>& logger,
                                    const std::string& event_name = "Local\\ulog-reopen");

/// Stops the watcher thread (if any). Safe to call multiple times.
void UninstallNamedEventReopenHandler();

}  // namespace ulog::win

#endif  // _WIN32
