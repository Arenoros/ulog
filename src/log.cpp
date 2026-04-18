#include <ulog/log.hpp>

#include <atomic>
#include <chrono>
#include <mutex>

#include <ulog/dynamic_debug.hpp>
#include <ulog/impl/logger_base.hpp>
#include <ulog/null_logger.hpp>

namespace ulog {

namespace {

std::atomic<impl::LoggerBase*> g_default{nullptr};
// Holds ownership of the installed default logger so the raw pointer stays alive.
std::mutex g_default_mu;
LoggerPtr g_default_owner{};  // protected by g_default_mu

impl::LoggerBase& ResolveDefault() noexcept {
    auto* ptr = g_default.load(std::memory_order_acquire);
    if (ptr) return *ptr;
    return static_cast<impl::LoggerBase&>(GetNullLogger());
}

}  // namespace

LoggerRef GetDefaultLogger() noexcept { return ResolveDefault(); }

void SetDefaultLogger(LoggerPtr new_default_logger) noexcept {
    std::lock_guard lock(g_default_mu);
    g_default_owner = std::move(new_default_logger);
    g_default.store(g_default_owner.get(), std::memory_order_release);
}

void SetDefaultLoggerLevel(Level level) { ResolveDefault().SetLevel(level); }

Level GetDefaultLoggerLevel() noexcept { return ResolveDefault().GetLevel(); }

bool ShouldLog(Level level) noexcept { return ResolveDefault().ShouldLog(level); }

void SetLoggerLevel(LoggerRef logger, Level level) { logger.SetLevel(level); }

bool LoggerShouldLog(LoggerRef logger, Level level) noexcept { return logger.ShouldLog(level); }

bool LoggerShouldLog(const LoggerPtr& logger, Level level) noexcept {
    return logger && logger->ShouldLog(level);
}

Level GetLoggerLevel(LoggerRef logger) noexcept { return logger.GetLevel(); }

void LogFlush() { ResolveDefault().Flush(); }
void LogFlush(LoggerRef logger) { logger.Flush(); }

// ---------------- DefaultLoggerGuard ----------------

DefaultLoggerGuard::DefaultLoggerGuard(LoggerPtr new_default_logger) noexcept
    : prev_ptr_(), new_ptr_(std::move(new_default_logger)), prev_level_(Level::kInfo) {
    std::lock_guard lock(g_default_mu);
    prev_ptr_ = g_default_owner;
    prev_level_ = ResolveDefault().GetLevel();
    g_default_owner = new_ptr_;
    g_default.store(g_default_owner.get(), std::memory_order_release);
}

DefaultLoggerGuard::~DefaultLoggerGuard() {
    std::lock_guard lock(g_default_mu);
    g_default_owner = prev_ptr_;
    g_default.store(g_default_owner.get(), std::memory_order_release);
    if (g_default_owner) g_default_owner->SetLevel(prev_level_);
}

// ---------------- DefaultLoggerLevelScope ----------------

DefaultLoggerLevelScope::DefaultLoggerLevelScope(Level level)
    : logger_(ResolveDefault()), initial_(logger_.GetLevel()) {
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
