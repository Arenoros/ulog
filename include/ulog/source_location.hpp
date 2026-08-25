#pragma once

#include <cstdint>
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

}  // namespace ulog
