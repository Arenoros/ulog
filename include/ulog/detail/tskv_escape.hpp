#pragma once

/// @file ulog/detail/tskv_escape.hpp
/// @brief TSKV key/value escaping. Portable non-SIMD implementation.

#include <string_view>

namespace ulog::detail {

inline constexpr char kTskvKeyValueSeparator = '=';
inline constexpr char kTskvPairsSeparator = '\t';

enum class TskvMode {
    kKey,                ///< Escape for a key (space,=,\ all escaped)
    kValue,              ///< Escape for a value
    kKeyReplacePeriod,   ///< As kKey, plus '.' replaced with '_'
};

/// Appends escaped representation of one character to sink.
template <typename Sink>
void EncodeTskvChar(Sink& sink, char ch, TskvMode mode) {
    const bool is_key = (mode == TskvMode::kKey || mode == TskvMode::kKeyReplacePeriod);
    switch (ch) {
        case '\t':
            sink += '\\';
            sink += 't';
            return;
        case '\r':
            sink += '\\';
            sink += 'r';
            return;
        case '\n':
            sink += '\\';
            sink += 'n';
            return;
        case '\0':
            sink += '\\';
            sink += '0';
            return;
        case '\\':
            sink += '\\';
            sink += '\\';
            return;
        case '=':
            if (is_key) {
                sink += '\\';
                sink += '=';
                return;
            }
            break;
        case '.':
            if (mode == TskvMode::kKeyReplacePeriod) {
                sink += '_';
                return;
            }
            break;
        default:
            break;
    }
    sink += ch;
}

/// Appends escaped representation of `str` to sink. Sink must support `+= char`.
template <typename Sink>
void EncodeTskv(Sink& sink, std::string_view str, TskvMode mode) {
    for (char c : str) EncodeTskvChar(sink, c, mode);
}

}  // namespace ulog::detail
