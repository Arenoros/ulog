#include <array>
#include <cstddef>
#include <string_view>
#include <ulog/log.hpp>

#ifndef ULOG_EXPECTED_MIN_LEVEL
#error "ULOG_EXPECTED_MIN_LEVEL must be defined for the log cutoff probe."
#endif

#if ULOG_EXPECTED_MIN_LEVEL < 0 || ULOG_EXPECTED_MIN_LEVEL > 5
#error "ULOG_EXPECTED_MIN_LEVEL must be an integer from 0 (Trace) through 5 (Critical)."
#endif

static_assert(ULOG_COMPILE_TIME_MIN_LEVEL == ULOG_EXPECTED_MIN_LEVEL);

namespace erased_message {

// These declarations intentionally have no definitions. A non-erased builder creates a link error.
[[nodiscard]] std::string_view Trace();
[[nodiscard]] std::string_view Debug();
[[nodiscard]] std::string_view Info();
[[nodiscard]] std::string_view Warning();
[[nodiscard]] std::string_view Error();

}  // namespace erased_message

namespace {

constexpr std::size_t kLevelCount = 6;

std::array<int, kLevelCount> target_evaluations{};
std::array<int, kLevelCount> message_evaluations{};
int generic_level_evaluations = 0;
int generic_target_evaluations = 0;
int generic_message_evaluations = 0;

[[nodiscard]] constexpr std::size_t ToIndex(ulog::Level level) noexcept {
  return static_cast<std::size_t>(level);
}

[[nodiscard]] ulog::Logger CountTarget(ulog::Level level) noexcept {
  ++target_evaluations[ToIndex(level)];
  return ulog::GetNullLogger();
}

[[nodiscard]] std::string_view CountMessage(ulog::Level level) noexcept {
  ++message_evaluations[ToIndex(level)];
  return "message must remain lazy";
}

[[nodiscard]] ulog::Level CountGenericLevel() noexcept {
  ++generic_level_evaluations;
  return ulog::Level::kInfo;
}

[[nodiscard]] ulog::Logger CountGenericTarget() noexcept {
  ++generic_target_evaluations;
  return ulog::GetNullLogger();
}

[[nodiscard]] std::string_view CountGenericMessage() noexcept {
  ++generic_message_evaluations;
  return "generic message must remain lazy";
}

void ExerciseNamedMacros() {
#if ULOG_EXPECTED_MIN_LEVEL > 0
  LOG_TRACE_TO(CountTarget(ulog::Level::kTrace), erased_message::Trace());
  LOG_TRACE(erased_message::Trace());
#else
  LOG_TRACE_TO(CountTarget(ulog::Level::kTrace), CountMessage(ulog::Level::kTrace));
  LOG_TRACE(CountMessage(ulog::Level::kTrace));
#endif

#if ULOG_EXPECTED_MIN_LEVEL > 1
  LOG_DEBUG_TO(CountTarget(ulog::Level::kDebug), erased_message::Debug());
  LOG_DEBUG(erased_message::Debug());
#else
  LOG_DEBUG_TO(CountTarget(ulog::Level::kDebug), CountMessage(ulog::Level::kDebug));
  LOG_DEBUG(CountMessage(ulog::Level::kDebug));
#endif

#if ULOG_EXPECTED_MIN_LEVEL > 2
  LOG_INFO_TO(CountTarget(ulog::Level::kInfo), erased_message::Info());
  LOG_INFO(erased_message::Info());
#else
  LOG_INFO_TO(CountTarget(ulog::Level::kInfo), CountMessage(ulog::Level::kInfo));
  LOG_INFO(CountMessage(ulog::Level::kInfo));
#endif

#if ULOG_EXPECTED_MIN_LEVEL > 3
  LOG_WARNING_TO(CountTarget(ulog::Level::kWarning), erased_message::Warning());
  LOG_WARNING(erased_message::Warning());
#else
  LOG_WARNING_TO(CountTarget(ulog::Level::kWarning), CountMessage(ulog::Level::kWarning));
  LOG_WARNING(CountMessage(ulog::Level::kWarning));
#endif

#if ULOG_EXPECTED_MIN_LEVEL > 4
  LOG_ERROR_TO(CountTarget(ulog::Level::kError), erased_message::Error());
  LOG_ERROR(erased_message::Error());
#else
  LOG_ERROR_TO(CountTarget(ulog::Level::kError), CountMessage(ulog::Level::kError));
  LOG_ERROR(CountMessage(ulog::Level::kError));
#endif

  LOG_CRITICAL_TO(CountTarget(ulog::Level::kCritical), CountMessage(ulog::Level::kCritical));
  LOG_CRITICAL(CountMessage(ulog::Level::kCritical));
}

[[nodiscard]] int CheckNamedMacros() noexcept {
  for (std::size_t index = 0; index < kLevelCount; ++index) {
    const int expected_target_evaluations =
        static_cast<int>(index) >= ULOG_EXPECTED_MIN_LEVEL ? 1 : 0;
    if (target_evaluations[index] != expected_target_evaluations) {
      return 10 + static_cast<int>(index);
    }
    if (message_evaluations[index] != 0) {
      return 20 + static_cast<int>(index);
    }
  }
  return 0;
}

[[nodiscard]] int CheckGenericMacro() noexcept {
  if (generic_level_evaluations != 1) {
    return 30;
  }
  if (generic_target_evaluations != 1) {
    return 31;
  }
  if (generic_message_evaluations != 0) {
    return 32;
  }
  return 0;
}

}  // namespace

int main() {
  ExerciseNamedMacros();

  if (const int result = CheckNamedMacros(); result != 0) {
    return result;
  }

  LOG_TO(CountGenericTarget(), CountGenericLevel(), CountGenericMessage());
  return CheckGenericMacro();
}
