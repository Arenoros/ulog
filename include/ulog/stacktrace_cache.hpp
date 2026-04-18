#pragma once

/// @file ulog/stacktrace_cache.hpp
/// @brief Thread-safe cache of symbolized boost::stacktrace strings.
///
/// Stack unwinding is cheap; symbolization is expensive (minutes of PDB I/O
/// per new frame on Windows). The cache memoizes frame address vectors to
/// their symbolized representation so repeated captures pay resolution cost
/// only once.

#include <string>

#include <boost/stacktrace.hpp>

namespace ulog {

/// Returns the symbolized string for the given stacktrace, reusing a cached
/// result on subsequent calls with the same frame addresses.
std::string StacktraceToString(const boost::stacktrace::stacktrace& st);

/// Captures and symbolizes the current thread's stacktrace, caching the result.
std::string CurrentStacktrace();

/// Globally toggles stacktrace collection (defaults to enabled).
void EnableStacktrace(bool enabled) noexcept;

/// Returns the current global toggle state.
bool IsStacktraceEnabled() noexcept;

/// RAII scope guard for temporary toggling.
class StacktraceGuard final {
public:
    explicit StacktraceGuard(bool enabled) noexcept;
    ~StacktraceGuard();
    StacktraceGuard(const StacktraceGuard&) = delete;
    StacktraceGuard& operator=(const StacktraceGuard&) = delete;

private:
    bool previous_;
};

/// Drops every cached entry (releases memory).
void ClearStacktraceCache();

}  // namespace ulog
