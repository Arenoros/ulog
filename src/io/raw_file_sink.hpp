#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "producer/producer_kernel.hpp"
#include "route/delivery_accounting.hpp"

namespace ulog::detail::io {

inline constexpr std::uint64_t kNoInjectedWriteFailure = std::numeric_limits<std::uint64_t>::max();

/// Deterministic test faults and observations applied on the I/O loop thread.
///
/// Production Runtimes use the default plan, which injects nothing and observes nothing.
struct FileFaultPlan final {
  /// Fails the open with this libuv error instead of opening the path.
  std::int32_t open_error{0};
  /// Caps every write submission, forcing partial-write continuation when non-zero.
  std::size_t maximum_write_bytes{0};
  /// Zero-based write submission that fails with `write_error` instead of reaching the OS.
  std::uint64_t failing_write_call{kNoInjectedWriteFailure};
  std::int32_t write_error{0};
  /// Reports this libuv error after the file is really closed.
  std::int32_t close_error{0};
  /// Counts filesystem operations issued on and off the dedicated loop thread.
  std::atomic<std::uint64_t>* loop_thread_operations{nullptr};
  std::atomic<std::uint64_t>* other_thread_operations{nullptr};
};

enum class FileSinkStartStatus : std::uint8_t {
  kStarted,
  kThreadFailed,
  kLoopFailed,
  kOpenFailed,
  kTimedOut,
};

struct FileSinkStartResult final {
  FileSinkStartStatus status{FileSinkStartStatus::kThreadFailed};
  std::int32_t io_error{0};
};

struct RawFileSinkState;

/// Exclusive worker ownership of one Runtime-owned encoded frame buffer.
class FileWriteClaim final {
 public:
  FileWriteClaim() noexcept = default;
  FileWriteClaim(FileWriteClaim&& other) noexcept;
  FileWriteClaim& operator=(FileWriteClaim&& other) noexcept;
  FileWriteClaim(const FileWriteClaim&) = delete;
  FileWriteClaim& operator=(const FileWriteClaim&) = delete;
  ~FileWriteClaim();

  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
  /// Encodes into the claimed buffer and queues one append for the loop thread.
  [[nodiscard]] route::StoreResult StoreRaw(std::uint64_t admission_sequence,
                                            const producer::RecordView& record) noexcept;

 private:
  friend class RawFileSink;

  FileWriteClaim(std::shared_ptr<RawFileSinkState> state, std::size_t index,
                 std::uint64_t generation) noexcept;
  void Reset() noexcept;

  std::shared_ptr<RawFileSinkState> state_;
  std::size_t index_{0};
  std::uint64_t generation_{0};
};

/// Appends Raw frames to one file through a Runtime-owned libuv loop on a dedicated thread.
///
/// Every buffer, request, and loop structure is allocated before Start(). The worker encodes into
/// a claimed buffer; the loop thread performs one append at a time in admission order,
/// continues partial writes from the known frame offset, and never replays a failed frame.
class RawFileSink final {
 public:
  RawFileSink(std::string path_utf8, std::size_t write_buffers, std::size_t maximum_record_bytes,
              FileFaultPlan faults);
  ~RawFileSink();

  RawFileSink(const RawFileSink&) = delete;
  RawFileSink& operator=(const RawFileSink&) = delete;
  RawFileSink(RawFileSink&&) = delete;
  RawFileSink& operator=(RawFileSink&&) = delete;

  /// Starts the loop thread, opens the file on it, and waits for the result until `deadline`.
  [[nodiscard]] FileSinkStartResult Start(route::RouteWake wake,
                                          std::chrono::steady_clock::time_point deadline) noexcept;
  [[nodiscard]] FileWriteClaim TryClaimWrite() noexcept;
  [[nodiscard]] std::optional<route::DeliveryRetirement> TryRetire(
      std::uint64_t admission_sequence) noexcept;
  [[nodiscard]] route::InFlightSummary SummarizeInFlight(std::uint64_t watermark) const noexcept;
  /// Returns the libuv error that failed the route, or zero while it remains usable.
  [[nodiscard]] std::int32_t FailureError() const noexcept;
  /// Cancels queued appends, rejects later claims, and closes after the active append.
  void Stop() noexcept;
  /// Closes the file and loop once no append is active.
  void RequestClose() noexcept;
  /// Returns the close result once the file and loop have closed.
  [[nodiscard]] std::optional<std::int32_t> CloseResult() const noexcept;
  void DiscardInFlight() noexcept;
  void DetachWake() noexcept;
  /// Joins the loop thread if it stops before `deadline`; otherwise detaches it safely.
  bool JoinUntil(std::chrono::steady_clock::time_point deadline) noexcept;
  [[nodiscard]] std::size_t FixedBackingBytes() const noexcept;

 private:
  std::shared_ptr<RawFileSinkState> state_;
  std::thread thread_;
};

}  // namespace ulog::detail::io
