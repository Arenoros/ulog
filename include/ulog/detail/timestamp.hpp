#pragma once

/// @file ulog/detail/timestamp.hpp
/// @brief Cross-platform timestamp formatting to TSKV-style strings.

#include <chrono>
#include <string>

namespace ulog::detail {

/// Formats time_point as "YYYY-MM-DDThh:mm:ss.uuuuuu+0000" (UTC, microseconds).
/// Thread-safe on all platforms (uses gmtime_s/gmtime_r).
std::string FormatTimestampUtc(std::chrono::system_clock::time_point tp);

/// Appends the formatted timestamp to a sink that supports `append(const char*, size_t)` or `+= std::string_view`.
template <typename Sink>
void AppendTimestampUtc(Sink& sink, std::chrono::system_clock::time_point tp) {
    const auto s = FormatTimestampUtc(tp);
    sink += std::string_view(s);
}

}  // namespace ulog::detail
