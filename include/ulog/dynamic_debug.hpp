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
#include <ulog/log.hpp>

#include <boost/intrusive/set.hpp>
#include <boost/intrusive/set_hook.hpp>

namespace ulog {

    inline constexpr int kAnyLine = 0;

    namespace bi = boost::intrusive;

    struct alignas(2 * sizeof(Level)) EntryState {
        Level force_enabled_level{ Level::kNone };
        Level force_disabled_level_plus_one{ Level::kTrace };
    };

    Level GetForceDisabledLevelPlusOne(Level level);

    enum class DynamicDebugState {
        kDefault,         ///< Use logger level as-is
        kForceEnabled,    ///< Always emit regardless of logger level
        kForceDisabled,   ///< Never emit regardless of logger level
    };

    static_assert(std::atomic<EntryState>::is_always_lock_free);

    using LogEntryContentHook = bi::set_base_hook<bi::optimize_size<true>, bi::link_mode<bi::normal_link>>;

    struct LogEntryContent {
        LogEntryContent(const char* path, int line) noexcept : line(line), path(path) {}

        std::atomic<EntryState> state{ EntryState{} };
        const int line;
        const char* const path;
        LogEntryContentHook hook;
    };

    bool operator<(const LogEntryContent& x, const LogEntryContent& y) noexcept;
    bool operator==(const LogEntryContent& x, const LogEntryContent& y) noexcept;

    // multiset to allow multiple logs on the same line (happens with templates).
    using LogEntryContentSet = bi::multiset<  //
        LogEntryContent,                      //
        bi::constant_time_size<false>,        //
        bi::member_hook<                      //
        LogEntryContent,                  //
        LogEntryContentHook,              //
        &LogEntryContent::hook            //
        >                                 //
    >;

    /// Sets the runtime state for a (file, line) pair. `line == 0` => applies to
    /// every line in the file. Matches are resolved by suffix: a configured
    /// `/foo/bar.cpp` matches any source whose path ends with `bar.cpp`.
    void SetDynamicDebugLog(const std::string& file, int line, DynamicDebugState state);

    void AddDynamicDebugLog(const std::string& location_relative, int line = kAnyLine);

    void RemoveDynamicDebugLog(const std::string& location_relative, int line = kAnyLine);

    void RemoveAllDynamicDebugLog();

    const LogEntryContentSet& GetDynamicDebugLocations();

    void RegisterLogLocation(LogEntryContent& location);

    /*
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
        const char* file{ nullptr };
        int line{ 0 };
        DynamicDebugState state{ DynamicDebugState::kDefault };
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
    */
}  // namespace ulog
