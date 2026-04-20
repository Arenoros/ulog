#pragma once

/// @file ulog/format.hpp
/// @brief Text-based log formats.

#include <string_view>

namespace ulog {

/// Text-based log output formats.
enum class Format {
    kTskv,          ///< key=value, tab-separated (Yandex TSKV)
    kLtsv,          ///< Label-separated (label:value, tab-separated)
    kRaw,           ///< TSKV without timestamp/default fields
    kJson,          ///< JSON object per line
    kJsonYaDeploy,  ///< JSON variant for deploy systems
    kOtlpJson,      ///< OTLP LogRecord JSON schema (one record per line)
};

/// Parse Format from string. Throws std::runtime_error if unknown.
Format FormatFromString(std::string_view format_str);

/// Lowercase string representation.
std::string_view ToString(Format format) noexcept;

/// How to render the `timestamp` field produced by the built-in text
/// formatters (TSKV, LTSV, JSON/JsonYaDeploy). `kOtlpJson` emits
/// `timeUnixNano` per OTLP spec and ignores this setting.
enum class TimestampFormat {
    kIso8601Micro,  ///< `YYYY-MM-DDThh:mm:ss.uuuuuu+0000` (default)
    kIso8601Milli,  ///< `YYYY-MM-DDThh:mm:ss.mmm+0000`
    kIso8601Sec,    ///< `YYYY-MM-DDThh:mm:ss+0000`
    kEpochNano,     ///< decimal nanoseconds since Unix epoch
    kEpochMicro,    ///< decimal microseconds since Unix epoch
    kEpochMilli,    ///< decimal milliseconds since Unix epoch
    kEpochSec,      ///< decimal seconds since Unix epoch
};

/// Parse TimestampFormat from lowercase string (e.g. "iso8601_micro",
/// "epoch_nano"). Throws std::runtime_error if unknown.
TimestampFormat TimestampFormatFromString(std::string_view s);

/// Lowercase string representation.
std::string_view ToString(TimestampFormat fmt) noexcept;

}  // namespace ulog
