#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <ulog/timestamp.hpp>

namespace ulog {
namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
constexpr std::int64_t kSecondsPerDay = 86'400;
constexpr std::int64_t kSecondsPerHour = 3'600;
constexpr std::int64_t kSecondsPerMinute = 60;
// 0000-01-01T00:00:00Z and 10000-01-01T00:00:00Z.
constexpr std::int64_t kFirstFormattedSecond = -62'167'219'200;
constexpr std::int64_t kEndFormattedSecond = 253'402'300'800;
constexpr int kMaximumYear = 9'999;
constexpr int kTmYearOrigin = 1'900;

struct SplitInstant final {
  std::int64_t seconds;
  std::uint32_t microseconds;
};

[[nodiscard]] constexpr SplitInstant Split(std::int64_t unix_microseconds) noexcept {
  std::int64_t seconds = unix_microseconds / kMicrosecondsPerSecond;
  std::int64_t fraction = unix_microseconds % kMicrosecondsPerSecond;
  if (fraction < 0) {
    fraction += kMicrosecondsPerSecond;
    --seconds;
  }
  return {seconds, static_cast<std::uint32_t>(fraction)};
}

[[nodiscard]] constexpr bool InFormattedRange(std::int64_t seconds) noexcept {
  return seconds >= kFirstFormattedSecond && seconds < kEndFormattedSecond;
}

template <std::size_t kWidth>
void WriteDigits(char* out, std::uint32_t value) noexcept {
  for (std::size_t index = kWidth; index > 0; --index) {
    out[index - 1] = static_cast<char>('0' + value % 10U);
    value /= 10U;
  }
}

struct CivilTime final {
  int year;
  unsigned month;
  unsigned day;
  unsigned hour;
  unsigned minute;
  unsigned second;
  std::uint32_t microsecond;
};

[[nodiscard]] TimestampText Write(const CivilTime& time) noexcept {
  TimestampText text;
  if (time.year < 0 || time.year > kMaximumYear) {
    return text;
  }
  char* out = text.bytes.data();
  WriteDigits<4>(out, static_cast<std::uint32_t>(time.year));
  out[4] = '-';
  WriteDigits<2>(out + 5, time.month);
  out[7] = '-';
  WriteDigits<2>(out + 8, time.day);
  out[10] = 'T';
  WriteDigits<2>(out + 11, time.hour);
  out[13] = ':';
  WriteDigits<2>(out + 14, time.minute);
  out[16] = ':';
  WriteDigits<2>(out + 17, time.second);
  out[19] = '.';
  WriteDigits<6>(out + 20, time.microsecond);
  text.size = kTimestampTextBytes;
  return text;
}

[[nodiscard]] bool ToLocalTime(std::time_t seconds, std::tm& local) noexcept {
#if defined(_WIN32)
  return localtime_s(&local, &seconds) == 0;
#else
  return localtime_r(&seconds, &local) != nullptr;
#endif
}

}  // namespace

TimestampText FormatUtcTimestamp(std::int64_t unix_microseconds) noexcept {
  const SplitInstant instant = Split(unix_microseconds);
  if (!InFormattedRange(instant.seconds)) {
    return {};
  }
  std::int64_t days = instant.seconds / kSecondsPerDay;
  std::int64_t second_of_day = instant.seconds % kSecondsPerDay;
  if (second_of_day < 0) {
    second_of_day += kSecondsPerDay;
    --days;
  }
  const std::chrono::year_month_day date{
      std::chrono::sys_days{std::chrono::days{static_cast<int>(days)}}};
  return Write({
      .year = static_cast<int>(date.year()),
      .month = static_cast<unsigned>(date.month()),
      .day = static_cast<unsigned>(date.day()),
      .hour = static_cast<unsigned>(second_of_day / kSecondsPerHour),
      .minute = static_cast<unsigned>(second_of_day % kSecondsPerHour / kSecondsPerMinute),
      .second = static_cast<unsigned>(second_of_day % kSecondsPerMinute),
      .microsecond = instant.microseconds,
  });
}

TimestampText FormatLocalTimestamp(std::int64_t unix_microseconds) noexcept {
  const SplitInstant instant = Split(unix_microseconds);
  // Bound the input before the platform conversion; the zone offset is checked by Write.
  if (!InFormattedRange(instant.seconds)) {
    return {};
  }
  std::tm local{};
  if (!ToLocalTime(static_cast<std::time_t>(instant.seconds), local)) {
    return {};
  }
  return Write({
      .year = local.tm_year + kTmYearOrigin,
      .month = static_cast<unsigned>(local.tm_mon + 1),
      .day = static_cast<unsigned>(local.tm_mday),
      .hour = static_cast<unsigned>(local.tm_hour),
      .minute = static_cast<unsigned>(local.tm_min),
      .second = static_cast<unsigned>(local.tm_sec),
      .microsecond = instant.microseconds,
  });
}

}  // namespace ulog
