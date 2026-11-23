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
  /// Fails every parent-directory creation with this libuv error instead of creating it.
  std::int32_t directory_error{0};
  /// Caps every write submission, forcing partial-write continuation when non-zero.
  std::size_t maximum_write_bytes{0};
  /// Zero-based write submission that fails with `write_error` instead of reaching the OS.
  std::uint64_t failing_write_call{kNoInjectedWriteFailure};
  std::int32_t write_error{0};
  /// Zero-based write submission that first waits on a libuv threadpool thread until
  /// `*write_gate` is true, like a slow or stuck write that the loop cannot cancel. The gate must
  /// outlive the loop thread, which a Runtime may detach.
  std::uint64_t gated_write_call{kNoInjectedWriteFailure};
  const std::atomic<bool>* write_gate{nullptr};
  /// Reports this libuv error after the file is really closed.
  std::int32_t close_error{0};
  /// Fails every `fsync` with this libuv error instead of reaching the OS.
  std::int32_t sync_error{0};
  /// Counts filesystem operations issued on and off the dedicated loop thread.
  std::atomic<std::uint64_t>* loop_thread_operations{nullptr};
  std::atomic<std::uint64_t>* other_thread_operations{nullptr};
  /// Counts `fsync` submissions, which are also counted as loop-thread operations.
  std::atomic<std::uint64_t>* sync_operations{nullptr};
};

enum class FileSinkStartStatus : std::uint8_t {
  kStarted,
  kThreadFailed,
  kLoopFailed,
  kDirectoryCreateFailed,
  kOpenFailed,
  kTimedOut,
};

struct FileSinkStartResult final {
  FileSinkStartStatus status{FileSinkStartStatus::kThreadFailed};
  std::int32_t io_error{0};
};

struct FileSinkConfig final {
  /// Path bytes passed to libuv: UTF-8 on Windows and native bytes elsewhere.
  std::string path_utf8;
  std::size_t write_buffers{0};
  std::size_t maximum_record_bytes{0};
  bool create_parent_directories{true};
  bool truncate_on_start{false};
  FileFaultPlan faults{};
};

struct FileSlotIdentity final {
  std::size_t index{0};
  std::uint64_t generation{0};
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

  FileWriteClaim(std::shared_ptr<RawFileSinkState> state, FileSlotIdentity identity) noexcept;
  void Reset() noexcept;

  std::shared_ptr<RawFileSinkState> state_;
  FileSlotIdentity identity_{};
};

/// Appends Raw frames to one file through a Runtime-owned libuv loop on a dedicated thread.
///
/// Every buffer, request, and loop structure is allocated before Start(). The worker encodes into
/// a claimed buffer; the loop thread performs one append at a time in admission order,
/// continues partial writes from the known frame offset, and never replays a failed frame.
class RawFileSink final {
 public:
  explicit RawFileSink(FileSinkConfig config);
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
  /// Requests one `fsync` that starts after every append completed so far; returns its ticket.
  [[nodiscard]] std::uint64_t RequestSync() noexcept;
  /// Returns whether an `fsync` that covers `ticket` succeeded. A failure fails the route instead.
  [[nodiscard]] bool SyncCompleted(std::uint64_t ticket) const noexcept;
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
