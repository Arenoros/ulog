#include <ulog/impl/formatters/text_item.hpp>

#include <atomic>

#include <concurrentqueue.h>

namespace ulog::impl::formatters {

void LoggerItemDeleter::operator()(LoggerItemBase* p) const noexcept {
    if (!p) return;
    // TextLogItem is the sole concrete subclass; static_cast is safe.
    // Dispatch on dynamic type if that invariant is relaxed later.
    TextLogItemPool::Instance().Release(static_cast<TextLogItem*>(p));
}

struct TextLogItemPool::State {
    moodycamel::ConcurrentQueue<TextLogItem*> free_list;
    std::atomic<std::size_t> pooled_count{0};
};

TextLogItemPool& TextLogItemPool::Instance() noexcept {
    // Meyers singleton — thread-safe under C++11+. Intentionally not
    // destroyed at process exit (static-storage destruction order vs.
    // user threads that might still be logging is hazardous). Any
    // still-pooled items leak to the OS, which reclaims the heap
    // wholesale on process teardown.
    static TextLogItemPool* instance = new TextLogItemPool();
    return *instance;
}

TextLogItemPool::~TextLogItemPool() {
    if (!state_) return;
    TextLogItem* p = nullptr;
    while (state_->free_list.try_dequeue(p)) {
        delete p;
    }
}

TextLogItem* TextLogItemPool::Acquire() {
    // Lazy state init. Deferred so the static singleton ctor itself
    // doesn't allocate (moodycamel's inner ring requires a heap buffer
    // which we prefer to pay for on first use rather than at program
    // load).
    if (!state_) state_ = std::make_unique<State>();
    TextLogItem* p = nullptr;
    if (state_->free_list.try_dequeue(p)) {
        state_->pooled_count.fetch_sub(1, std::memory_order_relaxed);
        // Reset caller-visible state — payload holds leftover bytes
        // from the previous record.
        p->payload.clear();
        return p;
    }
    return new TextLogItem();
}

void TextLogItemPool::Release(TextLogItem* p) noexcept {
    if (!p) return;
    if (!state_) {
        delete p;
        return;
    }
    // Cap the pool size — overflow deletes instead of unbounded growth.
    // `fetch_add` + conditional rollback races with itself under
    // concurrent Releases but the worst case is a handful of extra
    // items in the pool momentarily, which the next Acquire will drain.
    const auto prev = state_->pooled_count.fetch_add(1, std::memory_order_relaxed);
    if (prev >= kMaxPooled) {
        state_->pooled_count.fetch_sub(1, std::memory_order_relaxed);
        delete p;
        return;
    }
    // Clear on return rather than on Acquire to amortise the cost on
    // the consumer side where the thread is not on the hot producer
    // path.
    p->payload.clear();
    try {
        state_->free_list.enqueue(p);
    } catch (...) {
        // moodycamel's enqueue can throw on OOM. Undo the counter bump
        // and fall back to delete — steady-state OOM isn't something
        // we can fix here anyway.
        state_->pooled_count.fetch_sub(1, std::memory_order_relaxed);
        delete p;
    }
}

}  // namespace ulog::impl::formatters
