#include "process_cpu_clock.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <sys/time.h>

#include <cerrno>
#else
#error "ReadProcessCpuTime supports only Windows, Linux, and macOS"
#endif

namespace ulog::benchmark_support {
namespace {

[[noreturn]] void ThrowCpuTimeOutOfRange() {
  throw std::runtime_error(
      "process CPU time exceeds the nanosecond clock range; restart the benchmark process");
}

#if defined(_WIN32)

constexpr std::uint64_t kNanosecondsPerFileTimeTick = 100;

[[nodiscard]] std::uint64_t ToTicks(const FILETIME& value) noexcept {
  ULARGE_INTEGER ticks{};
  ticks.LowPart = value.dwLowDateTime;
  ticks.HighPart = value.dwHighDateTime;
  return ticks.QuadPart;
}

[[nodiscard]] std::chrono::nanoseconds FromFileTimes(const FILETIME& kernel_time,
                                                     const FILETIME& user_time) {
  const std::uint64_t kernel_ticks = ToTicks(kernel_time);
  const std::uint64_t user_ticks = ToTicks(user_time);
  const auto max_nanoseconds = std::chrono::nanoseconds::max().count();
  const auto max_ticks = static_cast<std::uint64_t>(max_nanoseconds) / kNanosecondsPerFileTimeTick;
  if (kernel_ticks > max_ticks || user_ticks > max_ticks - kernel_ticks) {
    ThrowCpuTimeOutOfRange();
  }

  const std::uint64_t total_nanoseconds = (kernel_ticks + user_ticks) * kNanosecondsPerFileTimeTick;
  return std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(total_nanoseconds)};
}

#else

constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000;
constexpr std::uint64_t kNanosecondsPerMicrosecond = 1'000;

[[nodiscard]] std::chrono::nanoseconds FromTimeval(const timeval& value) {
  if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1'000'000) {
    throw std::runtime_error(
        "getrusage returned an invalid timeval; retry the benchmark; if it persists, verify OS "
        "resource accounting support");
  }

  const std::uint64_t seconds = static_cast<std::uint64_t>(value.tv_sec);
  const std::uint64_t microseconds = static_cast<std::uint64_t>(value.tv_usec);
  const auto max_nanoseconds = static_cast<std::uint64_t>(std::chrono::nanoseconds::max().count());
  if (seconds > max_nanoseconds / kNanosecondsPerSecond) {
    ThrowCpuTimeOutOfRange();
  }

  const std::uint64_t seconds_nanoseconds = seconds * kNanosecondsPerSecond;
  const std::uint64_t remainder_nanoseconds = microseconds * kNanosecondsPerMicrosecond;
  if (remainder_nanoseconds > max_nanoseconds - seconds_nanoseconds) {
    ThrowCpuTimeOutOfRange();
  }

  return std::chrono::nanoseconds{
      static_cast<std::chrono::nanoseconds::rep>(seconds_nanoseconds + remainder_nanoseconds)};
}

[[nodiscard]] std::chrono::nanoseconds AddCpuTimes(std::chrono::nanoseconds user_time,
                                                   std::chrono::nanoseconds system_time) {
  const auto max_nanoseconds = static_cast<std::uint64_t>(std::chrono::nanoseconds::max().count());
  const auto user_nanoseconds = static_cast<std::uint64_t>(user_time.count());
  const auto system_nanoseconds = static_cast<std::uint64_t>(system_time.count());
  if (user_nanoseconds > max_nanoseconds ||
      system_nanoseconds > max_nanoseconds - user_nanoseconds) {
    ThrowCpuTimeOutOfRange();
  }

  return std::chrono::nanoseconds{
      static_cast<std::chrono::nanoseconds::rep>(user_nanoseconds + system_nanoseconds)};
}

#endif

}  // namespace

std::chrono::nanoseconds ReadProcessCpuTime() {
#if defined(_WIN32)
  FILETIME creation_time{};
  FILETIME exit_time{};
  FILETIME kernel_time{};
  FILETIME user_time{};
  if (::GetProcessTimes(::GetCurrentProcess(), &creation_time, &exit_time, &kernel_time,
                        &user_time) == 0) {
    const DWORD error = ::GetLastError();
    throw std::runtime_error("GetProcessTimes failed with Windows error " + std::to_string(error) +
                             "; retry the benchmark; if it persists, verify process query "
                             "permissions");
  }
  return FromFileTimes(kernel_time, user_time);
#else
  rusage usage{};
  if (::getrusage(RUSAGE_SELF, &usage) != 0) {
    const int error = errno;
    throw std::runtime_error(
        "getrusage(RUSAGE_SELF) failed with errno " + std::to_string(error) +
        "; retry the benchmark; if it persists, verify OS resource accounting support");
  }
  return AddCpuTimes(FromTimeval(usage.ru_utime), FromTimeval(usage.ru_stime));
#endif
}

}  // namespace ulog::benchmark_support
