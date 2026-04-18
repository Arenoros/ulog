#pragma once

/// @file ulog/detail/source_root.hpp
/// @brief Compile-time trim of the source-root prefix from `__FILE__`.
///
/// Windows `__FILE__` expands to a long absolute path with backslashes
/// (`H:\proj\foo\bar.cpp`); TSKV/JSON escaping then doubles those
/// backslashes, producing noisy `module=` fields. Given a compile-time
/// `ULOG_SOURCE_ROOT_LITERAL`, this helper returns a pointer into the
/// original string literal past the root (no allocation, no copy).
///
/// Matching treats `/` and `\` as equivalent — the root passed by CMake
/// normally uses forward slashes while `__FILE__` on MSVC uses backslashes.

namespace ulog::detail {

constexpr char NormalizeSeparator(char c) noexcept { return c == '\\' ? '/' : c; }

constexpr bool CharsEqNorm(char a, char b) noexcept {
    return NormalizeSeparator(a) == NormalizeSeparator(b);
}

constexpr const char* TrimSourceRoot(const char* path, const char* root) noexcept {
    if (!path || !root || *root == '\0') return path ? path : "";
    const char* p = path;
    const char* r = root;
    while (*r != '\0' && *p != '\0' && CharsEqNorm(*r, *p)) {
        ++r;
        ++p;
    }
    if (*r == '\0') {
        // Full prefix match — drop the leading separator if any.
        if (*p == '/' || *p == '\\') ++p;
        return p;
    }
    return path;
}

}  // namespace ulog::detail

#if defined(ULOG_SOURCE_ROOT_LITERAL)
#define ULOG_IMPL_TRIM_FILE(path) ::ulog::detail::TrimSourceRoot((path), ULOG_SOURCE_ROOT_LITERAL)
#else
#define ULOG_IMPL_TRIM_FILE(path) (path)
#endif
