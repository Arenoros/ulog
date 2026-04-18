#include <ulog/level.hpp>

#include <array>
#include <cctype>
#include <stdexcept>
#include <utility>

#include <fmt/format.h>

namespace ulog {

namespace {

struct Entry {
    std::string_view name;
    Level level;
};

constexpr std::array<Entry, 7> kLower = {{
    {"trace", Level::kTrace},
    {"debug", Level::kDebug},
    {"info", Level::kInfo},
    {"warning", Level::kWarning},
    {"error", Level::kError},
    {"critical", Level::kCritical},
    {"none", Level::kNone},
}};

constexpr std::array<Entry, 7> kUpper = {{
    {"TRACE", Level::kTrace},
    {"DEBUG", Level::kDebug},
    {"INFO", Level::kInfo},
    {"WARNING", Level::kWarning},
    {"ERROR", Level::kError},
    {"CRITICAL", Level::kCritical},
    {"NONE", Level::kNone},
}};

bool IEquals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

}  // namespace

Level LevelFromString(std::string_view name) {
    for (const auto& e : kLower) {
        if (IEquals(name, e.name)) return e.level;
    }
    throw std::runtime_error(
        fmt::format("Unknown log level '{}' (expected one of trace/debug/info/warning/error/critical/none)",
                    name));
}

std::string_view ToString(Level level) {
    for (const auto& e : kLower) {
        if (e.level == level) return e.name;
    }
    return "unknown";
}

std::string_view ToUpperCaseString(Level level) noexcept {
    for (const auto& e : kUpper) {
        if (e.level == level) return e.name;
    }
    return "ERROR";
}

std::optional<Level> OptionalLevelFromString(const std::optional<std::string>& name) {
    if (!name) return std::nullopt;
    return LevelFromString(*name);
}

}  // namespace ulog
