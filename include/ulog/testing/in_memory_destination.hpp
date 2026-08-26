#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <ulog/export.hpp>
#include <ulog/level.hpp>

namespace ulog::detail::testing {

struct InMemoryDestinationState;
class InMemoryDestinationAccess;

}  // namespace ulog::detail::testing

namespace ulog::testing {

struct InMemoryDestinationConfig final {
  std::size_t capacity_records{64};
  std::size_t maximum_record_bytes{16'384};
  bool start_paused{false};
};

class InMemoryDestination;

/// Immutable observation that keeps its destination slot occupied until destruction.
///
/// String views returned by this object remain valid until the object is moved from or destroyed.
class ObservedRecord final {
 public:
  ULOG_API ObservedRecord(ObservedRecord&& other) noexcept;
  ULOG_API ObservedRecord& operator=(ObservedRecord&& other) noexcept;
  ObservedRecord(const ObservedRecord&) = delete;
  ObservedRecord& operator=(const ObservedRecord&) = delete;
  ULOG_API ~ObservedRecord();

  [[nodiscard]] ULOG_API std::uint64_t AdmissionSequence() const noexcept;
  [[nodiscard]] ULOG_API Level GetLevel() const noexcept;
  [[nodiscard]] ULOG_API std::int64_t EventTimestamp() const noexcept;
  [[nodiscard]] ULOG_API std::string_view SourcePath() const noexcept;
  [[nodiscard]] ULOG_API std::string_view SourceFunction() const noexcept;
  [[nodiscard]] ULOG_API std::uint32_t SourceLine() const noexcept;
  [[nodiscard]] ULOG_API std::uint32_t SourceColumn() const noexcept;
  [[nodiscard]] ULOG_API std::string_view Message() const noexcept;
  [[nodiscard]] ULOG_API bool Truncated() const noexcept;
  [[nodiscard]] ULOG_API std::size_t SerializedBytes() const noexcept;
  [[nodiscard]] ULOG_API std::size_t AccountingChargeBytes() const noexcept;

 private:
  friend class InMemoryDestination;

  ObservedRecord(std::shared_ptr<detail::testing::InMemoryDestinationState> state,
                 std::size_t slot_index, std::uint64_t generation) noexcept;
  void Reset() noexcept;

  std::shared_ptr<detail::testing::InMemoryDestinationState> state_;
  std::size_t slot_index_{0};
  std::uint64_t generation_{0};
};

class InMemoryDestination final {
 public:
  explicit ULOG_API InMemoryDestination(InMemoryDestinationConfig config);

  /// Copies share one bounded destination state and its queued Records.
  InMemoryDestination(const InMemoryDestination&) noexcept = default;
  InMemoryDestination& operator=(const InMemoryDestination&) noexcept = default;
  InMemoryDestination(InMemoryDestination&&) noexcept = default;
  InMemoryDestination& operator=(InMemoryDestination&&) noexcept = default;
  ~InMemoryDestination() = default;

  /// Takes the Ready Record with the lowest admission sequence without blocking.
  [[nodiscard]] ULOG_API std::optional<ObservedRecord> TryTake() noexcept;
  ULOG_API void Resume() noexcept;
  [[nodiscard]] ULOG_API std::size_t Capacity() const noexcept;
  [[nodiscard]] ULOG_API std::size_t MaximumRecordBytes() const noexcept;

 private:
  friend class ulog::detail::testing::InMemoryDestinationAccess;

  std::shared_ptr<detail::testing::InMemoryDestinationState> state_;
};

}  // namespace ulog::testing
