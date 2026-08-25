#pragma once

#include <cstdint>
#include <functional>
#include <string_view>
#include <type_traits>
#include <ulog/export.hpp>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>

#ifndef ULOG_COMPILE_TIME_MIN_LEVEL
#define ULOG_COMPILE_TIME_MIN_LEVEL 0
#endif

#if ULOG_COMPILE_TIME_MIN_LEVEL < 0 || ULOG_COMPILE_TIME_MIN_LEVEL > 5
#error "ULOG_COMPILE_TIME_MIN_LEVEL must be an integer from 0 (Trace) through 5 (Critical)."
#endif

namespace ulog {

class Logger;

[[nodiscard]] ULOG_API Logger GetDefaultLogger() noexcept;
[[nodiscard]] ULOG_API Logger GetNullLogger() noexcept;

namespace detail {

struct LoggerState;
struct LoggerAccess;
using TextConsumer = void (*)(void*, std::string_view);
using TextBuilder = void (*)(void*, void*, TextConsumer);

}  // namespace detail

class Logger final {
 public:
  ULOG_API Logger() noexcept;

  [[nodiscard]] ULOG_API Level GetLevel() const noexcept;
  [[nodiscard]] ULOG_API bool ShouldLog(Level message_level) const noexcept;

  template <Level kLevel, typename MessageFactory>
  void Log(const SourceLocation& source, MessageFactory&& make_message) const {
    static_assert(static_cast<std::uint8_t>(kLevel) <= static_cast<std::uint8_t>(Level::kNone),
                  "Logger::Log requires a valid ulog::Level enumerator.");
    using MessageFactoryType = std::remove_reference_t<MessageFactory>;
    static_assert(std::is_invocable_v<MessageFactoryType&>,
                  "Logger::Log requires a message factory callable with no arguments.");
    static_assert(IsTextFactory<MessageFactoryType>(),
                  "Logger::Log message factory result must be usable as std::string_view.");

    if constexpr (IsCompileTimeEnabled<kLevel>()) {
      auto factory = std::ref(make_message);
      using Factory = decltype(factory);
      LogText(kLevel, source, &factory, &BuildText<Factory>);
    }
  }

  friend constexpr bool operator==(Logger, Logger) noexcept = default;

 private:
  explicit constexpr Logger(const detail::LoggerState* state) noexcept : state_(state) {}

  template <Level kLevel>
  [[nodiscard]] static consteval bool IsCompileTimeEnabled() noexcept {
    return kLevel != Level::kNone && static_cast<std::uint8_t>(kLevel) >=
                                         static_cast<std::uint8_t>(ULOG_COMPILE_TIME_MIN_LEVEL);
  }

  template <typename MessageFactory>
  [[nodiscard]] static consteval bool IsTextFactory() noexcept {
    return requires(MessageFactory& factory) { std::string_view{std::invoke(factory)}; };
  }

  template <typename Factory>
  static void BuildText(void* factory_context, void* consumer_context,
                        detail::TextConsumer consume) {
    auto& factory = *static_cast<Factory*>(factory_context);
    decltype(auto) message = std::invoke(factory);
    consume(consumer_context, std::string_view{message});
  }

  ULOG_API void LogText(Level level, const SourceLocation& source, void* factory_context,
                        detail::TextBuilder build_text) const;

  friend struct detail::LoggerAccess;

  const detail::LoggerState* state_;
};

}  // namespace ulog
