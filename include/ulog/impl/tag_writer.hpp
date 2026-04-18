#pragma once

/// @file ulog/impl/tag_writer.hpp
/// @brief Low-level tag writer, funnels tags into the active formatter.
///
/// This is an implementation-detail helper accessible to users who need
/// direct control over tag emission inside a LogHelper (e.g. custom
/// extension operators that add structured fields). For ordinary use,
/// stream a `LogExtra` into a `LogHelper` instead.

#include <string_view>

#include <ulog/fwd.hpp>
#include <ulog/impl/formatters/base.hpp>
#include <ulog/json_string.hpp>

namespace ulog::impl {

class TagWriter final {
public:
    explicit TagWriter(formatters::Base* formatter) noexcept : formatter_(formatter) {}

    /// Writes a plain string-valued tag.
    void PutTag(std::string_view key, std::string_view value);

    /// Writes a tag whose value is pre-serialized JSON.
    void PutJsonTag(std::string_view key, const JsonString& value);

    /// Spills every key-value pair of `extra` through PutTag / PutJsonTag.
    void PutLogExtra(const LogExtra& extra);

    /// Resets the underlying formatter pointer (used when LogHelper is inactive).
    void Reset(formatters::Base* formatter) noexcept { formatter_ = formatter; }

private:
    formatters::Base* formatter_{nullptr};
};

}  // namespace ulog::impl
