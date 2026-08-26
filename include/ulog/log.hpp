#pragma once

#include <fmt/format.h>

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>
#include <ulog/logger.hpp>
#include <utility>

namespace ulog::detail {

inline void WriteMessage(MessageSink&) = delete;

template <typename Text>
[[nodiscard]] bool WriteMessage(MessageSink& sink, Text&& text) {
  static_assert(
      requires { std::string_view{std::forward<Text>(text)}; },
      "A one-operand Ulog macro message must be usable as std::string_view.");
  try {
    sink.Append(std::string_view{std::forward<Text>(text)});
    return true;
  } catch (...) {
    return false;
  }
}

template <typename... Args>
  requires(sizeof...(Args) > 0)
[[nodiscard]] bool WriteMessage(MessageSink& sink, fmt::format_string<Args...> format,
                                Args&&... args) {
  try {
    fmt::format_to(sink.FormatOutput(), format, std::forward<Args>(args)...);
    return true;
  } catch (...) {
    return false;
  }
}

struct MacroAccess final {
  template <typename Builder>
  static void Log(Logger logger, Level level, const SourceLocation& source, Builder&& builder) {
    using BuilderType = std::remove_reference_t<Builder>;
    static_assert(std::is_invocable_r_v<bool, BuilderType&, MessageSink&>,
                  "A Ulog macro message builder must return bool and accept "
                  "ulog::detail::MessageSink&.");
    logger.LogMessage(level, source, std::addressof(builder), &BuildMessage<BuilderType>);
  }

 private:
  template <typename Builder>
  static bool BuildMessage(void* context, MessageSink& sink) {
    return (*static_cast<Builder*>(context))(sink);
  }
};

}  // namespace ulog::detail

#if defined(LOG) || defined(LOG_TO) || defined(LOG_TRACE) || defined(LOG_DEBUG) ||              \
    defined(LOG_INFO) || defined(LOG_WARNING) || defined(LOG_ERROR) || defined(LOG_CRITICAL) || \
    defined(LOG_TRACE_TO) || defined(LOG_DEBUG_TO) || defined(LOG_INFO_TO) ||                   \
    defined(LOG_WARNING_TO) || defined(LOG_ERROR_TO) || defined(LOG_CRITICAL_TO)
#error \
    "A LOG macro is already defined. Undefine the conflicting frontend before including <ulog/log.hpp>."
#endif

#define ULOG_DETAIL_CONCAT_IMPL(left, right) left##right
#define ULOG_DETAIL_CONCAT(left, right) ULOG_DETAIL_CONCAT_IMPL(left, right)
#define ULOG_DETAIL_SINK_NAME(counter) ULOG_DETAIL_CONCAT(ulog_detail_generated_sink_, counter)

#define ULOG_DETAIL_MESSAGE_BUILDER_IMPL(counter, ...)                                \
  [&](::ulog::detail::MessageSink& ULOG_DETAIL_SINK_NAME(counter)) {                  \
    return ::ulog::detail::WriteMessage(ULOG_DETAIL_SINK_NAME(counter), __VA_ARGS__); \
  }

#define ULOG_DETAIL_MESSAGE_BUILDER(...) ULOG_DETAIL_MESSAGE_BUILDER_IMPL(__COUNTER__, __VA_ARGS__)

#define ULOG_DETAIL_NAMED_LOG_TO(target, level, ...)                                       \
  do {                                                                                     \
    if constexpr (static_cast<std::uint8_t>(level) >= ULOG_COMPILE_TIME_MIN_LEVEL) {       \
      ::ulog::detail::MacroAccess::Log((target), level, ::ulog::SourceLocation::Current(), \
                                       ULOG_DETAIL_MESSAGE_BUILDER(__VA_ARGS__));          \
    }                                                                                      \
  } while (false)

#define ULOG_DETAIL_NAMED_LOG(level, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(::ulog::GetDefaultLogger(), level, __VA_ARGS__)

#define ULOG_DETAIL_GENERIC_LOG_TO(target, level, ...)                                     \
  do {                                                                                     \
    ::ulog::detail::MacroAccess::Log((target), (level), ::ulog::SourceLocation::Current(), \
                                     ULOG_DETAIL_MESSAGE_BUILDER(__VA_ARGS__));            \
  } while (false)

#define LOG(level, ...) ULOG_DETAIL_GENERIC_LOG_TO(::ulog::GetDefaultLogger(), level, __VA_ARGS__)
#define LOG_TO(logger, level, ...) ULOG_DETAIL_GENERIC_LOG_TO(logger, level, __VA_ARGS__)

#define LOG_TRACE(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kTrace, __VA_ARGS__)
#define LOG_DEBUG(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kDebug, __VA_ARGS__)
#define LOG_INFO(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kInfo, __VA_ARGS__)
#define LOG_WARNING(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kWarning, __VA_ARGS__)
#define LOG_ERROR(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kError, __VA_ARGS__)
#define LOG_CRITICAL(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kCritical, __VA_ARGS__)

#define LOG_TRACE_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kTrace, __VA_ARGS__)
#define LOG_DEBUG_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kDebug, __VA_ARGS__)
#define LOG_INFO_TO(logger, ...) ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kInfo, __VA_ARGS__)
#define LOG_WARNING_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kWarning, __VA_ARGS__)
#define LOG_ERROR_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kError, __VA_ARGS__)
#define LOG_CRITICAL_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kCritical, __VA_ARGS__)
