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

/// Selects how the destination completes each later delivery submitted by its Runtime.
enum class EncodedDeliveryMode : std::uint8_t {
  /// Commits the frame for TryTake() while the Runtime submits it.
  kCompleteInline,
  /// Keeps the frame pending until a PendingEncodedDelivery completes or fails it.
  kHold,
  /// Fails the delivery while the Runtime submits it and discards the frame.
  kFailInline,
};

enum class EncodedDeliveryCompletionStatus : std::uint8_t {
  /// This call supplied the delivery's only completion.
  kCompleted,
  /// The handle is empty, moved from, or has already completed its delivery.
  kInvalidHandle,
  /// Runtime shutdown by destruction or route failure cancelled the delivery first.
  kCancelled,
};

class InMemoryEncodedDestination;

/// Move-only exactly-once completion for one held Raw delivery.
///
/// The frame stays pinned in its destination slot until the delivery completes. Destroying an
/// uncompleted handle fails its delivery so that every submitted delivery completes once.
class PendingEncodedDelivery final {
 public:
  ULOG_API PendingEncodedDelivery(PendingEncodedDelivery&& other) noexcept;
  ULOG_API PendingEncodedDelivery& operator=(PendingEncodedDelivery&& other) noexcept;
  PendingEncodedDelivery(const PendingEncodedDelivery&) = delete;
  PendingEncodedDelivery& operator=(const PendingEncodedDelivery&) = delete;
  ULOG_API ~PendingEncodedDelivery();

  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
  /// Returns the delivered Record's admission sequence, or zero for an empty handle.
  [[nodiscard]] std::uint64_t AdmissionSequence() const noexcept { return admission_sequence_; }
  /// Returns the complete Raw frame until this handle completes, is moved from, or is destroyed.
  [[nodiscard]] std::string_view Bytes() const noexcept { return bytes_; }
  /// Commits the frame for TryTake() and reports a delivered Record to the Runtime.
  [[nodiscard]] ULOG_API EncodedDeliveryCompletionStatus Complete() noexcept;
  /// Discards the frame and reports a failed Record to the Runtime.
  [[nodiscard]] ULOG_API EncodedDeliveryCompletionStatus Fail() noexcept;

 private:
  friend class InMemoryEncodedDestination;

  PendingEncodedDelivery(std::shared_ptr<detail::testing::InMemoryEncodedDestinationState> state,
                         detail::testing::EncodedDestinationSlotIdentity identity,
                         std::uint64_t admission_sequence, std::string_view bytes) noexcept;
  [[nodiscard]] EncodedDeliveryCompletionStatus Finish(bool delivered) noexcept;

  std::shared_ptr<detail::testing::InMemoryEncodedDestinationState> state_;
  detail::testing::EncodedDestinationSlotIdentity identity_{};
  std::uint64_t admission_sequence_{0};
  std::string_view bytes_{};
};

/// Immutable delivered Raw frame that keeps its destination slot occupied until destruction.
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
  /// Takes the held delivery with the lowest admission sequence without blocking.
  [[nodiscard]] ULOG_API std::optional<PendingEncodedDelivery> TryTakePendingDelivery() noexcept;
  /// Applies to deliveries submitted after the call; unknown enumerators are ignored.
  ULOG_API void SetDeliveryMode(EncodedDeliveryMode mode) noexcept;
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
