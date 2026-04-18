#include <ulog/log.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include <ulog/dynamic_debug.hpp>
#include <ulog/impl/logger_base.hpp>
#include <ulog/null_logger.hpp>

namespace ulog {

namespace {

/// Converts std::shared_ptr<T> into boost::shared_ptr<T> by stashing the std
/// owner in the boost deleter — refcount semantics are preserved and the
/// underlying object is released exactly once when both sides drop their
/// last reference.
template <typename T>
boost::shared_ptr<T> StdToBoost(std::shared_ptr<T> p) {
    if (!p) return {};
    T* raw = p.get();
    return boost::shared_ptr<T>(raw, [p = std::move(p)](T*) mutable {});
}

/// Inverse of StdToBoost: mirrors boost ownership into a std::shared_ptr.
template <typename T>
std::shared_ptr<T> BoostToStd(boost::shared_ptr<T> p) {
    if (!p) return {};
    T* raw = p.get();
    return std::shared_ptr<T>(raw, [p = std::move(p)](T*) mutable {});
}

/// boost::atomic_shared_ptr gives us lock-free load/store with correct
/// lifetime handling: readers always obtain a reference-counted snapshot,
/// so SetDefaultLogger racing with logging no longer dangles.
boost::atomic_shared_ptr<impl::LoggerBase>& DefaultSlot() noexcept {
    static boost::atomic_shared_ptr<impl::LoggerBase> slot;
    return slot;
}

/// Monotonically bumped on every SetDefaultLogger call. Per-thread caches
/// compare their cached value against this and refresh on mismatch — the
/// std↔boost shared_ptr conversion then runs O(1 per SetDefault) instead of
/// O(per LOG_*). Races are benign: stale TLS still returns a live,
/// ref-counted pointer (the previous logger), because DefaultSlot's
/// refcount keeps it alive for as long as anyone references it.
std::atomic<std::uint64_t> g_default_gen{0};

constexpr std::uint64_t kTlsStartGen = std::numeric_limits<std::uint64_t>::max();

LoggerPtr BuildDefaultPtr() {
    auto b = DefaultSlot().load();
    if (!b) return MakeNullLogger();
    return BoostToStd(std::move(b));
}

LoggerPtr& TlsDefault() noexcept {
    thread_local LoggerPtr cached;
    return cached;
}

std::uint64_t& TlsGeneration() noexcept {
    thread_local std::uint64_t gen = kTlsStartGen;
    return gen;
}

LoggerPtr LoadDefaultCached() noexcept {
    const auto gen = g_default_gen.load(std::memory_order_acquire);
    auto& tls = TlsDefault();
    auto& tls_gen = TlsGeneration();
    if (tls_gen != gen || !tls) {
        tls = BuildDefaultPtr();
        tls_gen = gen;
    }
    return tls;  // refcount++ on copy — no fresh control-block allocation
}

}  // namespace

LoggerRef GetDefaultLogger() noexcept {
    auto ptr = LoadDefaultCached();
    if (ptr) return *ptr;
    return GetNullLogger();
}

LoggerPtr GetDefaultLoggerPtr() noexcept { return LoadDefaultCached(); }

void SetDefaultLogger(LoggerPtr new_default_logger) noexcept {
    DefaultSlot().store(StdToBoost(std::move(new_default_logger)));
    g_default_gen.fetch_add(1, std::memory_order_release);
}

void SetDefaultLoggerLevel(Level level) { GetDefaultLogger().SetLevel(level); }

Level GetDefaultLoggerLevel() noexcept { return GetDefaultLogger().GetLevel(); }

bool ShouldLog(Level level) noexcept { return GetDefaultLogger().ShouldLog(level); }

void SetLoggerLevel(LoggerRef logger, Level level) { logger.SetLevel(level); }

bool LoggerShouldLog(LoggerRef logger, Level level) noexcept { return logger.ShouldLog(level); }

bool LoggerShouldLog(const LoggerPtr& logger, Level level) noexcept {
    return logger && logger->ShouldLog(level);
}

Level GetLoggerLevel(LoggerRef logger) noexcept { return logger.GetLevel(); }

void LogFlush() { GetDefaultLogger().Flush(); }
void LogFlush(LoggerRef logger) { logger.Flush(); }

// ---------------- DefaultLoggerGuard ----------------

DefaultLoggerGuard::DefaultLoggerGuard(LoggerPtr new_default_logger) noexcept
    : prev_ptr_(GetDefaultLoggerPtr()),
      new_ptr_(std::move(new_default_logger)),
      prev_level_(prev_ptr_ ? prev_ptr_->GetLevel() : Level::kInfo) {
    SetDefaultLogger(new_ptr_);
}

DefaultLoggerGuard::~DefaultLoggerGuard() {
    SetDefaultLogger(prev_ptr_);
    if (prev_ptr_) prev_ptr_->SetLevel(prev_level_);
}

// ---------------- DefaultLoggerLevelScope ----------------

DefaultLoggerLevelScope::DefaultLoggerLevelScope(Level level)
    : logger_(GetDefaultLogger()), initial_(logger_.GetLevel()) {
    logger_.SetLevel(level);
}
DefaultLoggerLevelScope::~DefaultLoggerLevelScope() { logger_.SetLevel(initial_); }

// ---------------- RateLimiter ----------------

namespace impl {

namespace {

bool NextCountShouldLog(std::uint64_t count) noexcept {
    // Emit for count == 1 and every power-of-two thereafter.
    return count == 1 || (count > 0 && (count & (count - 1)) == 0);
}

}  // namespace

RateLimiter::RateLimiter(RateLimitData& data) noexcept {
    const auto now = std::chrono::steady_clock::now();
    if (now - data.last_reset_time >= std::chrono::seconds(1)) {
        data.last_reset_time = now;
        data.count_since_reset = 0;
        data.dropped_count = 0;
    }
    ++data.count_since_reset;
    should_log_ = NextCountShouldLog(data.count_since_reset);
    if (!should_log_) ++data.dropped_count;
    dropped_count_ = data.dropped_count;
}

// ---------------- StaticLogEntry ----------------

namespace {
bool ShouldNotLogFor(const char* path, int line, bool logger_allows) noexcept {
    const auto state = impl::LookupDynamicDebugLog(path, line);
    switch (state) {
        case DynamicDebugState::kForceEnabled:  return false;
        case DynamicDebugState::kForceDisabled: return true;
        case DynamicDebugState::kDefault:       return !logger_allows;
    }
    return !logger_allows;
}
}  // namespace

bool StaticLogEntry::ShouldNotLog(LoggerRef logger, Level level) const noexcept {
    return ShouldNotLogFor(path_, line_, logger.ShouldLog(level));
}

bool StaticLogEntry::ShouldNotLog(const LoggerPtr& logger, Level level) const noexcept {
    const bool allows = logger && logger->ShouldLog(level);
    return ShouldNotLogFor(path_, line_, allows);
}

}  // namespace impl

}  // namespace ulog
