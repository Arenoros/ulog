#include <ulog/detail/timestamp.hpp>

#include <cstdio>
#include <ctime>

namespace ulog::detail {

std::string FormatTimestampUtc(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto micros = duration_cast<microseconds>(tp - secs).count();
    const auto tt = system_clock::to_time_t(secs);

    std::tm tm_utc{};
#if defined(_WIN32)
    ::gmtime_s(&tm_utc, &tt);
#else
    ::gmtime_r(&tt, &tm_utc);
#endif

    // Format: YYYY-MM-DDThh:mm:ss.uuuuuu+0000 — 31 chars + NUL.
    char buf[40];
    const int n = std::snprintf(
        buf, sizeof(buf),
        "%04d-%02d-%02dT%02d:%02d:%02d.%06lld+0000",
        tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
        tm_utc.tm_hour, tm_utc.tm_min, tm_utc.tm_sec,
        static_cast<long long>(micros));
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
}

}  // namespace ulog::detail
