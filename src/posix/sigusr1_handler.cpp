#include <ulog/posix/sigusr1_handler.hpp>

#if !defined(_WIN32)

#include <atomic>
#include <csignal>
#include <memory>
#include <mutex>

namespace ulog::posix {

namespace {

std::mutex g_mu;
std::weak_ptr<AsyncLogger> g_logger;
std::atomic<bool> g_flag{false};

void HandleSigUsr1(int) noexcept {
    // Signal-handler safe: just set a flag. The actual reopen runs on the
    // worker thread via a periodic checker (below).
    g_flag.store(true, std::memory_order_release);
}

std::thread& PollerThread() {
    static std::thread t{[] {
        using namespace std::chrono_literals;
        while (true) {
            std::this_thread::sleep_for(200ms);
            if (!g_flag.exchange(false, std::memory_order_acq_rel)) continue;
            std::shared_ptr<AsyncLogger> logger;
            {
                std::lock_guard lock(g_mu);
                logger = g_logger.lock();
            }
            if (logger) logger->RequestReopen(sinks::ReopenMode::kAppend);
        }
    }};
    return t;
}

}  // namespace

void InstallSigUsr1ReopenHandler(const std::shared_ptr<AsyncLogger>& logger) {
    {
        std::lock_guard lock(g_mu);
        g_logger = logger;
    }

    struct sigaction sa{};
    sa.sa_handler = &HandleSigUsr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGUSR1, &sa, nullptr);

    (void)PollerThread();  // ensure thread is started exactly once
}

void UninstallSigUsr1ReopenHandler() {
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    ::sigaction(SIGUSR1, &sa, nullptr);
    std::lock_guard lock(g_mu);
    g_logger.reset();
}

}  // namespace ulog::posix

#endif  // !_WIN32
