#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ulog {

/// Text format a route encodes its Records in.
enum class Format : std::uint8_t {
  /// `tskv` prefix, local timestamp, level, module, fields, and text as `key=value` pairs.
  kTskv = 0,
  /// The TSKV content without the prefix, as `key:value` pairs.
  kLtsv = 1,
  /// `tskv` prefix, fields, and text, without timestamp, level, or module.
  kRaw = 2,
  /// Compact JSON object per line with a UTC timestamp.
  kJson = 3,
  /// JSON with the YaDeploy names `@timestamp`, `levelStr`, and `message`.
  kJsonYaDeploy = 4,
};

namespace detail {

inline constexpr std::array<std::string_view, 5> kFormatNames{"tskv", "ltsv", "raw", "json",
                                                              "json_yadeploy"};

}  // namespace detail

/// Canonical name, such as "tskv"; empty for a value that is not a Format enumerator.
[[nodiscard]] constexpr std::string_view ToString(Format format) noexcept {
  const auto index = static_cast<std::size_t>(format);
  return index < detail::kFormatNames.size() ? detail::kFormatNames[index] : std::string_view{};
}

struct FormatParseResult final {
  std::optional<Format> format{};

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return format.has_value(); }
  /// Correction for a rejected name; names every accepted value.
  [[nodiscard]] constexpr std::string_view HowToFix() const noexcept {
    return format ? std::string_view{}
                  : std::string_view{
                        "Use one of the format names tskv, ltsv, raw, json, or json_yadeploy, in "
                        "lowercase."};
  }
};

/// Parses one of the five canonical format names exactly; letter case and spacing must match.
[[nodiscard]] constexpr FormatParseResult ParseFormat(std::string_view name) noexcept {
  for (std::size_t index = 0; index < detail::kFormatNames.size(); ++index) {
    if (name == detail::kFormatNames[index]) {
      return {.format = static_cast<Format>(index)};
    }
  }
  return {};
}

}  // namespace ulog
