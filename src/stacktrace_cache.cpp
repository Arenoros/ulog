#include <ulog/stacktrace_cache.hpp>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ulog {

namespace {

std::atomic<bool> g_enabled{true};

struct FrameKey {
    std::vector<std::uintptr_t> frames;
    bool operator==(const FrameKey& o) const noexcept { return frames == o.frames; }
};

struct FrameKeyHash {
    std::size_t operator()(const FrameKey& k) const noexcept {
        // FNV-1a over frame addresses.
        std::size_t h = 1469598103934665603ull;
        for (auto f : k.frames) {
            h ^= static_cast<std::size_t>(f);
            h *= 1099511628211ull;
        }
        return h;
    }
};

struct Cache {
    std::shared_mutex mu;
    std::unordered_map<FrameKey, std::string, FrameKeyHash> entries;
};

Cache& GetCache() noexcept {
    static Cache c;
    return c;
}

FrameKey MakeKey(const boost::stacktrace::stacktrace& st) {
    FrameKey k;
    k.frames.reserve(st.size());
    for (const auto& f : st) {
        k.frames.push_back(reinterpret_cast<std::uintptr_t>(f.address()));
    }
    return k;
}

}  // namespace

std::string StacktraceToString(const boost::stacktrace::stacktrace& st) {
    auto& cache = GetCache();
    auto key = MakeKey(st);
    {
        std::shared_lock lock(cache.mu);
        auto it = cache.entries.find(key);
        if (it != cache.entries.end()) return it->second;
    }
    std::string symbolized = boost::stacktrace::to_string(st);
    std::unique_lock lock(cache.mu);
    auto [it, _] = cache.entries.emplace(std::move(key), std::move(symbolized));
    return it->second;
}

std::string CurrentStacktrace() {
    if (!g_enabled.load(std::memory_order_relaxed)) return {};
    return StacktraceToString(boost::stacktrace::stacktrace());
}

void EnableStacktrace(bool enabled) noexcept {
    g_enabled.store(enabled, std::memory_order_relaxed);
}

bool IsStacktraceEnabled() noexcept {
    return g_enabled.load(std::memory_order_relaxed);
}

StacktraceGuard::StacktraceGuard(bool enabled) noexcept
    : previous_(g_enabled.exchange(enabled, std::memory_order_relaxed)) {}

StacktraceGuard::~StacktraceGuard() {
    g_enabled.store(previous_, std::memory_order_relaxed);
}

void ClearStacktraceCache() {
    auto& cache = GetCache();
    std::unique_lock lock(cache.mu);
    cache.entries.clear();
}

}  // namespace ulog
