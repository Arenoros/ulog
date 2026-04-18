#include <ulog/win/named_event_handler.hpp>

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace ulog::win {

namespace {

struct Watcher {
    std::mutex mu;
    HANDLE event_handle{nullptr};  ///< named event; guarded by mu
    HANDLE stop_handle{nullptr};   ///< anonymous auto-reset, breaks WaitFor loop
    std::thread worker;
    std::weak_ptr<AsyncLogger> logger;
    std::string event_name;
};

Watcher& Instance() noexcept {
    static Watcher w;
    return w;
}

void WatcherLoop(Watcher& w) {
    HANDLE handles[2];
    {
        std::lock_guard lock(w.mu);
        handles[0] = w.event_handle;
        handles[1] = w.stop_handle;
    }

    while (true) {
        const DWORD r = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (r == WAIT_OBJECT_0) {
            if (auto l = w.logger.lock()) l->RequestReopen(sinks::ReopenMode::kAppend);
            continue;
        }
        // WAIT_OBJECT_0 + 1 (stop) or any WAIT_FAILED / abandon → exit.
        break;
    }
}

void StopLocked(Watcher& w) {
    // Must be called with w.mu held, then released before join().
    if (w.stop_handle) ::SetEvent(w.stop_handle);
}

void CloseHandles(Watcher& w) {
    if (w.event_handle) { ::CloseHandle(w.event_handle); w.event_handle = nullptr; }
    if (w.stop_handle)  { ::CloseHandle(w.stop_handle);  w.stop_handle  = nullptr; }
}

}  // namespace

void InstallNamedEventReopenHandler(const std::shared_ptr<AsyncLogger>& logger,
                                    const std::string& event_name) {
    UninstallNamedEventReopenHandler();

    auto& w = Instance();
    {
        std::lock_guard lock(w.mu);
        w.logger = logger;
        w.event_name = event_name;

        // Auto-reset named event so each SetEvent fires the watcher exactly once.
        w.event_handle = ::CreateEventA(nullptr, /*manualReset=*/FALSE,
                                        /*initialState=*/FALSE, event_name.c_str());
        if (!w.event_handle) return;

        w.stop_handle = ::CreateEventA(nullptr, FALSE, FALSE, nullptr);
        if (!w.stop_handle) {
            CloseHandles(w);
            return;
        }
    }

    w.worker = std::thread([&w] { WatcherLoop(w); });
}

void UninstallNamedEventReopenHandler() {
    auto& w = Instance();
    std::thread joinable_thread;
    {
        std::lock_guard lock(w.mu);
        if (!w.event_handle && !w.stop_handle && !w.worker.joinable()) return;
        StopLocked(w);
        joinable_thread = std::move(w.worker);
    }
    if (joinable_thread.joinable()) joinable_thread.join();

    std::lock_guard lock(w.mu);
    CloseHandles(w);
    w.logger.reset();
    w.event_name.clear();
}

}  // namespace ulog::win

#endif  // _WIN32
