#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ulog/testing/in_memory_destination.hpp>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::testing {

class InMemoryDestinationAccess;

class DestinationWriteClaim final {
 public:
  DestinationWriteClaim() noexcept = default;
  DestinationWriteClaim(DestinationWriteClaim&& other) noexcept;
  DestinationWriteClaim& operator=(DestinationWriteClaim&& other) noexcept;
  DestinationWriteClaim(const DestinationWriteClaim&) = delete;
  DestinationWriteClaim& operator=(const DestinationWriteClaim&) = delete;
  ~DestinationWriteClaim();

  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
  void Store(std::uint64_t admission_sequence, const producer::RecordView& record) noexcept;

 private:
  friend class InMemoryDestinationAccess;

  DestinationWriteClaim(std::shared_ptr<InMemoryDestinationState> state, std::size_t slot_index,
                        std::uint64_t generation) noexcept;
  void Reset() noexcept;

  std::shared_ptr<InMemoryDestinationState> state_;
  std::size_t slot_index_{0};
  std::uint64_t generation_{0};
};

class InMemoryDestinationAccess final {
 public:
  [[nodiscard]] static DestinationWriteClaim WaitForWrite(
      ulog::testing::InMemoryDestination& destination);
  static void Stop(ulog::testing::InMemoryDestination& destination) noexcept;
  [[nodiscard]] static std::size_t FixedBackingBytes(
      const ulog::testing::InMemoryDestination& destination) noexcept;
};

}  // namespace ulog::detail::testing
