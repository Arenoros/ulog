#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <string_view>
#include <thread>
#include <ulog/timestamp.hpp>
#include <vector>

// This executable owns its process time zone: the local cases select fixed zones through TZ.

namespace ulog::test {
namespace {

constexpr std::int64_t kMicrosecondsPerSecond = 1'000'000;
constexpr std::int64_t kThreeHoursMicroseconds = 10'800 * kMicrosecondsPerSecond;

[[nodiscard]] constexpr std::int64_t At(std::int64_t unix_seconds,
                                        std::int64_t microseconds = 0) noexcept {
  return unix_seconds * kMicrosecondsPerSecond + microseconds;
}

struct Case final {
  std::int64_t unix_microseconds;
  std::string_view text;
};

// Second, day, year, century, and leap-day boundaries, including the non-leap 1900 and 2100.
constexpr Case kUtcCases[] = {
    {At(0), "1970-01-01T00:00:00.000000"},
    {At(0, 999'999), "1970-01-01T00:00:00.999999"},
    {At(1), "1970-01-01T00:00:01.000000"},
    {At(59, 999'999), "1970-01-01T00:00:59.999999"},
    {At(60), "1970-01-01T00:01:00.000000"},
    {At(86'399, 999'999), "1970-01-01T23:59:59.999999"},
    {At(86'400), "1970-01-02T00:00:00.000000"},
    {At(946'684'799, 999'999), "1999-12-31T23:59:59.999999"},
    {At(946'684'800), "2000-01-01T00:00:00.000000"},
    {At(951'782'399, 999'999), "2000-02-28T23:59:59.999999"},
    {At(951'782'400), "2000-02-29T00:00:00.000000"},
    {At(951'868'800), "2000-03-01T00:00:00.000000"},
    {At(1'704'067'200, 123'456), "2024-01-01T00:00:00.123456"},
    {At(1'709'164'800, 1), "2024-02-29T00:00:00.000001"},
    {At(1'709'251'199, 999'999), "2024-02-29T23:59:59.999999"},
    {At(4'107'542'399, 999'999), "2100-02-28T23:59:59.999999"},
    {At(4'107'542'400), "2100-03-01T00:00:00.000000"},
    {-1, "1969-12-31T23:59:59.999999"},
    {At(-2'203'891'201, 999'999), "1900-02-28T23:59:59.999999"},
    {At(-2'203'891'200), "1900-03-01T00:00:00.000000"},
    {At(-62'167'219'200), "0000-01-01T00:00:00.000000"},
    {At(253'402'300'799, 999'999), "9999-12-31T23:59:59.999999"},
};

constexpr std::int64_t kOutOfRange[] = {
    At(-62'167'219'200) - 1,
    At(253'402'300'800),
    std::numeric_limits<std::int64_t>::min(),
    std::numeric_limits<std::int64_t>::max(),
};

// Windows converts local time only from 1970 through 3000, so local cases stay in that window.
[[nodiscard]] constexpr bool PortableLocalInput(std::int64_t unix_microseconds) noexcept {
  return unix_microseconds >= 0 && unix_microseconds < At(32'503'680'000);
}

void SelectTimeZone(const char* zone) {
#if defined(_WIN32)
  ASSERT_EQ(_putenv_s("TZ", zone), 0);
  _tzset();
#else
  ASSERT_EQ(setenv("TZ", zone, 1), 0);
  tzset();
#endif
}

TEST(Timestamp, UtcFormatsEveryCalendarBoundaryWithMicroseconds) {
  for (const auto& test_case : kUtcCases) {
    SCOPED_TRACE(test_case.text);
    const TimestampText text = FormatUtcTimestamp(test_case.unix_microseconds);
    EXPECT_EQ(text.size, kTimestampTextBytes);
    EXPECT_EQ(text.View(), test_case.text);
  }
}

TEST(Timestamp, UtcIsEmptyOutsideFourDigitYears) {
  EXPECT_TRUE(TimestampText{}.View().empty());
  for (const std::int64_t instant : kOutOfRange) {
    SCOPED_TRACE(instant);
    EXPECT_TRUE(FormatUtcTimestamp(instant).View().empty());
    EXPECT_TRUE(FormatLocalTimestamp(instant).View().empty());
  }
}

TEST(Timestamp, LocalMatchesUtcInAZeroOffsetZone) {
  SelectTimeZone("UTC0");
  for (const auto& test_case : kUtcCases) {
    if (!PortableLocalInput(test_case.unix_microseconds)) {
      continue;
    }
    SCOPED_TRACE(test_case.text);
    EXPECT_EQ(FormatLocalTimestamp(test_case.unix_microseconds).View(), test_case.text);
  }
}

TEST(Timestamp, LocalAppliesTheProcessZoneOffsetAcrossBoundaries) {
  // POSIX and Windows TZ both read "XXX-3" as three hours east of UTC.
  SelectTimeZone("XXX-3");
  for (const auto& test_case : kUtcCases) {
    const auto shifted = test_case.unix_microseconds - kThreeHoursMicroseconds;
    if (!PortableLocalInput(shifted)) {
      continue;
    }
    SCOPED_TRACE(test_case.text);
    EXPECT_EQ(FormatLocalTimestamp(shifted).View(), test_case.text);
  }
  EXPECT_EQ(FormatLocalTimestamp(At(946'674'000)).View(), "2000-01-01T00:00:00.000000");
}

TEST(Timestamp, ConcurrentUtcAndLocalCallsAgree) {
  SelectTimeZone("XXX-3");
  std::vector<TimestampText> expected_local;
  for (const auto& test_case : kUtcCases) {
    expected_local.push_back(
        FormatUtcTimestamp(test_case.unix_microseconds + kThreeHoursMicroseconds));
  }

  constexpr std::size_t kThreads = 8;
  constexpr std::size_t kRounds = 500;
  std::atomic<std::size_t> mismatches{0};
  std::vector<std::thread> threads;
  for (std::size_t thread = 0; thread < kThreads; ++thread) {
    threads.emplace_back([&] {
      for (std::size_t round = 0; round < kRounds; ++round) {
        for (std::size_t index = 0; index < std::size(kUtcCases); ++index) {
          const auto& test_case = kUtcCases[index];
          if (FormatUtcTimestamp(test_case.unix_microseconds).View() != test_case.text) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
          }
          if (PortableLocalInput(test_case.unix_microseconds) &&
              FormatLocalTimestamp(test_case.unix_microseconds).View() !=
                  expected_local[index].View()) {
            mismatches.fetch_add(1, std::memory_order_relaxed);
          }
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(mismatches.load(), 0U);
}

}  // namespace
}  // namespace ulog::test
