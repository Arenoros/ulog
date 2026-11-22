#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <ulog/export.hpp>

namespace ulog {

/// Source of Record event timestamps, fixed when a Runtime is created.
///
/// `now` returns wall-clock microseconds since the Unix epoch, 1970-01-01T00:00:00 UTC. A
/// producer calls it exactly once for each admitted Record, after admission reservation and
/// before publication, so it must be thread-safe, non-blocking, allocation-free, and must not log.
/// A null `now` selects the system clock. `context` is passed to `now` unchanged and must outlive
/// the Runtime.
struct EventClock final {
  void* context{nullptr};
  std::int64_t (*now)(void* context) noexcept {nullptr};
};

/// Length of `YYYY-MM-DDTHH:MM:SS.ffffff`.
inline constexpr std::size_t kTimestampTextBytes = 26;

/// Fixed-capacity timestamp text; it owns its characters and never allocates.
struct TimestampText final {
  std::array<char, kTimestampTextBytes> bytes{};
  std::size_t size{0};

  [[nodiscard]] std::string_view View() const noexcept { return {bytes.data(), size}; }
};

/// Formats microseconds since the Unix epoch as UTC `YYYY-MM-DDTHH:MM:SS.ffffff`.
///
/// Thread-safe and independent of the C library time functions. The result is empty for instants
/// outside years 0000 through 9999.
[[nodiscard]] ULOG_API TimestampText FormatUtcTimestamp(std::int64_t unix_microseconds) noexcept;

/// Formats microseconds since the Unix epoch in the process-local time zone as
/// `YYYY-MM-DDTHH:MM:SS.ffffff`, without a zone designator.
///
/// Thread-safe. The result is empty for instants outside years 0000 through 9999 or that the
/// platform cannot convert to local time, such as instants before 1970 on Windows.
[[nodiscard]] ULOG_API TimestampText FormatLocalTimestamp(std::int64_t unix_microseconds) noexcept;

}  // namespace ulog
