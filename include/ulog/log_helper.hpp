#pragma once

/// @file ulog/log_helper.hpp
/// @brief Stream-like builder for log records. Produced by LOG_* macros.

#include <exception>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>

#include <fmt/format.h>

#include <ulog/fwd.hpp>
#include <ulog/level.hpp>
#include <ulog/log_extra.hpp>

namespace ulog {

namespace impl {
class RateLimiter;
class TagWriter;
namespace formatters {
class Base;
struct BaseDeleter;
using BasePtr = std::unique_ptr<Base, BaseDeleter>;
}  // namespace formatters
}  // namespace impl

/// Source location captured by LOG_* macros.
struct LogRecordLocation {
    const char* file{nullptr};
    int line{0};
    const char* function{nullptr};
};

/// Hex-formatted value helper.
struct Hex {
    std::uintptr_t value;
};

/// Short-hex-formatted value helper (no leading zeros).
struct HexShort {
    std::uintptr_t value;
};

/// Quoted-string helper. Value will be output wrapped in double quotes.
struct Quoted {
    std::string_view value;
};

/// Stream-like builder. Assembled by LOG_* macros, flushed in destructor.
class LogHelper final {
public:
    /// RAII tag: when set, destructor does not emit (useful for `if(ShouldLog)`).
    struct NoLog {};

    LogHelper(LoggerRef logger, Level level, LogRecordLocation location) noexcept;
    LogHelper(LoggerRef logger, Level level, LogRecordLocation location, NoLog) noexcept;

    /// Overload that keeps the logger alive for the helper's lifetime.
    /// Used by the default-logger macro expansion so the record survives a
    /// concurrent SetDefaultLogger() swap.
    LogHelper(LoggerPtr logger, Level level, LogRecordLocation location) noexcept;

    ~LogHelper();

    LogHelper(const LogHelper&) = delete;
    LogHelper(LogHelper&&) = delete;
    LogHelper& operator=(const LogHelper&) = delete;
    LogHelper& operator=(LogHelper&&) = delete;

    /// Stream operator for arbitrary types. Primary entry point for LOG_* macros.
    template <typename T>
    LogHelper& operator<<(const T& value) & {
        Put(value);
        return *this;
    }
    template <typename T>
    LogHelper&& operator<<(const T& value) && {
        Put(value);
        return std::move(*this);
    }

    /// Stream LogExtra: merges its fields into the record.
    LogHelper& operator<<(const LogExtra& extra) &;
    LogHelper&& operator<<(const LogExtra& extra) &&;

    /// Stream helpers.
    LogHelper& operator<<(Hex v) &;
    LogHelper&& operator<<(Hex v) && { return std::move(*this << v); }
    LogHelper& operator<<(HexShort v) &;
    LogHelper&& operator<<(HexShort v) && { return std::move(*this << v); }
    LogHelper& operator<<(Quoted v) &;
    LogHelper&& operator<<(Quoted v) && { return std::move(*this << v); }

    /// Stream for the rate limiter used by LOG_LIMITED_*.
    LogHelper& operator<<(const impl::RateLimiter& rl) &;
    LogHelper&& operator<<(const impl::RateLimiter& rl) && { return std::move(*this << rl); }

    /// Captures an exception (message + type) into the record.
    LogHelper& WithException(const std::exception& ex) &;
    LogHelper&& WithException(const std::exception& ex) && { return std::move(WithException(ex)); }

    /// Access low-level tag writer.
    impl::TagWriter& GetTagWriter() noexcept;

    /// True if the helper will emit when destroyed.
    bool IsActive() const noexcept;

private:
    void Put(std::string_view sv);
    void Put(const char* s);
    void Put(const std::string& s) { Put(std::string_view(s)); }

    template <typename T>
    std::enable_if_t<std::is_integral_v<T> || std::is_floating_point_v<T>> Put(T v) {
        PutFormatted(fmt::format("{}", v));
    }
    template <typename T>
    std::enable_if_t<std::is_enum_v<T>> Put(T v) {
        PutFormatted(fmt::format("{}", static_cast<std::underlying_type_t<T>>(v)));
    }
    void Put(bool v);
    void Put(char v);
    void PutFormatted(std::string s);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ulog
