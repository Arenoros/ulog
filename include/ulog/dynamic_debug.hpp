#pragma once

/// @file ulog/dynamic_debug.hpp
/// @brief Per-source-line runtime toggle for log sites.
///
/// Each `LOG_*` macro expansion registers a StaticLogEntry tagged with the
/// source file and line. At runtime the application may force-enable or
/// force-disable logging for specific (file, line) pairs regardless of the
/// logger's level setting. Useful for debugging production without raising
/// the global log level.

#include <cstdint>
#include <functional>
#include <string_view>

namespace ulog {

enum class DynamicDebugState {
    kDefault,         ///< Use logger level as-is
    kForceEnabled,    ///< Always emit regardless of logger level
    kForceDisabled,   ///< Never emit regardless of logger level
};

/// Sets the runtime state for a (file, line) pair. `line == 0` => applies to
/// every line in the file. Matches are resolved by suffix: a configured
/// `/foo/bar.cpp` matches any source whose path ends with `bar.cpp`.
void SetDynamicDebugLog(std::string_view file, int line, DynamicDebugState state);

/// Convenience: force-enable a location.
inline void EnableDynamicDebugLog(std::string_view file, int line = 0) {
    SetDynamicDebugLog(file, line, DynamicDebugState::kForceEnabled);
}

/// Convenience: force-disable a location.
inline void DisableDynamicDebugLog(std::string_view file, int line = 0) {
    SetDynamicDebugLog(file, line, DynamicDebugState::kForceDisabled);
}

/// Resets all dynamic-debug overrides.
void ResetDynamicDebugLog();

/// Snapshot view of a single statically-registered log site passed to
/// `ForEachLogEntry`. `file` / `line` are the raw `__FILE__` / `__LINE__`
/// captured at macro expansion; `state` is the current dynamic-debug
/// override resolved via the same lookup the LOG_* path uses.
struct LogEntryInfo {
    const char* file{nullptr};
    int line{0};
    DynamicDebugState state{DynamicDebugState::kDefault};
};

/// Invokes `cb` once per registered log site. Order is reverse of
/// registration (most recent first) and is not stable across runs.
///
/// The same (file, line) pair may appear more than once — a separate
/// `StaticLogEntry` is instantiated per translation unit that expands a
/// LOG_* macro at that location. Deduplicate on the consumer side if
/// needed.
///
/// Safe to invoke `SetDynamicDebugLog` / `EnableDynamicDebugLog` /
/// `DisableDynamicDebugLog` from inside the callback.
void ForEachLogEntry(const std::function<void(const LogEntryInfo&)>& cb);

namespace impl {

/// Resolves the configured state for a given source location.
DynamicDebugState LookupDynamicDebugLog(const char* file, int line) noexcept;

}  // namespace impl

}  // namespace ulog
