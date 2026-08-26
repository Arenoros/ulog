#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <ulog/export.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>

namespace ulog {

namespace testing {
class InMemoryDestination;
class InMemoryEncodedDestination;
}  // namespace testing

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
};

struct RuntimeCreateFailure final {
  RuntimeCreateErrorCode code{RuntimeCreateErrorCode::kAllocationFailed};

  [[nodiscard]] ULOG_API std::string_view Message() const noexcept;
  [[nodiscard]] ULOG_API std::string_view HowToFix() const noexcept;
};

struct RuntimeSnapshot final {
  std::uint64_t accepted_records{0};
  std::uint64_t completed_records{0};
  std::uint64_t delivered_records{0};
  std::uint64_t delivered_bytes{0};
  std::uint64_t encoding_failed_records{0};
  std::uint64_t rejected_no_producer{0};
  std::uint64_t rejected_lane_full{0};
  std::uint64_t rejected_budget{0};
  std::uint64_t dropped_newest_records{0};
  std::uint64_t retained_records{0};
  std::size_t logical_retained_bytes{0};
  std::size_t physical_retained_bytes{0};
  std::size_t payload_capacity_bytes{0};
  std::size_t fixed_backing_bytes{0};
  bool admission_open{false};
  bool worker_running{false};
};

struct RuntimeCreateResult;

class Runtime final {
 public:
  [[nodiscard]] static ULOG_API RuntimeCreateResult
  Create(RuntimeConfig config, testing::InMemoryDestination destination) noexcept;
  [[nodiscard]] static ULOG_API RuntimeCreateResult
  Create(RuntimeConfig config, testing::InMemoryEncodedDestination destination) noexcept;

  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
  Runtime(Runtime&&) = delete;
  Runtime& operator=(Runtime&&) = delete;
  ULOG_API ~Runtime();

  /// Returns this Runtime's Logger and prepares the calling thread's bounded producer slot.
  [[nodiscard]] ULOG_API Logger GetLogger() noexcept;
  [[nodiscard]] ULOG_API RuntimeSnapshot GetSnapshot() const noexcept;
  [[nodiscard]] ULOG_API OperationStartResult Drain() noexcept;
  [[nodiscard]] ULOG_API OperationStartResult Shutdown() noexcept;

 private:
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
