#include <ulog/async_logger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <concurrentqueue.h>

#include <ulog/impl/formatters/text_item.hpp>

namespace ulog {

namespace {

struct LogRecord {
    Level level{Level::kInfo};
    /// Produced by the formatter on the producer thread; the worker only
    /// reads (never mutates). Owning pointer — consumed exactly once.
    std::unique_ptr<impl::LoggerItemBase> item;
};

struct ReopenRequest {
    sinks::ReopenMode mode;
};

struct FlushRequest {
    std::promise<void>* promise;
};

}  // namespace

struct AsyncLogger::State {
    Config config;

    moodycamel::ConcurrentQueue<LogRecord> records;
    moodycamel::ConcurrentQueue<ReopenRequest> reopens;
    moodycamel::ConcurrentQueue<FlushRequest> flushes;

    std::mutex sinks_mu;
    std::vector<sinks::SinkPtr> sinks;

    std::mutex wait_mu;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> pending{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> total_logged{0};

    std::thread worker;

    explicit State(const Config& cfg)
        : config(cfg), records(cfg.queue_capacity > 0 ? cfg.queue_capacity : 32) {}

    /// Wakes the worker only when it might actually be asleep. When pending
    /// was already non-zero before this producer's fetch_add, the worker is
    /// either draining already or about to wake on its next CV wait_for.
    void WakeIfIdle(std::size_t prev_pending) {
        if (prev_pending == 0) cv.notify_one();
    }

    void WakeForControl() { cv.notify_all(); }

    bool TryEnqueueRecord(LogRecord&& rec) {
        if (config.queue_capacity > 0 &&
            pending.load(std::memory_order_relaxed) >= config.queue_capacity) {
            if (config.overflow == OverflowBehavior::kDiscard) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            std::unique_lock lock(wait_mu);
            cv.wait(lock, [&] {
                return stop.load(std::memory_order_relaxed) ||
                       pending.load(std::memory_order_relaxed) < config.queue_capacity;
            });
            if (stop.load(std::memory_order_relaxed)) return false;
        }
        records.enqueue(std::move(rec));
        const auto prev = pending.fetch_add(1, std::memory_order_release);
        WakeIfIdle(prev);
        return true;
    }

    void WorkerLoop() {
        constexpr std::size_t kBatch = 256;
        std::vector<LogRecord> batch(kBatch);

        auto drain_records = [&]() {
            while (true) {
                const std::size_t n = records.try_dequeue_bulk(batch.begin(), kBatch);
                if (n == 0) return;
                std::vector<sinks::SinkPtr> snapshot;
                {
                    std::lock_guard lk(sinks_mu);
                    snapshot = sinks;
                }
                for (std::size_t i = 0; i < n; ++i) {
                    auto* text = static_cast<impl::formatters::TextLogItem*>(batch[i].item.get());
                    if (!text) continue;
                    const auto view = text->payload.view();
                    for (const auto& s : snapshot) {
                        if (!s->ShouldLog(batch[i].level)) continue;
                        try { s->Write(view); }
                        catch (...) {}
                    }
                    batch[i].item.reset();
                }
                pending.fetch_sub(n, std::memory_order_release);
                total_logged.fetch_add(n, std::memory_order_relaxed);
                // Blocked producers waiting on capacity may be parked.
                WakeForControl();
            }
        };

        auto drain_reopens = [&]() {
            ReopenRequest req{};
            while (reopens.try_dequeue(req)) {
                std::vector<sinks::SinkPtr> snapshot;
                {
                    std::lock_guard lk(sinks_mu);
                    snapshot = sinks;
                }
                for (const auto& s : snapshot) {
                    try { s->Reopen(req.mode); }
                    catch (...) {}
                }
            }
        };

        auto drain_flushes = [&]() {
            FlushRequest req{};
            while (flushes.try_dequeue(req)) {
                std::vector<sinks::SinkPtr> snapshot;
                {
                    std::lock_guard lk(sinks_mu);
                    snapshot = sinks;
                }
                for (const auto& s : snapshot) {
                    try { s->Flush(); }
                    catch (...) {}
                }
                if (req.promise) req.promise->set_value();
            }
        };

        while (!stop.load(std::memory_order_acquire)) {
            drain_records();
            drain_reopens();
            drain_flushes();

            std::unique_lock lk(wait_mu);
            cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
                return stop.load(std::memory_order_relaxed) ||
                       pending.load(std::memory_order_relaxed) > 0;
            });
        }

        drain_records();
        drain_reopens();
        drain_flushes();
    }
};

// ---------------- AsyncLogger ----------------

AsyncLogger::AsyncLogger(const Config& cfg) : impl::TextLoggerBase(cfg.format) {
    state_ = std::make_unique<State>(cfg);
    state_->worker = std::thread([this] { state_->WorkerLoop(); });
}

AsyncLogger::~AsyncLogger() {
    state_->stop.store(true, std::memory_order_release);
    state_->WakeForControl();
    if (state_->worker.joinable()) state_->worker.join();
}

void AsyncLogger::AddSink(sinks::SinkPtr sink) {
    if (!sink) return;
    std::lock_guard lk(state_->sinks_mu);
    state_->sinks.push_back(std::move(sink));
}

void AsyncLogger::RequestReopen(sinks::ReopenMode mode) {
    state_->reopens.enqueue({mode});
    state_->WakeForControl();
}

void AsyncLogger::Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) {
    if (!item) return;
    LogRecord rec;
    rec.level = level;
    rec.item = std::move(item);
    state_->TryEnqueueRecord(std::move(rec));
}

void AsyncLogger::Flush() {
    std::promise<void> done;
    auto fut = done.get_future();
    state_->flushes.enqueue({&done});
    state_->WakeForControl();
    fut.wait();
}

std::uint64_t AsyncLogger::GetDroppedCount() const noexcept {
    return state_->dropped.load(std::memory_order_relaxed);
}

std::size_t AsyncLogger::GetQueueDepth() const noexcept {
    return state_->pending.load(std::memory_order_relaxed);
}

std::uint64_t AsyncLogger::GetTotalLogged() const noexcept {
    return state_->total_logged.load(std::memory_order_relaxed);
}

}  // namespace ulog
