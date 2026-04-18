#include <ulog/dynamic_debug.hpp>

#include <atomic>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace ulog {

namespace {

struct Key {
    std::string file;
    int line;
    bool operator<(const Key& o) const noexcept {
        if (file != o.file) return file < o.file;
        return line < o.line;
    }
};

struct Registry {
    std::shared_mutex mu;
    std::map<Key, DynamicDebugState> entries;  // line == 0 => all lines in file
};

Registry& Reg() noexcept {
    static Registry r;
    return r;
}

/// Lock-free fast-path marker. Set by any SetDynamicDebugLog that inserts a
/// non-default entry; cleared by ResetDynamicDebugLog or by erasing the last
/// entry. `LookupDynamicDebugLog` reads this before attempting a shared_lock,
/// so the common "no overrides" case is a single atomic load per LOG call.
std::atomic<bool> g_has_entries{false};

bool PathSuffixMatch(std::string_view full, std::string_view pattern) noexcept {
    if (pattern.size() > full.size()) return false;
    if (!pattern.empty() && (pattern.front() == '/' || pattern.front() == '\\')) {
        pattern.remove_prefix(1);
    }
    if (pattern.size() > full.size()) return false;
    const auto offset = full.size() - pattern.size();
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char a = full[offset + i];
        char b = pattern[i];
        if (a == '\\') a = '/';
        if (b == '\\') b = '/';
        if (a != b) return false;
    }
    if (offset > 0) {
        const char c = full[offset - 1];
        if (c != '/' && c != '\\') return false;
    }
    return true;
}

}  // namespace

void SetDynamicDebugLog(std::string_view file, int line, DynamicDebugState state) {
    auto& r = Reg();
    std::unique_lock lock(r.mu);
    Key k{std::string(file), line};
    if (state == DynamicDebugState::kDefault) {
        r.entries.erase(k);
    } else {
        r.entries[k] = state;
    }
    g_has_entries.store(!r.entries.empty(), std::memory_order_release);
}

void ResetDynamicDebugLog() {
    auto& r = Reg();
    std::unique_lock lock(r.mu);
    r.entries.clear();
    g_has_entries.store(false, std::memory_order_release);
}

namespace impl {

DynamicDebugState LookupDynamicDebugLog(const char* file, int line) noexcept {
    // Fast path: no overrides installed — never take the shared_lock.
    if (!g_has_entries.load(std::memory_order_acquire)) return DynamicDebugState::kDefault;
    if (!file) return DynamicDebugState::kDefault;

    auto& r = Reg();
    std::shared_lock lock(r.mu);
    if (r.entries.empty()) return DynamicDebugState::kDefault;

    const std::string_view full(file);
    DynamicDebugState result = DynamicDebugState::kDefault;
    for (const auto& [k, state] : r.entries) {
        const bool line_match = (k.line == 0) || (k.line == line);
        if (!line_match) continue;
        if (!PathSuffixMatch(full, k.file)) continue;
        // Most specific wins (file + line overrides file-only).
        if (k.line != 0 || result == DynamicDebugState::kDefault) result = state;
    }
    return result;
}

}  // namespace impl

}  // namespace ulog
