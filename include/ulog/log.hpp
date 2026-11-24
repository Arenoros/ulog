#pragma once

#include <ulog/fields.hpp>
#include <ulog/log_writer.hpp>
#include <ulog/logger.hpp>

#if defined(LOG) || defined(LOG_TO) || defined(LOG_TRACE) || defined(LOG_DEBUG) ||              \
    defined(LOG_INFO) || defined(LOG_WARNING) || defined(LOG_ERROR) || defined(LOG_CRITICAL) || \
    defined(LOG_TRACE_TO) || defined(LOG_DEBUG_TO) || defined(LOG_INFO_TO) ||                   \
    defined(LOG_WARNING_TO) || defined(LOG_ERROR_TO) || defined(LOG_CRITICAL_TO)
#error \
    "A LOG macro is already defined. Undefine the conflicting frontend before including <ulog/log.hpp>."
#endif

#define ULOG_DETAIL_CONCAT_IMPL(left, right) left##right
#define ULOG_DETAIL_CONCAT(left, right) ULOG_DETAIL_CONCAT_IMPL(left, right)
#define ULOG_DETAIL_WRITER_NAME(counter) ULOG_DETAIL_CONCAT(ulog_detail_generated_writer_, counter)

// One statement: a scope writer that reserves the Record before the message operands, any
// trailing `<<` operands, and the Record's publication are evaluated. A filtered or rejected call
// leaves the loop without evaluating them; an exception abandons the Record.
#define ULOG_DETAIL_LOG_TO_IMPL(counter, target, level, ...)                                   \
  for (::ulog::LogWriter ULOG_DETAIL_WRITER_NAME(counter){                                     \
           (target), (level), ::ulog::detail::CallSite(ULOG_SOURCE_LOCATION())};               \
       ULOG_DETAIL_WRITER_NAME(counter).IsActive(); ULOG_DETAIL_WRITER_NAME(counter).Submit()) \
  ULOG_DETAIL_WRITER_NAME(counter)(__VA_ARGS__)

#define ULOG_DETAIL_LOG_TO(target, level, ...) \
  ULOG_DETAIL_LOG_TO_IMPL(__COUNTER__, target, level, __VA_ARGS__)

#define ULOG_DETAIL_NAMED_LOG_TO(target, level, ...) ULOG_DETAIL_LOG_TO(target, level, __VA_ARGS__)

#define ULOG_DETAIL_NAMED_LOG(level, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(::ulog::GetDefaultLogger(), level, __VA_ARGS__)

// Erased statements still type-check, but evaluate neither the target nor any operand.
#define ULOG_DETAIL_ERASED_NAMED_LOG_TO(target, level, ...) \
  if constexpr (true) {                                     \
  } else                                                    \
    ULOG_DETAIL_LOG_TO(target, level, __VA_ARGS__)

#define ULOG_DETAIL_ERASED_NAMED_LOG(level, ...) \
  ULOG_DETAIL_ERASED_NAMED_LOG_TO(::ulog::GetDefaultLogger(), level, __VA_ARGS__)

#define LOG(level, ...) ULOG_DETAIL_NAMED_LOG_TO(::ulog::GetDefaultLogger(), level, __VA_ARGS__)
#define LOG_TO(logger, level, ...) ULOG_DETAIL_NAMED_LOG_TO(logger, level, __VA_ARGS__)

#if ULOG_COMPILE_TIME_MIN_LEVEL <= 0
#define LOG_TRACE(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kTrace, __VA_ARGS__)
#define LOG_TRACE_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kTrace, __VA_ARGS__)
#else
#define LOG_TRACE(...) ULOG_DETAIL_ERASED_NAMED_LOG(::ulog::Level::kTrace, __VA_ARGS__)
#define LOG_TRACE_TO(logger, ...) \
  ULOG_DETAIL_ERASED_NAMED_LOG_TO(logger, ::ulog::Level::kTrace, __VA_ARGS__)
#endif

#if ULOG_COMPILE_TIME_MIN_LEVEL <= 1
#define LOG_DEBUG(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kDebug, __VA_ARGS__)
#define LOG_DEBUG_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kDebug, __VA_ARGS__)
#else
#define LOG_DEBUG(...) ULOG_DETAIL_ERASED_NAMED_LOG(::ulog::Level::kDebug, __VA_ARGS__)
#define LOG_DEBUG_TO(logger, ...) \
  ULOG_DETAIL_ERASED_NAMED_LOG_TO(logger, ::ulog::Level::kDebug, __VA_ARGS__)
#endif

#if ULOG_COMPILE_TIME_MIN_LEVEL <= 2
#define LOG_INFO(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kInfo, __VA_ARGS__)
#define LOG_INFO_TO(logger, ...) ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kInfo, __VA_ARGS__)
#else
#define LOG_INFO(...) ULOG_DETAIL_ERASED_NAMED_LOG(::ulog::Level::kInfo, __VA_ARGS__)
#define LOG_INFO_TO(logger, ...) \
  ULOG_DETAIL_ERASED_NAMED_LOG_TO(logger, ::ulog::Level::kInfo, __VA_ARGS__)
#endif

#if ULOG_COMPILE_TIME_MIN_LEVEL <= 3
#define LOG_WARNING(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kWarning, __VA_ARGS__)
#define LOG_WARNING_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kWarning, __VA_ARGS__)
#else
#define LOG_WARNING(...) ULOG_DETAIL_ERASED_NAMED_LOG(::ulog::Level::kWarning, __VA_ARGS__)
#define LOG_WARNING_TO(logger, ...) \
  ULOG_DETAIL_ERASED_NAMED_LOG_TO(logger, ::ulog::Level::kWarning, __VA_ARGS__)
#endif

#if ULOG_COMPILE_TIME_MIN_LEVEL <= 4
#define LOG_ERROR(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kError, __VA_ARGS__)
#define LOG_ERROR_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kError, __VA_ARGS__)
#else
#define LOG_ERROR(...) ULOG_DETAIL_ERASED_NAMED_LOG(::ulog::Level::kError, __VA_ARGS__)
#define LOG_ERROR_TO(logger, ...) \
  ULOG_DETAIL_ERASED_NAMED_LOG_TO(logger, ::ulog::Level::kError, __VA_ARGS__)
#endif

#define LOG_CRITICAL(...) ULOG_DETAIL_NAMED_LOG(::ulog::Level::kCritical, __VA_ARGS__)
#define LOG_CRITICAL_TO(logger, ...) \
  ULOG_DETAIL_NAMED_LOG_TO(logger, ::ulog::Level::kCritical, __VA_ARGS__)
