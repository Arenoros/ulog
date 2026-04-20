#pragma once

/// @file ulog/impl/formatters/text_item.hpp
/// @brief Textual log-record payload produced by TSKV/LTSV/RAW/JSON formatters.

#include <atomic>
#include <cstddef>
#include <memory>

#include <ulog/detail/small_string.hpp>
#include <ulog/impl/formatters/base.hpp>

namespace ulog::impl::formatters {

/// Textual record: a finalized single-line string (including trailing '\n').
///
/// Inline capacity 512 B fits the vast majority of records (timestamp +
/// level + module + text + a handful of tags). Larger records spill to a
/// heap std::string inside SmallString. With a 512 B inline buffer the
/// whole TextLogItem object fits in ~540 B.
///
/// Lifecycle: allocated by `TextLogItemPool::Acquire()` (producer side),
/// populated by the formatter, moved through the async queue, consumed
/// on the worker thread, and returned to the pool via
/// `TextLogItemPool::Release()` (invoked by the custom deleter). Steady
/// state after warmup: zero heap allocations per record.
struct TextLogItem final : LoggerItemBase {
    detail::SmallString<512> payload;
};

/// Global cross-thread pool of `TextLogItem` objects. One MPMC
/// moodycamel queue holding raw pointers to idle items. The queue lives
/// for the lifetime of the process (Meyers singleton); items outstanding
/// at process exit are leaked to the OS along with the rest of the
/// heap — same behaviour as any other allocator arena.
///
/// Sized via `kMaxPooled` to cap peak memory (1024 × ~540 B ≈ 540 KB).
/// Overflow spills to `delete` instead of growing the pool unboundedly.
class TextLogItemPool final {
public:
    TextLogItemPool(const TextLogItemPool&) = delete;
    TextLogItemPool& operator=(const TextLogItemPool&) = delete;

    /// Singleton accessor. First call constructs; subsequent calls
    /// return the same instance. Intentionally leaked on process exit.
    static TextLogItemPool& Instance() noexcept;

    /// Return an item ready to be filled — either recycled from the
    /// free list or newly heap-allocated. `payload` is cleared so the
    /// caller sees an empty buffer.
    TextLogItem* Acquire();

    /// Return an item to the pool. Discards (deletes) if the pool is
    /// already at `kMaxPooled` to bound steady-state memory. Never
    /// throws. `p` may be `nullptr` — ignored.
    void Release(TextLogItem* p) noexcept;

private:
    TextLogItemPool() = default;
    ~TextLogItemPool();  // drains pool; called if singleton ever destroyed
    struct State;
    std::unique_ptr<State> state_;
    static constexpr std::size_t kMaxPooled = 1024;
};

/// Concrete-type deleter for `std::unique_ptr<TextLogItem, …>` —
/// returns the item to the pool. Mirrors `LoggerItemDeleter` but
/// preserves the static `TextLogItem` type so formatters can access
/// `item_->payload` without casting.
struct TextLogItemPoolDeleter {
    void operator()(TextLogItem* p) const noexcept {
        TextLogItemPool::Instance().Release(p);
    }
};

using PooledTextLogItemPtr = std::unique_ptr<TextLogItem, TextLogItemPoolDeleter>;

}  // namespace ulog::impl::formatters
