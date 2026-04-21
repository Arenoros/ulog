#include <ulog/dynamic_debug.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

#include <ulog/utils/underlying_value.hpp>

namespace ulog {

    namespace {

        LogEntryContentSet& GetAllLocations() noexcept {
            static LogEntryContentSet locations{};
            return locations;
        }

        [[noreturn]] void ThrowUnknownDynamicLogLocation(std::string_view location, int line) {
            if (line != kAnyLine) {
                throw std::runtime_error(fmt::format("dynamic-debug-log: no logging in '{}' at line {}", location, line));
            }

            throw std::runtime_error(fmt::format("dynamic-debug-log: no logging in '{}'", location));
        }

        //struct Key {
        //    std::string file;
        //    int line;
        //    bool operator<(const Key& o) const noexcept {
        //        if (file != o.file) return file < o.file;
        //        return line < o.line;
        //    }
        //};

        //struct Registry {
        //    std::shared_mutex mu;
        //    std::map<Key, DynamicDebugState> entries;  // line == 0 => all lines in file
        //};

        //Registry& Reg() noexcept {
        //    static Registry r;
        //    return r;
        //}

        ///// Lock-free fast-path marker. Set by any SetDynamicDebugLog that inserts a
        ///// non-default entry; cleared by ResetDynamicDebugLog or by erasing the last
        ///// entry. `LookupDynamicDebugLog` reads this before attempting a shared_lock,
        ///// so the common "no overrides" case is a single atomic load per LOG call.
        //std::atomic<bool> g_has_entries{ false };

        //bool PathSuffixMatch(std::string_view full, std::string_view pattern) noexcept {
        //    if (pattern.size() > full.size()) return false;
        //    if (!pattern.empty() && (pattern.front() == '/' || pattern.front() == '\\')) {
        //        pattern.remove_prefix(1);
        //    }
        //    if (pattern.size() > full.size()) return false;
        //    const auto offset = full.size() - pattern.size();
        //    for (std::size_t i = 0; i < pattern.size(); ++i) {
        //        char a = full[offset + i];
        //        char b = pattern[i];
        //        if (a == '\\') a = '/';
        //        if (b == '\\') b = '/';
        //        if (a != b) return false;
        //    }
        //    if (offset > 0) {
        //        const char c = full[offset - 1];
        //        if (c != '/' && c != '\\') return false;
        //    }
        //    return true;
        //}

    }  // namespace

    Level GetForceDisabledLevelPlusOne(Level level) {
        if (level == Level::kNone) {
            return Level::kTrace;
        }
        return static_cast<Level>(utils::UnderlyingValue(level) + 1);
    }

    bool operator<(const LogEntryContent& x, const LogEntryContent& y) noexcept {
        const auto cmp = std::strcmp(x.path, y.path);
        return cmp < 0 || (cmp == 0 && x.line < y.line);
    }

    bool operator==(const LogEntryContent& x, const LogEntryContent& y) noexcept {
        return x.line == y.line && std::strcmp(x.path, y.path) == 0;
    }

    void SetDynamicDebugLog(const std::string& location_relative, int line, EntryState state) {
        //utils::impl::AssertStaticRegistrationFinished();

        auto& all_locations = GetAllLocations();
        auto it_lower = all_locations.lower_bound({ location_relative.c_str(), line });

        bool found_match = false;
        if (line != kAnyLine) {
            for (; it_lower != all_locations.end(); ++it_lower) {
                // check for exact match
                if (line != it_lower->line || it_lower->path != location_relative) {
                    break;
                }
                it_lower->state.store(state);
                found_match = true;
            }
        } else {
            for (; it_lower != all_locations.end(); ++it_lower) {
                // compare prefixes
                if (std::strncmp(it_lower->path, location_relative.c_str(), location_relative.size()) != 0) {
                    break;
                }
                it_lower->state.store(state);
                found_match = true;
            }
        }

        if (!found_match) {
            ThrowUnknownDynamicLogLocation(location_relative, line);
        }
    }

    void AddDynamicDebugLog(const std::string& location_relative, int line) {
        SetDynamicDebugLog(location_relative, line, EntryState{/*force_enabled_level=*/Level::kTrace });
    }

    void RemoveDynamicDebugLog(const std::string& location_relative, int line) {
        SetDynamicDebugLog(location_relative, line, EntryState{});
    }

    void RemoveAllDynamicDebugLog() {
        //utils::impl::AssertStaticRegistrationFinished();
        auto& all_locations = GetAllLocations();
        for (auto& location : all_locations) {
            location.state.store(EntryState{});
        }
    }

    const LogEntryContentSet& GetDynamicDebugLocations() {
        //utils::impl::AssertStaticRegistrationFinished();
        return GetAllLocations();
    }

    void RegisterLogLocation(LogEntryContent& location) {
        //utils::impl::AssertStaticRegistrationAllowed("Dynamic debug logging location");
        assert(location.path);
        assert(location.line);
        GetAllLocations().insert(location);
    }
    
    void ForEachLogEntry(const std::function<void(const char* const path, int line, bool force_enabled)>& cb) {
        if (!cb) return;
        
        auto& all_locations = GetAllLocations();
        for (const auto& location : all_locations) {
            cb(location.path, location.line, location.state.load().force_enabled_level != Level::kNone);
        }
    }

    //void SetDynamicDebugLog(std::string_view file, int line, DynamicDebugState state) {
    //    auto& r = Reg();
    //    std::unique_lock lock(r.mu);
    //    Key k{ std::string(file), line };
    //    if (state == DynamicDebugState::kDefault) {
    //        r.entries.erase(k);
    //    } else {
    //        r.entries[k] = state;
    //    }
    //    g_has_entries.store(!r.entries.empty(), std::memory_order_release);
    //}

    //void ResetDynamicDebugLog() {
    //    auto& r = Reg();
    //    std::unique_lock lock(r.mu);
    //    r.entries.clear();
    //    g_has_entries.store(false, std::memory_order_release);
    //}

    //namespace impl {

    //    DynamicDebugState LookupDynamicDebugLog(const char* file, int line) noexcept {
    //        // Fast path: no overrides installed — never take the shared_lock.
    //        if (!g_has_entries.load(std::memory_order_acquire)) return DynamicDebugState::kDefault;
    //        if (!file) return DynamicDebugState::kDefault;

    //        auto& r = Reg();
    //        std::shared_lock lock(r.mu);
    //        if (r.entries.empty()) return DynamicDebugState::kDefault;

    //        const std::string_view full(file);
    //        DynamicDebugState result = DynamicDebugState::kDefault;
    //        for (const auto& [k, state] : r.entries) {
    //            const bool line_match = (k.line == 0) || (k.line == line);
    //            if (!line_match) continue;
    //            if (!PathSuffixMatch(full, k.file)) continue;
    //            // Most specific wins (file + line overrides file-only).
    //            if (k.line != 0 || result == DynamicDebugState::kDefault) result = state;
    //        }
    //        return result;
    //    }

    //}  // namespace impl

}  // namespace ulog
