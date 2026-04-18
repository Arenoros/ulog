#include <ulog/json_string.hpp>

#include <stdexcept>

#ifdef ULOG_WITH_NLOHMANN
#include <nlohmann/json.hpp>
#endif

namespace ulog {

namespace {

bool LooksLikeJson(std::string_view sv) noexcept {
    // Minimal cheap sanity check when nlohmann is absent: must start with '{'|'['|'"'|digit|t|f|n|-.
    for (char c : sv) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        return c == '{' || c == '[' || c == '"' || c == '-' ||
               (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n';
    }
    return false;
}

}  // namespace

JsonString::JsonString(std::string json) : json_(std::move(json)) {
#ifdef ULOG_WITH_NLOHMANN
    try {
        (void)nlohmann::json::parse(json_);
    } catch (const nlohmann::json::exception& e) {
        throw std::invalid_argument(std::string("Invalid JSON for JsonString: ") + e.what());
    }
#else
    if (!LooksLikeJson(json_)) {
        throw std::invalid_argument("Invalid JSON for JsonString (compiled without ULOG_WITH_NLOHMANN, only shape-check)");
    }
#endif
}

}  // namespace ulog
