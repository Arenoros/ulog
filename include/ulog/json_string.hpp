#pragma once

/// @file ulog/json_string.hpp
/// @brief JSON-validated string value for use in LogExtra.

#include <string>
#include <string_view>

namespace ulog {

/// Holds a pre-serialized JSON document (validated on construction).
/// Emits nested structure into JSON/TSKV formatters without re-parsing.
class JsonString {
public:
    JsonString() = default;

    /// Constructs from pre-built JSON text. Throws std::invalid_argument if not valid JSON.
    explicit JsonString(std::string json);

    /// Convenience: use kFromValidatedJson to skip validation for trusted input.
    struct TrustedTag {};
    static constexpr TrustedTag kTrusted{};
    JsonString(TrustedTag, std::string json) noexcept : json_(std::move(json)) {}

    const std::string& View() const noexcept { return json_; }
    bool Empty() const noexcept { return json_.empty(); }

private:
    std::string json_;
};

}  // namespace ulog
