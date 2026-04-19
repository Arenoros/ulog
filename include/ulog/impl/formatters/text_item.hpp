#pragma once

/// @file ulog/impl/formatters/text_item.hpp
/// @brief Textual log-record payload produced by TSKV/LTSV/RAW/JSON formatters.

#include <ulog/detail/small_string.hpp>
#include <ulog/impl/formatters/base.hpp>

namespace ulog::impl::formatters {

/// Textual record: a finalized single-line string (including trailing '\n').
///
/// Inline capacity 512 B fits the vast majority of records (timestamp +
/// level + module + text + a handful of tags). Larger records spill to a
/// heap std::string inside SmallString. With a 512 B inline buffer the
/// whole TextLogItem object fits in ~540 B — one small heap allocation
/// per record on the async producer path, rather than a full 4 KB.
struct TextLogItem final : LoggerItemBase {
    detail::SmallString<512> payload;
};

}  // namespace ulog::impl::formatters
