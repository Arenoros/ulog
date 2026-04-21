#include <ulog/detail/timestamp.hpp>
#include <fmt/chrono.h>
#include <fmt/compile.h>
#include <fmt/format.h>

#include "ulog/format.hpp"

//
//namespace ulog::detail {
//
//    namespace {
//
//        std::string FormatIso8601(std::chrono::system_clock::time_point tp,
//            int frac_digits /* 0, 3, or 6 */) {
//            using namespace std::chrono;
//            const auto secs = time_point_cast<seconds>(tp);
//            const auto tt = system_clock::to_time_t(secs);
//
//            std::tm tm_utc{};
//#if defined(_WIN32)
//            ::gmtime_s(&tm_utc, &tt);
//#else
//            ::gmtime_r(&tt, &tm_utc);
//#endif
//
//            char buf[40];
//            int n = 0;
//            if (frac_digits == 6) {
//                const auto micros = duration_cast<microseconds>(tp - secs).count();
//                n = std::snprintf(
//                    buf, sizeof(buf),
//                    "%04d-%02d-%02dT%02d:%02d:%02d.%06lld+0000",
//                    tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
//                    tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
//                    static_cast<long long>(micros));
//            } else if (frac_digits == 3) {
//                const auto millis = duration_cast<milliseconds>(tp - secs).count();
//                n = std::snprintf(
//                    buf, sizeof(buf),
//                    "%04d-%02d-%02dT%02d:%02d:%02d.%03lld+0000",
//                    tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
//                    tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
//                    static_cast<long long>(millis));
//            } else {
//                n = std::snprintf(
//                    buf, sizeof(buf),
//                    "%04d-%02d-%02dT%02d:%02d:%02d+0000",
//                    tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
//                    tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
//            }
//            if (n <= 0) return {};
//            return std::string(buf, static_cast<std::size_t>(n));
//        }
//
//        template <typename Duration>
//        std::string FormatEpoch(std::chrono::system_clock::time_point tp) {
//            using namespace std::chrono;
//            const auto v = duration_cast<Duration>(tp.time_since_epoch()).count();
//            char buf[32];
//            const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
//            if (n <= 0) return {};
//            return std::string(buf, static_cast<std::size_t>(n));
//        }
//
//    }  // namespace
//
//    std::string FormatTimestamp(std::chrono::system_clock::time_point tp,
//        TimestampFormat fmt) {
//        using namespace std::chrono;
//        switch (fmt) {
//        case TimestampFormat::kIso8601Micro: return FormatIso8601(tp, 6);
//        case TimestampFormat::kIso8601Milli: return FormatIso8601(tp, 3);
//        case TimestampFormat::kIso8601Sec:   return FormatIso8601(tp, 0);
//        case TimestampFormat::kEpochNano:    return FormatEpoch<nanoseconds>(tp);
//        case TimestampFormat::kEpochMicro:   return FormatEpoch<microseconds>(tp);
//        case TimestampFormat::kEpochMilli:   return FormatEpoch<milliseconds>(tp);
//        case TimestampFormat::kEpochSec:     return FormatEpoch<seconds>(tp);
//        }
//        return FormatIso8601(tp, 6);
//    }
//
//}  // namespace ulog::detail

// detail/posix_compat.hpp
#if defined(_WIN32)
inline std::tm* gmtime_r(const std::time_t* t, std::tm* result) noexcept {
    return (::gmtime_s(result, t) == 0) ? result : nullptr;
}
inline std::tm* localtime_r(const std::time_t* t, std::tm* result) noexcept {
    return (::localtime_s(result, t) == 0) ? result : nullptr;
}
#endif

namespace ulog::detail {
    namespace {

        using TimePoint = std::chrono::system_clock::time_point;

        template<TimestampFormat Format>
        struct FormatInfo;

        template<>
        struct FormatInfo<TimestampFormat::kEpochNano> {
            using type = std::chrono::nanoseconds;
            static constexpr std::string_view kTimeTemplate = "12345678901234567890";
        };

        template<>
        struct FormatInfo<TimestampFormat::kEpochMicro> {
            using type = std::chrono::microseconds;
            static constexpr std::string_view kTimeTemplate = "12345678901234567890";
        };

        template<>
        struct FormatInfo<TimestampFormat::kEpochMilli> {
            using type = std::chrono::milliseconds;
            static constexpr std::string_view kTimeTemplate = "12345678901234567890";
        };

        template<>
        struct FormatInfo<TimestampFormat::kEpochSec> {
            using type = std::chrono::seconds;
            static constexpr std::string_view kTimeTemplate = "12345678901234567890";
        };

        template<>
        struct FormatInfo<TimestampFormat::kIso8601Micro> {
            using type = std::chrono::microseconds;
            static constexpr std::string_view kTimeTemplate = "0000-00-00T00:00:00.000000";
        };

        template<>
        struct FormatInfo<TimestampFormat::kIso8601Milli> {
            using type = std::chrono::milliseconds;
            static constexpr std::string_view kTimeTemplate = "0000-00-00T00:00:00.000";
        };

        template<>
        struct FormatInfo<TimestampFormat::kIso8601Sec> {
            using type = std::chrono::seconds;
            static constexpr std::string_view kTimeTemplate = "0000-00-00T00:00:00";
        };

        template<TimestampFormat Format>
        using StdChronoType = FormatInfo<Format>::type;

        template<TimestampFormat Format>
        using StdTimePoint = std::chrono::time_point<TimePoint::clock, StdChronoType<Format>>;

        template<TimestampFormat Format>
        static auto GetRounded(TimePoint now) {
            return std::chrono::time_point_cast<StdChronoType<Format>>(now);
        }
        template<TimestampFormat Format>
        static consteval size_t TemplateSize() {
            return FormatInfo<Format>::kTimeTemplate.size();
        }


        template<TimestampFormat Format>
        struct myTimeString {
            char data[TemplateSize<Format>()]{};
            size_t size = TemplateSize<Format>();
            std::string_view ToStringView() const noexcept { return { data, size }; }
        };


        template<TimestampFormat Format>
        struct myCachedTime final {
            StdTimePoint<Format> time{};
            myTimeString<Format> string{};
        };

        template<TimestampFormat Format>
        static std::string_view GetCurrentTimeStringEpoch(TimePoint now) noexcept {
            static_assert(Format == TimestampFormat::kEpochNano || Format == TimestampFormat::kEpochMicro || Format == TimestampFormat::kEpochMilli || Format == TimestampFormat::kEpochSec);
            static thread_local myCachedTime<Format> cached_time{};
            const auto rounded_now = GetRounded<Format>(now);
            if (rounded_now != cached_time.time) {
                auto res = fmt::format_to(cached_time.string.data, FMT_COMPILE("{}"), rounded_now.time_since_epoch().count());
                cached_time.time = rounded_now;
                cached_time.string.size = res - cached_time.string.data;
            }
            return cached_time.string.ToStringView();
        }

        template <std::tm* (*TimeConverter)(const std::time_t*, std::tm*)>
        static std::string_view GetCurrentTimeStringIso8601Sec(TimePoint now) noexcept {
            static thread_local myCachedTime<TimestampFormat::kIso8601Sec> cached_time{};
            const auto rounded_now = GetRounded<TimestampFormat::kIso8601Sec>(now);

            if (rounded_now != cached_time.time) {
                std::tm tm{};
                std::time_t now_t = std::chrono::system_clock::to_time_t(now);
                if (TimeConverter(&now_t, &tm) != nullptr) {
                    fmt::format_to(cached_time.string.data, FMT_COMPILE("{:%FT%T}"), tm);
                    cached_time.time = rounded_now;
                } else {
                    FMT_ASSERT(false, strerror(errno));

                    // ... keep using the old cached time
                }
            }
            return cached_time.string.ToStringView();
        }

        template <std::tm* (*TimeConverter)(const std::time_t*, std::tm*)>
        static std::string_view GetCurrentTimeStringIso8601Milli(TimePoint now) noexcept {
            static thread_local myCachedTime<TimestampFormat::kIso8601Milli> cached_time{};
            const auto rounded_now = GetRounded<TimestampFormat::kIso8601Milli>(now);

            if (rounded_now != cached_time.time) {
                std::tm tm{};
                std::time_t now_t = std::chrono::system_clock::to_time_t(now);
                if (TimeConverter(&now_t, &tm) != nullptr) {
                    fmt::format_to(cached_time.string.data, FMT_COMPILE("{:%FT%T}.{:03}"), tm, FractionalMilliseconds(now));
                    cached_time.time = rounded_now;
                } else {
                    FMT_ASSERT(false, strerror(errno));

                    // ... keep using the old cached time
                }
            }
            return cached_time.string.ToStringView();
        }
        template <std::tm* (*TimeConverter)(const std::time_t*, std::tm*)>
        static std::string_view GetCurrentTimeStringIso8601Micro(TimePoint now) noexcept {
            static thread_local myCachedTime<TimestampFormat::kIso8601Micro> cached_time{};
            const auto rounded_now = GetRounded<TimestampFormat::kIso8601Micro>(now);

            if (rounded_now != cached_time.time) {
                std::tm tm{};
                std::time_t now_t = std::chrono::system_clock::to_time_t(now);
                if (TimeConverter(&now_t, &tm) != nullptr) {
                    fmt::format_to(cached_time.string.data, FMT_COMPILE("{:%FT%T}.{:06}"), tm, FractionalMicroseconds(now));
                    cached_time.time = rounded_now;
                } else {
                    FMT_ASSERT(false, strerror(errno));

                    // ... keep using the old cached time
                }
            }
            return cached_time.string.ToStringView();
        }
    }


    std::chrono::microseconds::rep FractionalMicroseconds(std::chrono::system_clock::time_point time) noexcept {
        return std::chrono::time_point_cast<std::chrono::microseconds>(time).time_since_epoch().count() % 1'000'000;
    }
    std::chrono::microseconds::rep FractionalMilliseconds(std::chrono::system_clock::time_point time) noexcept {
        return std::chrono::time_point_cast<std::chrono::milliseconds>(time).time_since_epoch().count() % 1'000;
    }
    std::string_view FormatTimestamp(std::chrono::system_clock::time_point tp, TimestampFormat fmt) {
        using namespace std::chrono;
        switch (fmt) {
        case TimestampFormat::kIso8601Micro:      return GetCurrentTimeStringIso8601Micro<gmtime_r>(tp);
        case TimestampFormat::kIso8601Milli:      return GetCurrentTimeStringIso8601Milli<gmtime_r>(tp);
        case TimestampFormat::kIso8601Sec:        return GetCurrentTimeStringIso8601Sec<gmtime_r>(tp);
        case TimestampFormat::kLocalIso8601Micro: return GetCurrentTimeStringIso8601Micro<localtime_r>(tp);
        case TimestampFormat::kLocalIso8601Milli: return GetCurrentTimeStringIso8601Milli<localtime_r>(tp);
        case TimestampFormat::kLocalIso8601Sec:   return GetCurrentTimeStringIso8601Sec<localtime_r>(tp);
        case TimestampFormat::kEpochNano:         return GetCurrentTimeStringEpoch<TimestampFormat::kEpochNano>(tp);
        case TimestampFormat::kEpochMicro:        return GetCurrentTimeStringEpoch<TimestampFormat::kEpochMicro>(tp);
        case TimestampFormat::kEpochMilli:        return GetCurrentTimeStringEpoch<TimestampFormat::kEpochMilli>(tp);
        case TimestampFormat::kEpochSec:          return GetCurrentTimeStringEpoch<TimestampFormat::kEpochSec>(tp);
        }
        return GetCurrentTimeStringIso8601Milli<gmtime_r>(tp);
    }

}  // namespace ulog::impl

