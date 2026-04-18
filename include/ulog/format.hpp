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
};

/// Parse Format from string. Throws std::runtime_error if unknown.
Format FormatFromString(std::string_view format_str);

/// Lowercase string representation.
std::string_view ToString(Format format) noexcept;

}  // namespace ulog
