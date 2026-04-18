#include <ulog/tracing_hook.hpp>

#include <atomic>

namespace ulog {

namespace {

struct HookSlot {
    std::atomic<TracingHook> fn{nullptr};
    std::atomic<void*> ctx{nullptr};
};

HookSlot& Slot() noexcept {
    static HookSlot s;
    return s;
}

}  // namespace

void SetTracingHook(TracingHook hook, void* user_ctx) noexcept {
    auto& s = Slot();
    // Write ctx first, then fn — the reader sees the matching pair.
    s.ctx.store(user_ctx, std::memory_order_relaxed);
    s.fn.store(hook, std::memory_order_release);
}

namespace impl {

void DispatchTracingHook(TagSink& sink) {
    auto& s = Slot();
    auto fn = s.fn.load(std::memory_order_acquire);
    if (!fn) return;
    auto* ctx = s.ctx.load(std::memory_order_relaxed);
    fn(sink, ctx);
}

}  // namespace impl

}  // namespace ulog
