#include <ulog/async_logger.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <boost/make_shared.hpp>
#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include <concurrentqueue.h>

#include <ulog/impl/formatters/text_item.hpp>

namespace ulog {

namespace {

struct QueueRecord {
    Level level{Level::kInfo};
    /// Produced by the formatter on the producer thread; the worker only
    /// reads (never mutates). Owning list — one item per active format
    /// (inline-1 small_vector keeps the common single-format path free of
    /// heap allocations).
    impl::LogItemList items;
    /// Non-null when the logger has at least one structured sink. The
    /// producer built the record under `LogHelper::~LogHelper`; the
    /// worker fans it out to every StructuredSink on consumption.
    std::unique_ptr<sinks::LogRecord> structured;
};

/// Non-owning view into a sink + its registered format index. Mirrors
/// SyncLogger::SinkEntry so the worker can route each item to the sinks
/// whose format matches.
struct SinkEntry {
    sinks::SinkPtr sink;
    std::size_t format_idx;
};

struct ReopenRequest {
    sinks::ReopenMode mode;
};

struct FlushRequest {
    std::promise<void>* promise;
};

/// Per-thread cache for the most recently used AsyncLogger's
/// `ProducerToken`. moodycamel's implicit-producer path walks a lock-free
/// hashtable keyed by thread id on every `enqueue(...)` call; supplying a
/// `ProducerToken` skips that lookup and talks directly to the per-token
/// producer block. See `docs/BACKLOG.md` "Moodycamel per-producer token
/// caching" for the 8-thread regression that motivates this.
///
/// Single-slot cache: the overwhelming-common case is one AsyncLogger per
/// process (the default logger). Threads that publish to multiple loggers
/// will thrash this slot — not great, but still correct: the slow path
/// rebinds and re-caches, pointer identity plus a monotonic generation
/// counter stops stale-pointer reuse when a new State happens to reclaim
/// the old allocation address.
struct TlsProducerCache {
    const void* owner{nullptr};                 ///< State* (opaque here).
    std::uint64_t generation{0};                ///< State::generation snapshot.
    moodycamel::ProducerToken* token{nullptr};  ///< Non-owning; State owns.
};

thread_local TlsProducerCache g_tls_producer{};

}  // namespace

struct AsyncLogger::State {
    /// Generation id — monotonically bumped on every State construction
    /// so the TLS producer cache can distinguish "same State" from "new
    /// State at the reclaimed address". Read-only after construction.
    static std::atomic<std::uint64_t>& NextGeneration() {
        static std::atomic<std::uint64_t> next{1};
        return next;
    }

    Config config;

    moodycamel::ConcurrentQueue<QueueRecord> records;
    moodycamel::ConcurrentQueue<ReopenRequest> reopens;
    moodycamel::ConcurrentQueue<FlushRequest> flushes;

    using SinkVec = std::vector<SinkEntry>;
    using StructSinkVec = std::vector<sinks::StructuredSinkPtr>;

    /// COW sink registries. The worker loads the atomic shared_ptr once
    /// per batch — no mutex acquired on the drain path. Writers
    /// (AddSink / AddStructuredSink) serialize on the mutex and publish
    /// a fresh vector; the worker's already-pinned snapshot remains
    /// valid for the rest of the batch.
    std::mutex sinks_mu;
    boost::atomic_shared_ptr<SinkVec const> sinks;
    std::mutex struct_sinks_mu;
    boost::atomic_shared_ptr<StructSinkVec const> struct_sinks;

    std::mutex wait_mu;
    /// Producer → worker wake-up. Fired when a record was enqueued and
    /// the queue transitioned from empty to non-empty (single waiter
    /// sufficient — other workers stay asleep until they see work of
    /// their own).
    std::condition_variable records_cv;
    /// Worker → producer wake-up. Fired when `pending` transitions
    /// from ≥ capacity to < capacity, releasing blocked producers in
    /// `kBlock` overflow mode. Separate CV avoids waking workers on
    /// every drain cycle (they spin on `records_cv` only).
    std::condition_variable space_cv;
    /// Shutdown + control plane (flushes / reopens). Lightweight path —
    /// shared between workers for stop notification.
    std::condition_variable control_cv;
    std::atomic<bool> stop{false};
    std::atomic<std::size_t> pending{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> total_logged{0};

    /// ProducerTokens cached per producing thread. State owns the tokens
    /// so they are destroyed strictly before the queue (vector is the
    /// last member destroyed; deletion happens before `records` goes
    /// out of scope since member destruction is reverse declaration
    /// order — see `producers` position below).
    std::mutex producers_mu;
    std::vector<std::unique_ptr<moodycamel::ProducerToken>> producers;

    /// Unique identity for cache-invalidation on State reuse.
    const std::uint64_t generation;

    /// One or more consumer threads draining `records` / `reopens` /
    /// `flushes`. Sized from `config.worker_count` in the ctor; never
    /// resized afterwards. `WorkerLoop` is multi-consumer-safe — each
    /// thread independently pulls bulk batches from the lock-free
    /// moodycamel queue and dispatches to sinks. Sinks must be
    /// thread-safe when the vector has more than one element (see
    /// `Config::worker_count` docs).
    std::vector<std::thread> workers;

    explicit State(const Config& cfg)
        : config(cfg),
          records(cfg.queue_capacity > 0 ? cfg.queue_capacity : 32),
          generation(NextGeneration().fetch_add(1, std::memory_order_relaxed)) {}

    /// Slow path: walked on first enqueue from a given thread or after
    /// a cache invalidation. Protected by a mutex since the producers
    /// vector is shared across threads; contention is bounded to one hit
    /// per (thread, logger) pair for the lifetime of the logger.
    moodycamel::ProducerToken& AcquireToken() {
        auto& tls = g_tls_producer;
        if (tls.owner == this && tls.generation == generation) {
            return *tls.token;
        }
        auto tok = std::make_unique<moodycamel::ProducerToken>(records);
        auto* raw = tok.get();
        {
            std::lock_guard lk(producers_mu);
            producers.push_back(std::move(tok));
        }
        tls.owner = this;
        tls.generation = generation;
        tls.token = raw;
        return *raw;
    }

    /// Destruction order matters: the `producers` vector must be cleared
    /// before `records` is destroyed so ProducerToken destructors can
    /// detach cleanly from the queue's producer list. Member destruction
    /// runs in reverse declaration order — `producers` is declared after
    /// `records`, so it is destroyed first. The explicit dtor below is a
    /// belt-and-suspenders clear in case the class grows more members.
    ~State() {
        std::lock_guard lk(producers_mu);
        producers.clear();
    }

    /// Wakes ONE worker when the queue transitioned from empty to
    /// non-empty. Other workers stay asleep — they'll wake on their own
    /// enqueue or on `records_cv.notify_all()` at shutdown. `notify_one`
    /// avoids thundering-herd where all workers wake, find the queue
    /// already drained by the first responder, and go back to sleep.
    void WakeIfIdle(std::size_t prev_pending) {
        if (prev_pending == 0) records_cv.notify_one();
    }

    /// Wakes producers waiting on capacity release. Called only when
    /// `pending` actually crossed the capacity threshold downward — not
    /// on every drain cycle.
    void WakeProducersForSpace() { space_cv.notify_all(); }

    /// Wakes every worker for shutdown + every producer that may be
    /// parked on capacity. Single-shot broadcast used by the dtor path.
    void WakeForControl() {
        records_cv.notify_all();
        space_cv.notify_all();
        control_cv.notify_all();
    }

    bool TryEnqueueRecord(QueueRecord&& rec) {
        if (config.queue_capacity > 0 &&
            pending.load(std::memory_order_relaxed) >= config.queue_capacity) {
            if (config.overflow == OverflowBehavior::kDiscard) {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            std::unique_lock lock(wait_mu);
            space_cv.wait(lock, [&] {
                return stop.load(std::memory_order_relaxed) ||
                       pending.load(std::memory_order_relaxed) < config.queue_capacity;
            });
            if (stop.load(std::memory_order_relaxed)) return false;
        }
        records.enqueue(AcquireToken(), std::move(rec));
        const auto prev = pending.fetch_add(1, std::memory_order_release);
        WakeIfIdle(prev);
        return true;
    }

    void WorkerLoop() {
        constexpr std::size_t kBatch = 256;
        std::vector<QueueRecord> batch(kBatch);

        auto drain_records = [&]() {
            while (true) {
                const std::size_t n = records.try_dequeue_bulk(batch.begin(), kBatch);
                if (n == 0) return;
                auto snap = sinks.load();
                auto ss_snap = struct_sinks.load();
                for (std::size_t i = 0; i < n; ++i) {
                    auto& rec = batch[i];
                    // Text sink fan-out.
                    if (!rec.items.empty() && snap) {
                        for (const auto& entry : *snap) {
                            if (!entry.sink->ShouldLog(rec.level)) continue;
                            // Sink registered AFTER this record was materialized
                            // is invisible to it — skip rather than fall through
                            // to items[0] (wrong format).
                            if (entry.format_idx >= rec.items.size()) continue;
                            auto* text = static_cast<impl::formatters::TextLogItem*>(rec.items[entry.format_idx].get());
                            if (!text) continue;  // OOM upstream — drop sink write
                            try { entry.sink->Write(text->payload.view()); }
                            catch (...) {}
                        }
                        rec.items.clear();
                    }
                    // Structured sink fan-out — independent of text path; a
                    // sink added AFTER the producer built the record is
                    // invisible to this record (same semantics as the text
                    // side's out-of-range skip).
                    if (rec.structured && ss_snap) {
                        for (const auto& sink : *ss_snap) {
                            if (!sink || !sink->ShouldLog(rec.level)) continue;
                            try { sink->Write(*rec.structured); }
                            catch (...) {}
                        }
                        rec.structured.reset();
                    }
                }
                const auto prev = pending.fetch_sub(n, std::memory_order_release);
                total_logged.fetch_add(n, std::memory_order_relaxed);
                // Wake blocked producers only if we actually released
                // capacity — avoids `notify_all` thundering-herd on the
                // shared cv for every drain batch. `prev` is the
                // pre-subtraction value; crossing from `≥ capacity` to
                // `< capacity` means at least one producer may now make
                // progress.
                if (config.queue_capacity > 0 &&
                    prev >= config.queue_capacity &&
                    prev - n < config.queue_capacity) {
                    WakeProducersForSpace();
                }
            }
        };

        auto drain_reopens = [&]() {
            ReopenRequest req{};
            while (reopens.try_dequeue(req)) {
                auto snap = sinks.load();
                auto ss_snap = struct_sinks.load();
                if (snap) {
                    for (const auto& entry : *snap) {
                        try { entry.sink->Reopen(req.mode); }
                        catch (...) {}
                    }
                }
                if (ss_snap) {
                    for (const auto& sink : *ss_snap) {
                        try { if (sink) sink->Reopen(req.mode); }
                        catch (...) {}
                    }
                }
            }
        };

        auto drain_flushes = [&]() {
            FlushRequest req{};
            while (flushes.try_dequeue(req)) {
                auto snap = sinks.load();
                auto ss_snap = struct_sinks.load();
                if (snap) {
                    for (const auto& entry : *snap) {
                        try { entry.sink->Flush(); }
                        catch (...) {}
                    }
                }
                if (ss_snap) {
                    for (const auto& sink : *ss_snap) {
                        try { if (sink) sink->Flush(); }
                        catch (...) {}
                    }
                }
                if (req.promise) req.promise->set_value();
            }
        };

        while (!stop.load(std::memory_order_acquire)) {
            drain_records();
            drain_reopens();
            drain_flushes();

            std::unique_lock lk(wait_mu);
            // Wait on the producer→worker CV only. Workers are not
            // woken by `WakeProducersForSpace` — they park here until
            // real work lands or the 50 ms safety timeout expires.
            records_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
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

AsyncLogger::AsyncLogger() : AsyncLogger(Config{}) {}

AsyncLogger::AsyncLogger(const Config& cfg)
    : impl::TextLoggerBase(cfg.format, cfg.emit_location, cfg.timestamp_format) {
    // Start with no sinks — LogHelper must see the accurate flag until
    // AddSink / AddStructuredSink flips it.
    SetHasTextSinks(false);
    state_ = std::make_unique<State>(cfg);
    const std::size_t n = cfg.worker_count > 0 ? cfg.worker_count : 1;
    state_->workers.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        state_->workers.emplace_back([this] { state_->WorkerLoop(); });
    }
}

AsyncLogger::~AsyncLogger() {
    state_->stop.store(true, std::memory_order_release);
    // notify_all wakes every worker — notify_one would wake just one
    // and leave the rest blocked on the cv until their 50ms timeout
    // elapses, prolonging shutdown.
    state_->WakeForControl();
    for (auto& w : state_->workers) {
        if (w.joinable()) w.join();
    }
}

namespace {

/// Publishes a copy of `current` with `entry` appended. Writers hold
/// the list's serializing mutex; readers load the atomic shared_ptr
/// lock-free.
template <typename Vec, typename Entry>
boost::shared_ptr<Vec const> AppendCow(
        const boost::shared_ptr<Vec const>& current,
        Entry&& entry) {
    auto next = boost::make_shared<Vec>();
    if (current) {
        next->reserve(current->size() + 1);
        *next = *current;
    }
    next->push_back(std::forward<Entry>(entry));
    return boost::shared_ptr<Vec const>(std::move(next));
}

}  // namespace

void AsyncLogger::AddSink(sinks::SinkPtr sink) {
    if (!sink) return;
    const auto idx = RegisterSinkFormat(std::nullopt);
    std::lock_guard lk(state_->sinks_mu);
    auto next = AppendCow<State::SinkVec>(state_->sinks.load(),
                                          SinkEntry{std::move(sink), idx});
    state_->sinks.store(std::move(next));
    SetHasTextSinks(true);
}

void AsyncLogger::AddSink(sinks::SinkPtr sink, Format format_override) {
    if (!sink) return;
    const auto idx = RegisterSinkFormat(format_override);
    std::lock_guard lk(state_->sinks_mu);
    auto next = AppendCow<State::SinkVec>(state_->sinks.load(),
                                          SinkEntry{std::move(sink), idx});
    state_->sinks.store(std::move(next));
    SetHasTextSinks(true);
}

void AsyncLogger::AddStructuredSink(sinks::StructuredSinkPtr sink) {
    if (!sink) return;
    std::lock_guard lk(state_->struct_sinks_mu);
    auto next = AppendCow<State::StructSinkVec>(state_->struct_sinks.load(),
                                                std::move(sink));
    state_->struct_sinks.store(std::move(next));
    SetHasStructuredSinks(true);
}

void AsyncLogger::RequestReopen(sinks::ReopenMode mode) {
    state_->reopens.enqueue({mode});
    state_->WakeForControl();
}

void AsyncLogger::Log(Level level, std::unique_ptr<impl::LoggerItemBase> item) {
    if (!item) return;
    QueueRecord rec;
    rec.level = level;
    rec.items.push_back(std::move(item));
    state_->TryEnqueueRecord(std::move(rec));
}

void AsyncLogger::LogMulti(Level level,
                           impl::LogItemList items,
                           std::unique_ptr<sinks::LogRecord> structured) {
    if (items.empty() && !structured) return;
    QueueRecord rec;
    rec.level = level;
    rec.items = std::move(items);
    rec.structured = std::move(structured);
    state_->TryEnqueueRecord(std::move(rec));
}

void AsyncLogger::LogStructured(Level level, std::unique_ptr<sinks::LogRecord> record) {
    if (!record) return;
    QueueRecord rec;
    rec.level = level;
    rec.structured = std::move(record);
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
