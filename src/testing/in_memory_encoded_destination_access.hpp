#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <ulog/testing/in_memory_encoded_destination.hpp>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::testing {

struct EncodedDestinationStoreResult final {
  std::size_t encoded_bytes{0};
  bool committed{false};
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
  [[nodiscard]] static bool TryAttachRuntime(
      ulog::testing::InMemoryEncodedDestination& destination) noexcept;
  static void DetachRuntime(ulog::testing::InMemoryEncodedDestination& destination) noexcept;
  [[nodiscard]] static EncodedDestinationWriteClaim WaitForWrite(
      ulog::testing::InMemoryEncodedDestination& destination,
      std::chrono::steady_clock::duration recheck_interval) noexcept;
  static void Stop(ulog::testing::InMemoryEncodedDestination& destination) noexcept;
  [[nodiscard]] static std::size_t FixedBackingBytes(
      const ulog::testing::InMemoryEncodedDestination& destination) noexcept;
};

}  // namespace ulog::detail::testing
