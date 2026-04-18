// Windows-only: named-event → AsyncLogger.RequestReopen bridge smoke test.
#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include <ulog/async_logger.hpp>
#include <ulog/sinks/base_sink.hpp>
#include <ulog/win/named_event_handler.hpp>

namespace {

class ReopenCounterSink final : public ulog::sinks::BaseSink {
public:
    void Write(std::string_view /*record*/) override {}
    void Reopen(ulog::sinks::ReopenMode /*mode*/) override {
        ++reopens_;
    }
    int Count() const noexcept { return reopens_.load(std::memory_order_relaxed); }

private:
    std::atomic<int> reopens_{0};
};

}  // namespace

TEST(WinNamedEventReopen, InstallsThenForwardsSetEvent) {
    // Unique per-test event name so parallel runs don't collide.
    const std::string name = "Local\\ulog-test-reopen-" + std::to_string(::GetCurrentProcessId());

    auto sink = std::make_shared<ReopenCounterSink>();
    auto logger = std::make_shared<ulog::AsyncLogger>();
    logger->SetLevel(ulog::Level::kTrace);
    logger->AddSink(sink);

    ulog::win::InstallNamedEventReopenHandler(logger, name);

    // External signal path: open the same named event and fire SetEvent.
    HANDLE ev = ::OpenEventA(EVENT_MODIFY_STATE, FALSE, name.c_str());
    ASSERT_NE(ev, nullptr);
    ::SetEvent(ev);
    ::CloseHandle(ev);

    // Allow the watcher + worker to pick it up.
    for (int i = 0; i < 100 && sink->Count() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GE(sink->Count(), 1);

    ulog::win::UninstallNamedEventReopenHandler();
    logger.reset();
}

TEST(WinNamedEventReopen, UninstallIsIdempotent) {
    EXPECT_NO_THROW(ulog::win::UninstallNamedEventReopenHandler());
    EXPECT_NO_THROW(ulog::win::UninstallNamedEventReopenHandler());
}

#endif  // _WIN32
