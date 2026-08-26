#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <ulog/export.hpp>

namespace ulog::detail::testing {

struct InMemoryEncodedDestinationState;
class InMemoryEncodedDestinationAccess;
struct EncodedDestinationSlotIdentity final {
  std::size_t index{0};
  std::uint64_t generation{0};
};

}  // namespace ulog::detail::testing

namespace ulog::testing {

struct InMemoryEncodedDestinationConfig final {
  std::size_t capacity_records{64};
  std::size_t maximum_record_bytes{16'384};
  bool start_paused{false};
};

class InMemoryEncodedDestination;

/// Immutable Raw-encoded frame that keeps its destination slot occupied until destruction.
///
/// Bytes returned by this object remain valid until the object is moved from or destroyed.
class ObservedEncodedRecord final {
 public:
  ULOG_API ObservedEncodedRecord(ObservedEncodedRecord&& other) noexcept;
  ULOG_API ObservedEncodedRecord& operator=(ObservedEncodedRecord&& other) noexcept;
  ObservedEncodedRecord(const ObservedEncodedRecord&) = delete;
  ObservedEncodedRecord& operator=(const ObservedEncodedRecord&) = delete;
  ULOG_API ~ObservedEncodedRecord();

  [[nodiscard]] ULOG_API std::uint64_t AdmissionSequence() const noexcept;
  [[nodiscard]] ULOG_API std::string_view Bytes() const noexcept;

 private:
  friend class InMemoryEncodedDestination;

  ObservedEncodedRecord(std::shared_ptr<detail::testing::InMemoryEncodedDestinationState> state,
                        detail::testing::EncodedDestinationSlotIdentity identity) noexcept;
  void Reset() noexcept;

  std::shared_ptr<detail::testing::InMemoryEncodedDestinationState> state_;
  detail::testing::EncodedDestinationSlotIdentity identity_{};
};

class InMemoryEncodedDestination final {
 public:
  /// Constructs a bounded Raw destination and preallocates all frame storage.
  ///
  /// Invalid or overflowing bounds throw std::invalid_argument before backing allocation.
  explicit ULOG_API InMemoryEncodedDestination(InMemoryEncodedDestinationConfig config);

  /// Copies share one bounded destination state and its encoded frames.
  InMemoryEncodedDestination(const InMemoryEncodedDestination&) noexcept = default;
  InMemoryEncodedDestination& operator=(const InMemoryEncodedDestination&) noexcept = default;
  InMemoryEncodedDestination(InMemoryEncodedDestination&&) noexcept = default;
  InMemoryEncodedDestination& operator=(InMemoryEncodedDestination&&) noexcept = default;
  ~InMemoryEncodedDestination() = default;

  /// Takes the Ready frame with the lowest admission sequence without blocking.
  [[nodiscard]] ULOG_API std::optional<ObservedEncodedRecord> TryTake() noexcept;
  ULOG_API void Resume() noexcept;
  [[nodiscard]] ULOG_API std::size_t Capacity() const noexcept;
  [[nodiscard]] ULOG_API std::size_t MaximumRecordBytes() const noexcept;
  /// Returns the internally derived complete Raw frame bound for each slot.
  [[nodiscard]] ULOG_API std::size_t MaximumEncodedRecordBytes() const noexcept;

 private:
  friend class ulog::detail::testing::InMemoryEncodedDestinationAccess;

  std::shared_ptr<detail::testing::InMemoryEncodedDestinationState> state_;
};

}  // namespace ulog::testing
