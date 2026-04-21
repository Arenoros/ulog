#include <ulog/log.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>

#include <boost/smart_ptr/atomic_shared_ptr.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

#include <ulog/impl/logger_base.hpp>
#include <ulog/null_logger.hpp>
#include <ulog/dynamic_debug.hpp>

#include "ulog/impl/rate_limit.hpp"

namespace ulog {
    namespace {
        auto& NonOwningDefaultLoggerInternal() noexcept {
            // Initial logger should be Null logger as non-service utils
            // may use userver's universal without logger. They should not suffer from any
            // logger at all.
            static std::atomic<impl::LoggerBase*> default_logger_ptr{ &GetNullLogger() };
            return default_logger_ptr;
        }

        constexpr bool IsPowerOf2(uint64_t n) { return (n & (n - 1)) == 0; }

    }


    namespace impl {

        void SetDefaultLoggerRef(LoggerRef logger) noexcept { NonOwningDefaultLoggerInternal() = &logger; }

        bool has_background_threads_which_can_log{ false };

    }  // namespace impl

    void SetNullDefaultLogger() noexcept {
        impl::SetDefaultLoggerRef(GetNullLogger());
    }

    LoggerRef GetDefaultLogger() noexcept { return *NonOwningDefaultLoggerInternal().load(); }

    DefaultLoggerGuard::DefaultLoggerGuard(LoggerPtr new_default_logger) noexcept
        : logger_prev_(GetDefaultLogger()),
        level_prev_(GetDefaultLoggerLevel()),
        logger_new_(std::move(new_default_logger)) {
        assert(logger_new_);
        impl::SetDefaultLoggerRef(*logger_new_);
    }

    DefaultLoggerGuard::~DefaultLoggerGuard() {
        assert(
            !impl::has_background_threads_which_can_log,
            "DefaultLoggerGuard with a new logger should outlive the coroutine "
            "engine, because otherwise it could be in use right now, when the "
            "~DefaultLoggerGuard() is called and the logger is destroyed. "
            "Construct the DefaultLoggerGuard before calling engine::RunStandalone; "
            "in tests use the utest::DefaultLoggerFixture."
        );

        impl::SetDefaultLoggerRef(logger_prev_);
        SetDefaultLoggerLevel(level_prev_);
    }

    DefaultLoggerLevelScope::DefaultLoggerLevelScope(Level level)
        : logger_(GetDefaultLogger()),
        level_initial_(GetLoggerLevel(logger_)) {
        SetLoggerLevel(logger_, level);
    }

    DefaultLoggerLevelScope::~DefaultLoggerLevelScope() { SetLoggerLevel(logger_, level_initial_); }

    void SetDefaultLoggerLevel(Level level) { GetDefaultLogger().SetLevel(level); }

    void SetLoggerLevel(LoggerRef logger, Level level) { logger.SetLevel(level); }

    Level GetDefaultLoggerLevel() noexcept {
        static_assert(noexcept(GetDefaultLogger().GetLevel()));
        return GetDefaultLogger().GetLevel();
    }

    bool ShouldLog(Level level) noexcept {
        static_assert(noexcept(GetDefaultLogger().ShouldLog(level)));
        return GetDefaultLogger().ShouldLog(level);
    }

    bool LoggerShouldLog(LoggerRef logger, Level level) noexcept {
        static_assert(noexcept(logger.ShouldLog(level)));
        return logger.ShouldLog(level);
    }

    bool LoggerShouldLog(const LoggerPtr& logger, Level level) noexcept {
        return logger && LoggerShouldLog(*logger, level);
    }

    Level GetLoggerLevel(LoggerRef logger) noexcept {
        static_assert(noexcept(logger.GetLevel()));
        return logger.GetLevel();
    }

    void LogFlush() { GetDefaultLogger().Flush(); }

    void LogFlush(LoggerRef logger) { logger.Flush(); }


    // ---------------- RateLimiter ----------------

    namespace impl {

        namespace {

            bool NextCountShouldLog(std::uint64_t count) noexcept {
                // Emit for count == 1 and every power-of-two thereafter.
                return count == 1 || (count > 0 && (count & (count - 1)) == 0);
            }

        }  // namespace

        /// Process-wide running total, aggregated across every rate-limited
        /// site. Producers only increment on drops — zero overhead on the
        /// common (emit) path, one relaxed atomic add on the drop path.
        std::atomic<std::uint64_t> g_rate_limit_dropped_total{ 0 };

        /// Optional per-drop callback — nullptr by default. Producers load with
        /// acquire so the handler, once observed non-null, sees any stores the
        /// installer made before `SetRateLimitDropHandler`.
        std::atomic<RateLimitDropHandler> g_rate_limit_drop_handler{ nullptr };

        RateLimiter::RateLimiter(RateLimitData& data) noexcept {
            try {
                if (!impl::IsLogLimitedEnabled()) {
                    return;
                }

                const auto reset_interval = impl::GetLogLimitedInterval();
                const auto now = std::chrono::steady_clock::now();

                if (now - data.last_reset_time >= reset_interval) {
                    data.count_since_reset = 0;
                    data.last_reset_time = now;
                }

                if (IsPowerOf2(++data.count_since_reset)) {
                    // log the current message together with the dropped count
                    dropped_count_ = std::exchange(data.dropped_count, 0);
                } else {
                    // drop the current message
                    ++data.dropped_count;
                    should_log_ = false;
                }
            } catch (const std::exception& e) {
                assert(false, e.what());
                should_log_ = false;
            }
        }
        LogHelper& operator<<(LogHelper& lh, const RateLimiter& rl) noexcept {
            if (rl.dropped_count_ != 0) {
                lh << "[" << rl.dropped_count_ << " logs dropped] ";
            }
            return lh;
        }

        StaticLogEntry::StaticLogEntry(const char* path, int line) noexcept {
            static_assert(sizeof(LogEntryContent) == sizeof(content_));
            static_assert(sizeof(LogEntryContent) == sizeof(content_));
            // static_assert(std::is_trivially_destructible_v<LogEntryContent>);
            auto* item = new (&content_) LogEntryContent(path, line);
            RegisterLogLocation(*item);
        }

        bool StaticLogEntry::ShouldNotLog(LoggerRef logger, Level level) const noexcept {
            const auto& content = reinterpret_cast<const LogEntryContent&>(content_);
            const auto state = content.state.load();
            const bool force_disabled = level < state.force_disabled_level_plus_one;
            const bool force_enabled = level >= state.force_enabled_level && level != Level::kNone;
            return (!LoggerShouldLog(logger, level) || force_disabled) && !force_enabled;
        }

        bool StaticLogEntry::ShouldNotLog(const LoggerPtr& logger, Level level) const noexcept {
            return !logger || ShouldNotLog(*logger, level);
        }

    }  // namespace impl


    std::uint64_t GetRateLimitDroppedTotal() noexcept {
        return impl::g_rate_limit_dropped_total.load(std::memory_order_relaxed);
    }

    void ResetRateLimitStats() noexcept {
        impl::g_rate_limit_dropped_total.store(0, std::memory_order_relaxed);
    }

    void SetRateLimitDropHandler(RateLimitDropHandler handler) noexcept {
        impl::g_rate_limit_drop_handler.store(handler, std::memory_order_release);
    }

}  // namespace ulog
