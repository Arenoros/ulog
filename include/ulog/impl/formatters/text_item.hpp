#pragma once

/// @file ulog/impl/formatters/text_item.hpp
/// @brief Textual log-record payload produced by TSKV/LTSV/RAW/JSON formatters.

#include <ulog/detail/small_string.hpp>
#include <ulog/impl/formatters/base.hpp>

namespace ulog::impl::formatters {

/// Textual record: a finalized single-line string (including trailing '\n').
struct TextLogItem final : LoggerItemBase {
    detail::SmallString<4096> payload;
};

}  // namespace ulog::impl::formatters
