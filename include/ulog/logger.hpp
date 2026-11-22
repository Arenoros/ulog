#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <string_view>
#include <type_traits>
#include <ulog/export.hpp>
#include <ulog/fields.hpp>
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

/// Atomically replaces the process-wide Default Logger and returns the previous target.
///
/// No ownership is transferred. The state referenced by every Logger ever passed here must
/// remain alive at a stable address until application termination, including after replacement.
/// Concurrent calls that already loaded the previous target complete against it; replacement
/// performs no reclamation or quiescence wait.
ULOG_API Logger ExchangeDefaultLogger(Logger stable_logger) noexcept;

/// Outcome of Logger::SetLevel.
enum class LevelChangeStatus : std::uint8_t {
  /// The new threshold is visible to later calls.
  kChanged,
  /// The Null Logger always stays at kNone.
  kNullLogger,
  /// The value is not a Level enumerator.
  kInvalidLevel,
  /// The Runtime has closed admission; its Logger stays at kNone.
  kClosed,
};

namespace detail {

struct LoggerState;
struct LoggerAccess;
struct MacroAccess;
using TextConsumer = void (*)(void*, std::string_view);

class MessageSink final {
 public:
  class OutputIterator final {
   public:
    using value_type = void;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = void;
    using iterator_category = std::output_iterator_tag;

    OutputIterator& operator=(char value) {
      sink_->Append(std::string_view{&value, 1});
      return *this;
    }
    [[nodiscard]] OutputIterator& operator*() noexcept { return *this; }
    OutputIterator& operator++() noexcept { return *this; }
    OutputIterator operator++(int) noexcept { return *this; }

   private:
    friend class MessageSink;
    explicit OutputIterator(MessageSink& sink) noexcept : sink_(&sink) {}

    MessageSink* sink_;
  };

  MessageSink(void* context, TextConsumer consume_text,
              FieldConsumer consume_field = &DiscardField) noexcept
      : context_(context), consume_text_(consume_text), consume_field_(consume_field) {}

  void Append(std::string_view text) { consume_text_(context_, text); }
  [[nodiscard]] OutputIterator FormatOutput() noexcept { return OutputIterator{*this}; }
  [[nodiscard]] FieldWriter Fields() noexcept;

 private:
  static FieldStatus DiscardField(void*, std::string_view, const FieldValue&, FieldMode) noexcept {
    return FieldStatus::kTruncated;
  }

  void* context_;
  TextConsumer consume_text_;
  FieldConsumer consume_field_;
};

struct FieldSinkAccess final {
  [[nodiscard]] static FieldWriter Make(void* context, FieldConsumer consume) noexcept {
    return FieldWriter{context, consume};
  }
};

inline FieldWriter MessageSink::Fields() noexcept {
  return FieldSinkAccess::Make(context_, consume_field_);
}

using MessageBuilder = bool (*)(void*, MessageSink&);

}  // namespace detail

class Logger final {
 public:
  ULOG_API Logger() noexcept;

  [[nodiscard]] ULOG_API Level GetLevel() const noexcept;
  [[nodiscard]] ULOG_API bool ShouldLog(Level message_level) const noexcept;
  /// Atomically replaces the threshold of the referenced Logger state; kNone suppresses every
  /// level. Producers observe it on their next call without a lock.
  ULOG_API LevelChangeStatus SetLevel(Level threshold) const noexcept;

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
      LogMessage(kLevel, source, &factory, &BuildText<Factory>);
    }
  }

  /// Like the two-argument form, and first invokes `write_fields(FieldWriter&)` to attach typed
  /// fields. Both callables run synchronously at most once, after filtering and admission.
  template <Level kLevel, typename FieldsFactory, typename MessageFactory>
  void Log(const SourceLocation& source, FieldsFactory&& write_fields,
           MessageFactory&& make_message) const {
    static_assert(static_cast<std::uint8_t>(kLevel) <= static_cast<std::uint8_t>(Level::kNone),
                  "Logger::Log requires a valid ulog::Level enumerator.");
    using FieldsFactoryType = std::remove_reference_t<FieldsFactory>;
    using MessageFactoryType = std::remove_reference_t<MessageFactory>;
    static_assert(std::is_invocable_v<FieldsFactoryType&, FieldWriter&>,
                  "Logger::Log fields callback must accept ulog::FieldWriter&.");
    static_assert(std::is_invocable_v<MessageFactoryType&>,
                  "Logger::Log requires a message factory callable with no arguments.");
    static_assert(IsTextFactory<MessageFactoryType>(),
                  "Logger::Log message factory result must be usable as std::string_view.");

    if constexpr (IsCompileTimeEnabled<kLevel>()) {
      FieldsAndText<FieldsFactoryType, MessageFactoryType> call{write_fields, make_message};
      LogMessage(kLevel, source, &call, &BuildFieldsAndText<decltype(call)>);
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
  static bool BuildText(void* factory_context, detail::MessageSink& sink) {
    auto& factory = *static_cast<Factory*>(factory_context);
    decltype(auto) message = std::invoke(factory);
    sink.Append(std::string_view{message});
    return true;
  }

  template <typename FieldsFactory, typename MessageFactory>
  struct FieldsAndText final {
    FieldsFactory& write_fields;
    MessageFactory& make_message;
  };

  template <typename Call>
  static bool BuildFieldsAndText(void* call_context, detail::MessageSink& sink) {
    auto& call = *static_cast<Call*>(call_context);
    FieldWriter fields = sink.Fields();
    std::invoke(call.write_fields, fields);
    decltype(auto) message = std::invoke(call.make_message);
    sink.Append(std::string_view{message});
    return true;
  }

  ULOG_API void LogMessage(Level level, const SourceLocation& source, void* builder_context,
                           detail::MessageBuilder build_message) const;

  friend struct detail::LoggerAccess;
  friend struct detail::MacroAccess;

  const detail::LoggerState* state_;
};

/// Temporarily replaces the Default Logger threshold and restores the previous value on exit,
/// including during exception unwinding.
///
/// The scope loads the Default Logger once and changes only that target's threshold; it never
/// exchanges the target, so published Default Logger states keep their addresses. A later
/// ExchangeDefaultLogger does not redirect the restoration.
class DefaultLoggerLevelScope final {
 public:
  explicit DefaultLoggerLevelScope(Level threshold) noexcept
      : target_(GetDefaultLogger()),
        previous_(target_.GetLevel()),
        status_(target_.SetLevel(threshold)) {}

  DefaultLoggerLevelScope(const DefaultLoggerLevelScope&) = delete;
  DefaultLoggerLevelScope& operator=(const DefaultLoggerLevelScope&) = delete;
  DefaultLoggerLevelScope(DefaultLoggerLevelScope&&) = delete;
  DefaultLoggerLevelScope& operator=(DefaultLoggerLevelScope&&) = delete;

  ~DefaultLoggerLevelScope() {
    if (status_ == LevelChangeStatus::kChanged) {
      static_cast<void>(target_.SetLevel(previous_));
    }
  }

  /// Result of the initial change; nothing is restored unless it is kChanged.
  [[nodiscard]] LevelChangeStatus Status() const noexcept { return status_; }
  [[nodiscard]] Logger Target() const noexcept { return target_; }

 private:
  Logger target_;
  Level previous_;
  LevelChangeStatus status_;
};

}  // namespace ulog
