#pragma once

/// @file ulog/detail/timestamp.hpp
/// @brief Cross-platform timestamp formatting to TSKV-style strings.

#include <chrono>
#include <string>
#include <string_view>

#include <ulog/format.hpp>

namespace ulog::detail {

/// Formats `tp` according to `fmt`. Thread-safe on all platforms.
/// See `ulog::TimestampFormat` for the list of supported shapes.
std::string FormatTimestamp(std::chrono::system_clock::time_point tp,
                            TimestampFormat fmt);

/// Back-compat shim: `kIso8601Micro` (the pre-knob default shape).
inline std::string FormatTimestampUtc(std::chrono::system_clock::time_point tp) {
    return FormatTimestamp(tp, TimestampFormat::kIso8601Micro);
}

/// Appends the formatted timestamp to a sink that supports
/// `+= std::string_view`.
template <typename Sink>
void AppendTimestamp(Sink& sink, std::chrono::system_clock::time_point tp,
                     TimestampFormat fmt) {
    const auto s = FormatTimestamp(tp, fmt);
    sink += std::string_view(s);
}

template <typename Sink>
void AppendTimestampUtc(Sink& sink, std::chrono::system_clock::time_point tp) {
    AppendTimestamp(sink, tp, TimestampFormat::kIso8601Micro);
}

}  // namespace ulog::detail
