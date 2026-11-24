#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <ulog/export.hpp>
#include <ulog/format.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/timestamp.hpp>

namespace ulog {

namespace testing {
class InMemoryDestination;
class InMemoryEncodedDestination;
}  // namespace testing

namespace detail {
struct RuntimeFactoryAccess;
}  // namespace detail

/// How a Runtime admits a call when its producer lane or payload budget is full.
enum class AdmissionPolicy : std::uint8_t {
  /// Rejects the incoming call before its message is built; nothing already admitted is evicted.
  kDropNewest,
  /// When the payload budget rejects a call, evicts the oldest Records of equal or lower level
  /// that the worker has not claimed until the call fits, and rejects it only if they cannot make
  /// room. Records admitted through the priority reserve are never evicted.
  kDropOldest,
  /// Keeps every call while the payload budget is below `thinning_low_watermark_percent`, then
  /// keeps calls with a probability that falls linearly to zero as the budget fills. Each keep
  /// decision is a stateless hash of the call site and a per-producer attempt nonce.
  kAdaptiveThinning,
};

struct RuntimeConfig final {
  Level threshold{Level::kInfo};
  std::size_t payload_capacity_bytes{1U << 20U};
  std::size_t maximum_record_bytes{16'384};
  std::size_t producer_slots{32};
  std::size_t ingress_cells{64};
  std::size_t control_operations{8};
  std::size_t worker_threads{1};
  std::chrono::milliseconds startup_timeout{5'000};
  std::chrono::milliseconds destruction_timeout{1'000};
  /// Event timestamp source; the default reads the system clock.
  EventClock event_clock{};
  AdmissionPolicy admission_policy{AdmissionPolicy::kDropNewest};
  /// Payload bytes that only Error and Critical Records may use, after `payload_capacity_bytes`
  /// rejected them. Zero, the default, disables the reserve; otherwise it is 64-byte aligned and
  /// at least `maximum_record_bytes`. The maximum Ulog-owned payload memory is
  /// `payload_capacity_bytes + priority_reserve_bytes`.
  std::size_t priority_reserve_bytes{0};
  /// Adaptive thinning keeps every call while at most this percentage (0 through 99) of
  /// `payload_capacity_bytes` is taken from the shared budget. Other policies ignore it.
  std::uint8_t thinning_low_watermark_percent{50};
};

/// One immutable route that appends encoded frames to a file through Ulog's private I/O loop.
struct RawFileRouteConfig final {
  /// File created if missing and opened for append. Must name a file, not a directory.
  std::filesystem::path path{};
  /// Runtime-owned complete-frame buffers; bounds queued and in-flight appends and their bytes.
  std::size_t write_buffers{8};
  /// Creates missing parent directories before opening; existing directories are left unchanged.
  bool create_parent_directories{true};
  /// Truncates an existing file when the route opens instead of appending after its content.
  bool truncate_on_start{false};
  /// Encoding of every frame; TSKV, LTSV, and Raw are available.
  Format format{Format::kRaw};
};

enum class RuntimeCreateErrorCode : std::uint8_t {
  kInvalidThreshold,
  kInvalidMaximumRecordBytes,
  kInvalidPayloadCapacity,
  kInvalidProducerSlots,
  kInvalidIngressCells,
  kInvalidControlCapacity,
  kInvalidWorkerCount,
  kInvalidStartupTimeout,
  kInvalidDestructionTimeout,
  kInvalidDestination,
  kAllocationFailed,
  kWorkerStartFailed,
  kWorkerStartupTimedOut,
  kInvalidFilePath,
  kInvalidWriteBuffers,
  kIoLoopStartFailed,
  kFileOpenFailed,
  kDirectoryCreateFailed,
  kUnsupportedFormat,
  kInvalidAdmissionPolicy,
  kInvalidPriorityReserve,
  kInvalidThinningWatermark,
};

struct RuntimeCreateFailure final {
  RuntimeCreateErrorCode code{RuntimeCreateErrorCode::kAllocationFailed};
  /// Negative libuv error for I/O loop, directory, or file-open failures; zero otherwise.
  /// See IoErrorName().
  std::int32_t io_error{0};

  [[nodiscard]] ULOG_API std::string_view Message() const noexcept;
  [[nodiscard]] ULOG_API std::string_view HowToFix() const noexcept;
};

struct RuntimeSnapshot final {
  std::uint64_t accepted_records{0};
  std::uint64_t processed_records{0};
  std::uint64_t processed_bytes{0};
  std::uint64_t completed_records{0};
  std::uint64_t delivered_records{0};
  std::uint64_t delivered_bytes{0};
  std::uint64_t delivery_failed_records{0};
  std::uint64_t delivery_failed_bytes{0};
  std::uint64_t encoding_failed_records{0};
  std::uint64_t rejected_no_producer{0};
  std::uint64_t rejected_lane_full{0};
  std::uint64_t rejected_budget{0};
  /// Error and Critical calls rejected after both the payload budget and the reserve were full.
  std::uint64_t rejected_priority_reserve{0};
  /// Calls that adaptive thinning dropped before reserving anything.
  std::uint64_t rejected_thinned{0};
  /// Every rejected call: the sum of the five rejection reasons above.
  std::uint64_t dropped_newest_records{0};
  /// Admitted Records that drop-oldest evicted; they are never delivered or counted as rejected.
  std::uint64_t shed_records{0};
  std::uint64_t retained_records{0};
  std::size_t logical_retained_bytes{0};
  std::size_t physical_retained_bytes{0};
  std::size_t payload_capacity_bytes{0};
  std::size_t priority_reserve_bytes{0};
  std::size_t fixed_backing_bytes{0};
  bool admission_open{false};
  bool worker_running{false};
  /// The route stopped after an encoding, write, `fsync`, or close failure.
  bool route_failed{false};
  /// Negative libuv error that failed a file route; zero for other failures or a healthy route.
  std::int32_t route_io_error{0};
};

/// Returns the stable libuv error name for a Ulog I/O error, such as "ENOENT", or "UNKNOWN".
[[nodiscard]] ULOG_API std::string_view IoErrorName(std::int32_t io_error) noexcept;

struct RuntimeCreateResult;

class Runtime final {
 public:
  [[nodiscard]] static ULOG_API RuntimeCreateResult
  Create(RuntimeConfig config, testing::InMemoryDestination destination) noexcept;
  [[nodiscard]] static ULOG_API RuntimeCreateResult
  Create(RuntimeConfig config, testing::InMemoryEncodedDestination destination) noexcept;
  /// Opens the file on a dedicated I/O loop thread before returning a usable Runtime.
  [[nodiscard]] static ULOG_API RuntimeCreateResult
  Create(RuntimeConfig config, const RawFileRouteConfig& route) noexcept;

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;
  ULOG_API ~Runtime();

  /// Returns this Runtime's Logger and prepares the calling thread's bounded producer slot.
  [[nodiscard]] ULOG_API Logger GetLogger() noexcept;
  [[nodiscard]] ULOG_API RuntimeSnapshot GetSnapshot() const noexcept;
  [[nodiscard]] ULOG_API OperationStartResult Drain() noexcept;
  /// Drains through its watermark, then requests `fsync` for a file route on its I/O loop thread.
  [[nodiscard]] ULOG_API OperationStartResult DurableFlush() noexcept;
  /// Closes admission and completes once every accepted Record finished; it has no deadline.
  [[nodiscard]] ULOG_API OperationStartResult Shutdown() noexcept;
  /// Closes admission and drains until `deadline`, then cancels queued and in-flight work and
  /// completes as kCancelled with exact unfinished accounting. A later Shutdown can only bring
  /// the deadline forward, and every Shutdown completes with the same result.
  [[nodiscard]] ULOG_API OperationStartResult
  Shutdown(std::chrono::steady_clock::time_point deadline) noexcept;

 private:
  friend struct detail::RuntimeFactoryAccess;
  struct Impl;
  explicit Runtime(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

struct RuntimeCreateResult final {
  std::unique_ptr<Runtime> runtime{};
  std::optional<RuntimeCreateFailure> failure{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return runtime != nullptr && !failure.has_value();
  }
};

}  // namespace ulog
