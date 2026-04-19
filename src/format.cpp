#include <ulog/format.hpp>

#include <array>
#include <stdexcept>
#include <string_view>
#include <utility>

#include <fmt/format.h>

namespace ulog {

namespace {

struct Entry {
    std::string_view name;
    Format format;
};

constexpr std::array<Entry, 6> kTable = {{
    {"tskv", Format::kTskv},
    {"ltsv", Format::kLtsv},
    {"raw", Format::kRaw},
    {"json", Format::kJson},
    {"json_yadeploy", Format::kJsonYaDeploy},
    {"otlp_json", Format::kOtlpJson},
}};

}  // namespace

Format FormatFromString(std::string_view format_str) {
    for (const auto& e : kTable) {
        if (e.name == format_str) return e.format;
    }
    throw std::runtime_error(fmt::format(
        "Unknown log format '{}' (expected one of tskv/ltsv/raw/json/json_yadeploy)", format_str));
}

std::string_view ToString(Format format) noexcept {
    for (const auto& e : kTable) {
        if (e.format == format) return e.name;
    }
    return "unknown";
}

}  // namespace ulog
