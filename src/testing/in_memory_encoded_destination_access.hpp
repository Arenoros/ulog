#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ulog/testing/in_memory_encoded_destination.hpp>

#include "producer/producer_kernel.hpp"
#include "route/delivery_accounting.hpp"

namespace ulog::detail::testing {

using EncodedDestinationStoreResult = route::StoreResult;
using EncodedDestinationWake = route::RouteWake;
using EncodedDeliveryOutcome = route::DeliveryOutcome;
using EncodedDeliveryRetirement = route::DeliveryRetirement;
using EncodedInFlightSummary = route::InFlightSummary;

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
