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

namespace {

struct TsEntry {
    std::string_view name;
    TimestampFormat fmt;
};

constexpr std::array<TsEntry, 7> kTsTable = {{
    {"iso8601_micro", TimestampFormat::kIso8601Micro},
    {"iso8601_milli", TimestampFormat::kIso8601Milli},
    {"iso8601_sec",   TimestampFormat::kIso8601Sec},
    {"epoch_nano",    TimestampFormat::kEpochNano},
    {"epoch_micro",   TimestampFormat::kEpochMicro},
    {"epoch_milli",   TimestampFormat::kEpochMilli},
    {"epoch_sec",     TimestampFormat::kEpochSec},
}};

}  // namespace

TimestampFormat TimestampFormatFromString(std::string_view s) {
    for (const auto& e : kTsTable) {
        if (e.name == s) return e.fmt;
    }
    throw std::runtime_error(fmt::format(
        "Unknown timestamp format '{}' (expected iso8601_micro/iso8601_milli/"
        "iso8601_sec/epoch_nano/epoch_micro/epoch_milli/epoch_sec)", s));
}

std::string_view ToString(TimestampFormat fmt) noexcept {
    for (const auto& e : kTsTable) {
        if (e.fmt == fmt) return e.name;
    }
    return "unknown";
}

}  // namespace ulog
