#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ulog/testing/in_memory_encoded_destination.hpp>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::testing {

struct EncodedDestinationStoreResult final {
  std::size_t encoded_bytes{0};
  bool submitted{false};
};

/// Wakes the attached Runtime worker; invoked under the destination lock.
struct EncodedDestinationWake final {
  void* context{nullptr};
  void (*notify)(void*) noexcept {nullptr};
};

enum class EncodedDeliveryOutcome : std::uint8_t { kDelivered, kFailed };

struct EncodedDeliveryRetirement final {
  EncodedDeliveryOutcome outcome{EncodedDeliveryOutcome::kFailed};
  std::size_t encoded_bytes{0};
};

/// Submitted deliveries that the worker has not retired yet, limited to one watermark.
struct EncodedInFlightSummary final {
  std::uint64_t records{0};
  std::uint64_t bytes{0};
  std::uint64_t delivered_records{0};
  std::uint64_t delivered_bytes{0};
  std::uint64_t failed_records{0};
  std::uint64_t failed_bytes{0};
};

class EncodedDestinationWriteClaim final {
 public:
  EncodedDestinationWriteClaim() noexcept = default;
  EncodedDestinationWriteClaim(EncodedDestinationWriteClaim&& other) noexcept;
  EncodedDestinationWriteClaim& operator=(EncodedDestinationWriteClaim&& other) noexcept;
  EncodedDestinationWriteClaim(const EncodedDestinationWriteClaim&) = delete;
  EncodedDestinationWriteClaim& operator=(const EncodedDestinationWriteClaim&) = delete;
  ~EncodedDestinationWriteClaim();

  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
  /// Encodes into the claimed slot and submits one delivery in the current mode.
  [[nodiscard]] EncodedDestinationStoreResult StoreRaw(std::uint64_t admission_sequence,
                                                       const producer::RecordView& record) noexcept;

 private:
  friend class InMemoryEncodedDestinationAccess;

  EncodedDestinationWriteClaim(std::shared_ptr<InMemoryEncodedDestinationState> state,
                               EncodedDestinationSlotIdentity identity) noexcept;
  void Reset() noexcept;

  std::shared_ptr<InMemoryEncodedDestinationState> state_;
  EncodedDestinationSlotIdentity identity_{};
};

class InMemoryEncodedDestinationAccess final {
 public:
  [[nodiscard]] static bool TryAttachRuntime(ulog::testing::InMemoryEncodedDestination& destination,
                                             EncodedDestinationWake wake) noexcept;
  static void DetachRuntime(ulog::testing::InMemoryEncodedDestination& destination) noexcept;
  /// Claims a free retired slot without blocking; paused, stopped, or full destinations decline.
  [[nodiscard]] static EncodedDestinationWriteClaim TryClaimWrite(
      ulog::testing::InMemoryEncodedDestination& destination) noexcept;
  /// Retires the completed delivery for exactly `admission_sequence`, if it has completed.
  [[nodiscard]] static std::optional<EncodedDeliveryRetirement> TryRetire(
      ulog::testing::InMemoryEncodedDestination& destination,
      std::uint64_t admission_sequence) noexcept;
  [[nodiscard]] static EncodedInFlightSummary SummarizeInFlight(
      const ulog::testing::InMemoryEncodedDestination& destination,
      std::uint64_t watermark) noexcept;
  /// Rejects later claims and cancels every delivery that has not completed.
  static void Stop(ulog::testing::InMemoryEncodedDestination& destination) noexcept;
  /// Releases retirement state for every unretired delivery after Stop.
  static void DiscardInFlight(ulog::testing::InMemoryEncodedDestination& destination) noexcept;
  [[nodiscard]] static std::size_t FixedBackingBytes(
      const ulog::testing::InMemoryEncodedDestination& destination) noexcept;
};

}  // namespace ulog::detail::testing
