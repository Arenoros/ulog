#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <source_location>
#include <string_view>

namespace ulog {

class SourceLocation final {
 public:
  [[nodiscard]] static constexpr SourceLocation Current(
      std::source_location location = std::source_location::current()) noexcept {
    return Custom(location.file_name(), location.function_name(), location.line(),
                  location.column());
  }

  // The borrowed names must remain valid for the synchronous Logger call that uses them.
  [[nodiscard]] static constexpr SourceLocation Custom(std::string_view file_name,
                                                       std::string_view function_name,
                                                       std::uint_least32_t line,
                                                       std::uint_least32_t column = 0) noexcept {
    return SourceLocation{file_name, function_name, line, column};
  }

  [[nodiscard]] constexpr std::string_view GetFileName() const noexcept { return file_name_; }

  [[nodiscard]] constexpr std::string_view GetFunctionName() const noexcept {
    return function_name_;
  }

  [[nodiscard]] constexpr std::uint_least32_t GetLine() const noexcept { return line_; }

  [[nodiscard]] constexpr std::uint_least32_t GetColumn() const noexcept { return column_; }

 private:
  constexpr SourceLocation(std::string_view file_name, std::string_view function_name,
                           std::uint_least32_t line, std::uint_least32_t column) noexcept
      : file_name_(file_name), function_name_(function_name), line_(line), column_(column) {}

  std::string_view file_name_;
  std::string_view function_name_;
  std::uint_least32_t line_;
  std::uint_least32_t column_;
};

namespace detail {

[[nodiscard]] constexpr bool IsSourcePathSeparator(char character) noexcept {
#ifdef _WIN32
  return character == '/' || character == '\\';
#else
  return character == '/';
#endif
}

// Returns the length of the leading part of `path` that `prefix` removes, or zero. A prefix
// matches whole path components only; the separators that follow it are removed as well.
[[nodiscard]] constexpr std::size_t SourcePathPrefixLength(std::string_view path,
                                                           std::string_view prefix) noexcept {
  if (prefix.empty() || prefix.size() >= path.size()) {
    return 0;
  }
  for (std::size_t index = 0; index < prefix.size(); ++index) {
    const bool same = path[index] == prefix[index] ||
                      (IsSourcePathSeparator(path[index]) && IsSourcePathSeparator(prefix[index]));
    if (!same) {
      return 0;
    }
  }
  std::size_t length = prefix.size();
  if (!IsSourcePathSeparator(prefix.back()) && !IsSourcePathSeparator(path[length])) {
    return 0;
  }
  while (length < path.size() && IsSourcePathSeparator(path[length])) {
    ++length;
  }
  return length == path.size() ? 0 : length;
}

// The first matching prefix wins.
[[nodiscard]] constexpr SourceLocation TrimSourcePath(
    const SourceLocation& location, std::initializer_list<std::string_view> prefixes) noexcept {
  std::string_view path = location.GetFileName();
  for (const std::string_view prefix : prefixes) {
    if (const std::size_t length = SourcePathPrefixLength(path, prefix); length != 0) {
      path.remove_prefix(length);
      break;
    }
  }
  return SourceLocation::Custom(path, location.GetFunctionName(), location.GetLine(),
                                location.GetColumn());
}

}  // namespace detail

}  // namespace ulog

// The calling line's SourceLocation with the configured ULOG_SOURCE_PATH_PREFIXES removed from its
// path. A constant expression; Ulog macros use it as their call-site identity.
#ifdef ULOG_SOURCE_PATH_PREFIXES
#define ULOG_SOURCE_LOCATION() \
  ::ulog::detail::TrimSourcePath(::ulog::SourceLocation::Current(), {ULOG_SOURCE_PATH_PREFIXES})
#else
#define ULOG_SOURCE_LOCATION() ::ulog::SourceLocation::Current()
#endif
