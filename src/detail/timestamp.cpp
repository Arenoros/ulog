#include <ulog/detail/timestamp.hpp>

#include <cstdio>
#include <ctime>

namespace ulog::detail {

namespace {

std::string FormatIso8601(std::chrono::system_clock::time_point tp,
                          int frac_digits /* 0, 3, or 6 */) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto tt = system_clock::to_time_t(secs);

    std::tm tm_utc{};
#if defined(_WIN32)
    ::gmtime_s(&tm_utc, &tt);
#else
    ::gmtime_r(&tt, &tm_utc);
#endif

    char buf[40];
    int n = 0;
    if (frac_digits == 6) {
        const auto micros = duration_cast<microseconds>(tp - secs).count();
        n = std::snprintf(
            buf, sizeof(buf),
            "%04d-%02d-%02dT%02d:%02d:%02d.%06lld+0000",
            tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
            tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
            static_cast<long long>(micros));
    } else if (frac_digits == 3) {
        const auto millis = duration_cast<milliseconds>(tp - secs).count();
        n = std::snprintf(
            buf, sizeof(buf),
            "%04d-%02d-%02dT%02d:%02d:%02d.%03lld+0000",
            tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
            tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
            static_cast<long long>(millis));
    } else {
        n = std::snprintf(
            buf, sizeof(buf),
            "%04d-%02d-%02dT%02d:%02d:%02d+0000",
            tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
            tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec);
    }
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
}

template <typename Duration>
std::string FormatEpoch(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto v = duration_cast<Duration>(tp.time_since_epoch()).count();
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(v));
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
}

}  // namespace

std::string FormatTimestamp(std::chrono::system_clock::time_point tp,
                            TimestampFormat fmt) {
    using namespace std::chrono;
    switch (fmt) {
        case TimestampFormat::kIso8601Micro: return FormatIso8601(tp, 6);
        case TimestampFormat::kIso8601Milli: return FormatIso8601(tp, 3);
        case TimestampFormat::kIso8601Sec:   return FormatIso8601(tp, 0);
        case TimestampFormat::kEpochNano:    return FormatEpoch<nanoseconds>(tp);
        case TimestampFormat::kEpochMicro:   return FormatEpoch<microseconds>(tp);
        case TimestampFormat::kEpochMilli:   return FormatEpoch<milliseconds>(tp);
        case TimestampFormat::kEpochSec:     return FormatEpoch<seconds>(tp);
    }
    return FormatIso8601(tp, 6);
}

}  // namespace ulog::detail
