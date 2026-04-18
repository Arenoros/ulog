#include <ulog/async_logger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <utility>

#include <concurrentqueue.h>

namespace ulog {

namespace {

struct LogRecord {
    Level level{Level::kInfo};
    std::string payload;
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

    void WakeWorker() { cv.notify_all(); }

    bool TryEnqueueRecord(LogRecord&& rec) {
        if (config.queue_capacity > 0 && pending.load(std::memory_order_relaxed) >= config.queue_capacity) {
            if (config.overflow == OverflowBehavior::kDiscard) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            // Block until space frees up.
            std::unique_lock lock(wait_mu);
            cv.wait(lock, [&] {
                return stop.load(std::memory_order_relaxed) ||
                       pending.load(std::memory_order_relaxed) < config.queue_capacity;
            });
            if (stop.load(std::memory_order_relaxed)) return false;
        }
        records.enqueue(std::move(rec));
        pending.fetch_add(1, std::memory_order_relaxed);
        WakeWorker();
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
                    for (const auto& s : snapshot) {
                        if (!s->ShouldLog(batch[i].level)) continue;
                        try { s->Write(batch[i].payload); }
                        catch (...) {}
                    }
                }
                pending.fetch_sub(n, std::memory_order_relaxed);
                total_logged.fetch_add(n, std::memory_order_relaxed);
                WakeWorker();  // in case a blocked producer was waiting on capacity
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

        // Final drain after stop signal.
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
    state_->WakeWorker();
    if (state_->worker.joinable()) state_->worker.join();
}

void AsyncLogger::AddSink(sinks::SinkPtr sink) {
    if (!sink) return;
    std::lock_guard lk(state_->sinks_mu);
    state_->sinks.push_back(std::move(sink));
}

void AsyncLogger::RequestReopen(sinks::ReopenMode mode) {
    state_->reopens.enqueue({mode});
    state_->WakeWorker();
}

void AsyncLogger::Log(Level level, impl::LoggerItemRef item) {
    auto& text = static_cast<impl::formatters::TextLogItem&>(item);
    LogRecord rec;
    rec.level = level;
    rec.payload.assign(text.payload.data(), text.payload.size());
    state_->TryEnqueueRecord(std::move(rec));
}

void AsyncLogger::Flush() {
    std::promise<void> done;
    auto fut = done.get_future();
    state_->flushes.enqueue({&done});
    state_->WakeWorker();
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
