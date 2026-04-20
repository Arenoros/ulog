#include <ulog/record_enricher.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/make_shared.hpp>
#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <processthreadsapi.h>
#else
#  include <pthread.h>
#endif

namespace ulog {

namespace {

struct Entry {
    RecordEnricherHandle handle{0};
    RecordEnricher hook{nullptr};
    void* user_ctx{nullptr};
};

using EntryList = std::vector<Entry>;

/// Copy-on-write registry. Readers pin a snapshot via the atomic
/// shared_ptr — the snapshot is immutable for its lifetime, so the LOG_*
/// hot path walks the vector without holding any lock. Writers serialize
/// under `g_registry_mu` to avoid lost updates, then publish a new vector.
struct Registry {
    boost::atomic_shared_ptr<EntryList> entries;
    std::mutex mu;
    std::atomic<std::uint64_t> next_handle{1};
    /// Fast-path marker. Readers short-circuit before touching the atomic
    /// shared_ptr when no enrichers are installed — one relaxed load per
    /// record on the common path.
    std::atomic<bool> has_entries{false};
};

Registry& Reg() noexcept {
    static Registry r;
    return r;
}

boost::shared_ptr<EntryList> LoadList() noexcept {
    return Reg().entries.load();
}

void PublishList(boost::shared_ptr<EntryList> new_list) {
    const bool any = new_list && !new_list->empty();
    Reg().entries.store(std::move(new_list));
    Reg().has_entries.store(any, std::memory_order_release);
}

boost::shared_ptr<EntryList> CloneOrEmpty() {
    auto cur = LoadList();
    if (!cur) return boost::make_shared<EntryList>();
    return boost::make_shared<EntryList>(*cur);
}

}  // namespace

RecordEnricherHandle AddRecordEnricher(RecordEnricher hook, void* user_ctx) {
    if (!hook) return 0;
    std::lock_guard lk(Reg().mu);
    auto next = CloneOrEmpty();
    const auto h = Reg().next_handle.fetch_add(1, std::memory_order_relaxed);
    next->push_back({h, hook, user_ctx});
    PublishList(std::move(next));
    return h;
}

void RemoveRecordEnricher(RecordEnricherHandle handle) noexcept {
    if (handle == 0) return;
    try {
        std::lock_guard lk(Reg().mu);
        auto cur = LoadList();
        if (!cur || cur->empty()) return;
        auto next = boost::make_shared<EntryList>();
        next->reserve(cur->size());
        for (const auto& e : *cur) {
            if (e.handle != handle) next->push_back(e);
        }
        if (next->size() == cur->size()) return;  // not found
        PublishList(std::move(next));
    } catch (...) {
        // Never throw from this API — shutdown / teardown callers rely on
        // noexcept. Leaking the entry on allocation failure is preferable
        // to aborting.
    }
}

void ClearRecordEnrichers() noexcept {
    try {
        std::lock_guard lk(Reg().mu);
        PublishList(boost::make_shared<EntryList>());
    } catch (...) {
    }
}

namespace impl {

void DispatchRecordEnrichers(TagSink& sink) {
    if (!Reg().has_entries.load(std::memory_order_acquire)) return;
    auto list = LoadList();
    if (!list) return;
    for (const auto& e : *list) {
        if (e.hook) e.hook(sink, e.user_ctx);
    }
}

}  // namespace impl

// ---------------- Built-in enrichers ----------------

namespace {

void ThreadIdHook(TagSink& sink, void* user_ctx) noexcept {
    const auto id_hash = std::hash<std::thread::id>{}(std::this_thread::get_id());
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%zu", id_hash);
    if (n <= 0) return;
    const auto* key = static_cast<const std::string*>(user_ctx);
    sink.AddTag(std::string_view(*key), std::string_view(buf, static_cast<std::size_t>(n)));
}

std::string ReadCurrentThreadName() {
#if defined(_WIN32)
    PWSTR wname = nullptr;
    if (FAILED(::GetThreadDescription(::GetCurrentThread(), &wname)) || !wname) return {};
    std::wstring wide(wname);
    ::LocalFree(wname);
    if (wide.empty()) return {};
    // Cheap narrow conversion — thread names are ASCII in practice. Anything
    // outside the ASCII range is dropped rather than returning replacement
    // characters that would muddy log readability.
    std::string out;
    out.reserve(wide.size());
    for (wchar_t w : wide) {
        if (w > 0 && w < 128) out.push_back(static_cast<char>(w));
    }
    return out;
#else
    char buf[64] = {};
    if (::pthread_getname_np(::pthread_self(), buf, sizeof(buf)) != 0) return {};
    return std::string(buf);
#endif
}

void ThreadNameHook(TagSink& sink, void* user_ctx) noexcept {
    try {
        const auto name = ReadCurrentThreadName();
        if (name.empty()) return;
        const auto* key = static_cast<const std::string*>(user_ctx);
        sink.AddTag(std::string_view(*key), std::string_view(name));
    } catch (...) {
        // noexcept contract — swallow allocation failures.
    }
}

/// Holds the key string for a built-in hook for the lifetime of the
/// registration. Freed when the enricher is removed (handled manually
/// here — small leak on shutdown is acceptable).
struct KeyHolder {
    std::string key;
};

std::mutex& KeyHoldersMu() noexcept {
    static std::mutex m;
    return m;
}

std::vector<std::unique_ptr<KeyHolder>>& KeyHolders() noexcept {
    static std::vector<std::unique_ptr<KeyHolder>> v;
    return v;
}

std::string* InstallKeyHolder(std::string_view key) {
    std::lock_guard lk(KeyHoldersMu());
    auto h = std::make_unique<KeyHolder>();
    h->key = std::string(key);
    auto* ptr = &h->key;
    KeyHolders().push_back(std::move(h));
    return ptr;
}

}  // namespace

RecordEnricherHandle EnableThreadIdEnricher(std::string_view key) {
    auto* key_ptr = InstallKeyHolder(key);
    return AddRecordEnricher(&ThreadIdHook, key_ptr);
}

RecordEnricherHandle EnableThreadNameEnricher(std::string_view key) {
    auto* key_ptr = InstallKeyHolder(key);
    return AddRecordEnricher(&ThreadNameHook, key_ptr);
}

}  // namespace ulog
