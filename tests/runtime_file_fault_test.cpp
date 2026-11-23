#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>

#include "io/raw_file_sink.hpp"
#include "runtime_factory.hpp"
#include "support/temporary_directory.hpp"

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <uv.h>

namespace {

using namespace std::chrono_literals;
using ulog::detail::RuntimeFactoryAccess;
using ulog::detail::io::FileFaultPlan;
using ulog::test_support::ReadFileBytes;
using ulog::test_support::TemporaryDirectory;

template <typename T>
[[nodiscard]] const T& RequireValue(const std::optional<T>& value) {
  if (!value.has_value()) {
    throw std::logic_error{"expected an optional value"};
  }
  return *value;
}

[[nodiscard]] ulog::RuntimeConfig FaultRuntimeConfig() {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 2'048,
      .maximum_record_bytes = 512,
      .producer_slots = 1,
      .ingress_cells = 4,
      .control_operations = 4,
      .worker_threads = 1,
      .startup_timeout = 2s,
      .destruction_timeout = 2s,
  };
}

[[nodiscard]] ulog::OperationResult WaitForResult(ulog::OperationStartResult& started) {
  if (!started) {
    ADD_FAILURE() << (started.failure ? started.failure->Message() : "missing Operation");
    return {};
  }
  const auto completed = started.operation.WaitUntil(std::chrono::steady_clock::now() + 2s);
  if (completed.status != ulog::OperationWaitStatus::kCompleted || !completed.completion) {
    ADD_FAILURE() << "Operation did not complete: " << completed.Message();
    return {};
  }
  return *completed.completion;
}

[[nodiscard]] bool WaitForRouteFailure(const ulog::Runtime& runtime) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto snapshot = runtime.GetSnapshot();
    if (snapshot.route_failed && !snapshot.worker_running) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

constexpr std::string_view kFirstFrame = "tskv\ttext=first\n";
constexpr std::string_view kSecondFrame = "tskv\ttext=second\n";
constexpr std::string_view kThirdFrame = "tskv\ttext=third\n";

TEST(RuntimeFileFaults, PartialWritesContinueFromTheKnownOffsetOnTheLoopThread) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "partial.log";
  std::atomic<std::uint64_t> loop_operations{0};
  std::atomic<std::uint64_t> other_operations{0};
  const FileFaultPlan faults{.maximum_write_bytes = 3,
                             .loop_thread_operations = &loop_operations,
                             .other_thread_operations = &other_operations};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path, .write_buffers = 2}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  LOG_INFO_TO(logger, "second");
  LOG_INFO_TO(logger, "third");
  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(stopped.Report().delivered_records, 3U);
  const std::string expected =
      std::string{kFirstFrame} + std::string{kSecondFrame} + std::string{kThirdFrame};
  EXPECT_EQ(stopped.Report().delivered_bytes, expected.size());
  created.runtime.reset();
  EXPECT_EQ(ReadFileBytes(path), expected);

  // Open, one write per three-byte chunk, and close all ran on the dedicated loop thread.
  const std::uint64_t expected_writes = (kFirstFrame.size() + 2U) / 3U +
                                        (kSecondFrame.size() + 2U) / 3U +
                                        (kThirdFrame.size() + 2U) / 3U;
  EXPECT_EQ(loop_operations.load(), expected_writes + 2U);
  EXPECT_EQ(other_operations.load(), 0U);
}

TEST(RuntimeFileFaults, WriteFailureFailsTheRouteWithoutRetryOrReplay) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "write-failure.log";
  const FileFaultPlan faults{.failing_write_call = 1, .write_error = UV_EIO};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path, .write_buffers = 4}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  LOG_INFO_TO(logger, "second");
  LOG_INFO_TO(logger, "third");
  auto drain = created.runtime->Drain();
  const ulog::OperationResult drained = WaitForResult(drain);
  EXPECT_EQ(drained.Outcome(), ulog::OperationOutcome::kFailed);
  const auto& report = drained.Report();
  EXPECT_EQ(report.watermark_records, 3U);
  EXPECT_EQ(report.delivered_records, 1U);
  EXPECT_EQ(report.delivered_bytes, kFirstFrame.size());
  EXPECT_EQ(report.failed_records, 1U);
  EXPECT_EQ(report.failed_bytes, kSecondFrame.size());
  EXPECT_EQ(report.unfinished_records, 1U);
  EXPECT_EQ(report.delivered_bytes + report.failed_bytes + report.unfinished_bytes,
            report.processed_bytes);
  EXPECT_EQ(report.io_error, UV_EIO);
  EXPECT_EQ(report.durability, ulog::OperationDurability::kNotRequested);

  ASSERT_TRUE(WaitForRouteFailure(*created.runtime));
  const auto snapshot = created.runtime->GetSnapshot();
  EXPECT_EQ(snapshot.route_io_error, UV_EIO);
  EXPECT_EQ(ulog::IoErrorName(snapshot.route_io_error), "EIO");
  EXPECT_FALSE(snapshot.admission_open);

  auto late_drain = created.runtime->Drain();
  EXPECT_EQ(WaitForResult(late_drain).Outcome(), ulog::OperationOutcome::kFailed);
  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kFailed);
  EXPECT_EQ(stopped.Report().failed_records, 1U);
  created.runtime.reset();
  EXPECT_EQ(ReadFileBytes(path), kFirstFrame);
}

TEST(RuntimeFileFaults, FailureAfterAPartialWriteNeverReplaysThatFrame) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "partial-failure.log";
  const FileFaultPlan faults{
      .maximum_write_bytes = 4, .failing_write_call = 1, .write_error = UV_ENOSPC};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path, .write_buffers = 2}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  auto drain = created.runtime->Drain();
  const ulog::OperationResult drained = WaitForResult(drain);
  EXPECT_EQ(drained.Outcome(), ulog::OperationOutcome::kFailed);
  EXPECT_EQ(drained.Report(), (ulog::OperationReport{.watermark_records = 1,
                                                     .processed_records = 1,
                                                     .processed_bytes = kFirstFrame.size(),
                                                     .failed_records = 1,
                                                     .failed_bytes = kFirstFrame.size(),
                                                     .io_error = UV_ENOSPC}));
  ASSERT_TRUE(WaitForRouteFailure(*created.runtime));
  EXPECT_EQ(created.runtime->GetSnapshot().route_io_error, UV_ENOSPC);
  created.runtime.reset();
  EXPECT_EQ(ReadFileBytes(path), kFirstFrame.substr(0, 4));
}

TEST(RuntimeFileFaults, CloseFailureFailsShutdownAfterDelivery) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "close-failure.log";
  const FileFaultPlan faults{.close_error = UV_EIO};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kFailed);
  EXPECT_EQ(stopped.Report().delivered_records, 1U);
  EXPECT_EQ(stopped.Report().unfinished_records, 0U);
  const auto snapshot = created.runtime->GetSnapshot();
  EXPECT_TRUE(snapshot.route_failed);
  EXPECT_EQ(snapshot.route_io_error, UV_EIO);
  created.runtime.reset();
  EXPECT_EQ(ReadFileBytes(path), kFirstFrame);
}

TEST(RuntimeFileFaults, InjectedOpenFailureIsACreationResult) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "never-opened.log";
  const FileFaultPlan faults{.open_error = UV_EACCES};
  const auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_FALSE(created);
  ASSERT_TRUE(created.failure.has_value());
  EXPECT_EQ(RequireValue(created.failure).code, ulog::RuntimeCreateErrorCode::kFileOpenFailed);
  EXPECT_EQ(RequireValue(created.failure).io_error, UV_EACCES);
  EXPECT_EQ(ulog::IoErrorName(RequireValue(created.failure).io_error), "EACCES");
  EXPECT_FALSE(std::filesystem::exists(path));
}

struct OpenObservation final {
  std::uint64_t loop_operations{0};
  std::uint64_t other_operations{0};
  std::string bytes;
};

/// Opens and shuts down a route without Records, counting its filesystem operations.
[[nodiscard]] OpenObservation OpenAndClose(const ulog::RawFileRouteConfig& route) {
  std::atomic<std::uint64_t> loop_operations{0};
  std::atomic<std::uint64_t> other_operations{0};
  const FileFaultPlan faults{.loop_thread_operations = &loop_operations,
                             .other_thread_operations = &other_operations};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(FaultRuntimeConfig(), route, faults);
  if (!created) {
    ADD_FAILURE() << (created.failure ? created.failure->Message() : "missing Runtime");
    return {};
  }
  auto shutdown = created.runtime->Shutdown();
  EXPECT_EQ(WaitForResult(shutdown).Outcome(), ulog::OperationOutcome::kSucceeded);
  created.runtime.reset();
  return {.loop_operations = loop_operations.load(),
          .other_operations = other_operations.load(),
          .bytes = ReadFileBytes(route.path)};
}

void WriteFile(const std::filesystem::path& path, std::string_view bytes) {
  std::ofstream file{path, std::ios::binary};
  file << bytes;
}

TEST(RuntimeFileFaults, OpenCreateSeparateAndTruncateStepsRunOnTheLoopThread) {
  const TemporaryDirectory directory;

  // Missing open, mkdir of the missing parent, mkdir of its parent, mkdir again, open, close.
  const auto nested =
      OpenAndClose(ulog::RawFileRouteConfig{.path = directory.Path() / "a" / "b" / "app.log"});
  EXPECT_EQ(nested.loop_operations, 6U);
  EXPECT_EQ(nested.other_operations, 0U);
  EXPECT_TRUE(std::filesystem::is_directory(directory.Path() / "a" / "b"));

  // An existing parent needs only the open and the close.
  const auto existing = OpenAndClose(ulog::RawFileRouteConfig{.path = directory.Path() / "a.log"});
  EXPECT_EQ(existing.loop_operations, 2U);
  EXPECT_EQ(existing.other_operations, 0U);

  // Open, read-only open, read of the last byte, its close, separator write, close.
  const std::filesystem::path partial = directory.Path() / "partial.log";
  WriteFile(partial, "partial");
  const auto separated = OpenAndClose(ulog::RawFileRouteConfig{.path = partial});
  EXPECT_EQ(separated.loop_operations, 6U);
  EXPECT_EQ(separated.other_operations, 0U);
  EXPECT_EQ(separated.bytes, "partial\n");

  // A final newline needs no separator write.
  const std::filesystem::path line = directory.Path() / "line.log";
  WriteFile(line, "line\n");
  const auto unseparated = OpenAndClose(ulog::RawFileRouteConfig{.path = line});
  EXPECT_EQ(unseparated.loop_operations, 5U);
  EXPECT_EQ(unseparated.other_operations, 0U);
  EXPECT_EQ(unseparated.bytes, "line\n");

  // Truncation is part of the open, so the empty result needs no inspection.
  const auto truncated =
      OpenAndClose(ulog::RawFileRouteConfig{.path = partial, .truncate_on_start = true});
  EXPECT_EQ(truncated.loop_operations, 2U);
  EXPECT_EQ(truncated.other_operations, 0U);
  EXPECT_EQ(truncated.bytes, "");
}

TEST(RuntimeFileFaults, DirectoryCreationFailureIsACreationResult) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "missing" / "app.log";
  std::atomic<std::uint64_t> loop_operations{0};
  std::atomic<std::uint64_t> other_operations{0};
  const FileFaultPlan faults{.directory_error = UV_EACCES,
                             .loop_thread_operations = &loop_operations,
                             .other_thread_operations = &other_operations};
  const auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_FALSE(created);
  ASSERT_TRUE(created.failure.has_value());
  EXPECT_EQ(RequireValue(created.failure).code,
            ulog::RuntimeCreateErrorCode::kDirectoryCreateFailed);
  EXPECT_EQ(RequireValue(created.failure).io_error, UV_EACCES);
  EXPECT_FALSE(RequireValue(created.failure).Message().empty());
  EXPECT_NE(RequireValue(created.failure).HowToFix().find("permission"), std::string_view::npos);
  EXPECT_FALSE(std::filesystem::exists(directory.Path() / "missing"));
  // The failed open and the one rejected directory creation; nothing is retried.
  EXPECT_EQ(loop_operations.load(), 2U);
  EXPECT_EQ(other_operations.load(), 0U);
}

[[nodiscard]] ulog::OperationReport DeliveredReport(std::uint64_t records, std::uint64_t bytes,
                                                    ulog::OperationDurability durability) {
  return ulog::OperationReport{.watermark_records = records,
                               .processed_records = records,
                               .processed_bytes = bytes,
                               .delivered_records = records,
                               .delivered_bytes = bytes,
                               .durability = durability};
}

TEST(RuntimeFileFaults, DurableFlushSyncsOnTheLoopThreadAndDrainNeverDoes) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "durable.log";
  std::atomic<std::uint64_t> loop_operations{0};
  std::atomic<std::uint64_t> other_operations{0};
  std::atomic<std::uint64_t> sync_operations{0};
  const FileFaultPlan faults{.loop_thread_operations = &loop_operations,
                             .other_thread_operations = &other_operations,
                             .sync_operations = &sync_operations};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  auto drain = created.runtime->Drain();
  const ulog::OperationResult drained = WaitForResult(drain);
  EXPECT_EQ(drained.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(drained.Report(),
            DeliveredReport(1, kFirstFrame.size(), ulog::OperationDurability::kNotRequested));
  EXPECT_EQ(sync_operations.load(), 0U);

  LOG_INFO_TO(logger, "second");
  auto flush = created.runtime->DurableFlush();
  const ulog::OperationResult flushed = WaitForResult(flush);
  const std::uint64_t both = kFirstFrame.size() + kSecondFrame.size();
  EXPECT_EQ(flushed.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(flushed.Report(), DeliveredReport(2, both, ulog::OperationDurability::kSynced));
  EXPECT_EQ(sync_operations.load(), 1U);

  auto late_drain = created.runtime->Drain();
  EXPECT_EQ(WaitForResult(late_drain).Report(),
            DeliveredReport(2, both, ulog::OperationDurability::kNotRequested));
  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(stopped.Report(), DeliveredReport(2, both, ulog::OperationDurability::kNotRequested));
  created.runtime.reset();
  EXPECT_EQ(sync_operations.load(), 1U);
  EXPECT_EQ(ReadFileBytes(path), std::string{kFirstFrame} + std::string{kSecondFrame});
  // Open, two appends, one fsync, and close, all on the loop thread.
  EXPECT_EQ(loop_operations.load(), 5U);
  EXPECT_EQ(other_operations.load(), 0U);
}

TEST(RuntimeFileFaults, SyncFailureFailsTheRouteWithTheIoError) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "sync-failure.log";
  std::atomic<std::uint64_t> sync_operations{0};
  const FileFaultPlan faults{.sync_error = UV_EIO, .sync_operations = &sync_operations};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  auto flush = created.runtime->DurableFlush();
  const ulog::OperationResult flushed = WaitForResult(flush);
  EXPECT_EQ(flushed.Outcome(), ulog::OperationOutcome::kFailed);
  auto expected = DeliveredReport(1, kFirstFrame.size(), ulog::OperationDurability::kNotSynced);
  expected.io_error = UV_EIO;
  EXPECT_EQ(flushed.Report(), expected);

  ASSERT_TRUE(WaitForRouteFailure(*created.runtime));
  const auto snapshot = created.runtime->GetSnapshot();
  EXPECT_EQ(snapshot.route_io_error, UV_EIO);
  EXPECT_FALSE(snapshot.admission_open);

  // The failed sync is not retried, and a later flush cannot claim durability.
  auto late_flush = created.runtime->DurableFlush();
  const ulog::OperationResult late = WaitForResult(late_flush);
  EXPECT_EQ(late.Outcome(), ulog::OperationOutcome::kFailed);
  EXPECT_EQ(late.Report(), expected);
  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kFailed);
  EXPECT_EQ(stopped.Report().durability, ulog::OperationDurability::kNotRequested);
  EXPECT_EQ(stopped.Report().io_error, UV_EIO);
  created.runtime.reset();
  EXPECT_EQ(sync_operations.load(), 1U);
  EXPECT_EQ(ReadFileBytes(path), kFirstFrame);
}

TEST(RuntimeFileFaults, ShutdownWaitsForStartedDurableFlushesAndLaterOnesFail) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "flush-shutdown.log";
  std::atomic<std::uint64_t> sync_operations{0};
  const FileFaultPlan faults{.sync_operations = &sync_operations};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  auto first_flush = created.runtime->DurableFlush();
  auto second_flush = created.runtime->DurableFlush();
  auto shutdown = created.runtime->Shutdown();
  const auto synced = DeliveredReport(1, kFirstFrame.size(), ulog::OperationDurability::kSynced);
  const ulog::OperationResult first = WaitForResult(first_flush);
  EXPECT_EQ(first.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(first.Report(), synced);
  const ulog::OperationResult second = WaitForResult(second_flush);
  EXPECT_EQ(second.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(second.Report(), synced);
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kSucceeded);
  // Concurrent flushes may share one fsync.
  const std::uint64_t syncs = sync_operations.load();
  EXPECT_GE(syncs, 1U);
  EXPECT_LE(syncs, 2U);

  // The closed file can no longer be synced, so the flush fails without claiming durability.
  auto closed_flush = created.runtime->DurableFlush();
  const ulog::OperationResult closed = WaitForResult(closed_flush);
  EXPECT_EQ(closed.Outcome(), ulog::OperationOutcome::kFailed);
  EXPECT_EQ(closed.Report(),
            DeliveredReport(1, kFirstFrame.size(), ulog::OperationDurability::kNotSynced));
  EXPECT_EQ(sync_operations.load(), syncs);
}

// Shared by the stuck-write tests: a detached loop thread may read it after its test returns.
std::atomic<bool> stuck_write_gate{false};

[[nodiscard]] bool WaitForProcessed(const ulog::Runtime& runtime, std::uint64_t records) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (runtime.GetSnapshot().processed_records == records) {
      return true;
    }
    std::this_thread::yield();
  }
  return false;
}

[[nodiscard]] bool WaitForLoopOperations(const std::atomic<std::uint64_t>& operations,
                                         std::uint64_t expected) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (operations.load() < expected && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  return operations.load() == expected;
}

TEST(RuntimeFileFaults, ShutdownDeadlineCancelsAStuckWriteWithExactAccounting) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "stuck-shutdown.log";
  stuck_write_gate.store(false);
  std::atomic<std::uint64_t> loop_operations{0};
  const FileFaultPlan faults{.gated_write_call = 0,
                             .write_gate = &stuck_write_gate,
                             .loop_thread_operations = &loop_operations};
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      FaultRuntimeConfig(), ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  LOG_INFO_TO(logger, "second");
  ASSERT_TRUE(WaitForProcessed(*created.runtime, 2U));
  // The open and the stuck first append have been issued; the second append stays queued.
  ASSERT_TRUE(WaitForLoopOperations(loop_operations, 2U));
  const auto started = std::chrono::steady_clock::now();
  auto shutdown = created.runtime->Shutdown(started + 100ms);
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kCancelled);
  const std::uint64_t both = kFirstFrame.size() + kSecondFrame.size();
  // The stuck append and the queued one have no outcome, so both are unfinished.
  EXPECT_EQ(stopped.Report(), (ulog::OperationReport{.watermark_records = 2,
                                                     .processed_records = 2,
                                                     .processed_bytes = both,
                                                     .unfinished_records = 2,
                                                     .unfinished_bytes = both}));
  EXPECT_GE(elapsed, 100ms);
  EXPECT_LT(elapsed, 1s);
  const auto snapshot = created.runtime->GetSnapshot();
  EXPECT_FALSE(snapshot.worker_running);
  EXPECT_FALSE(snapshot.admission_open);
  EXPECT_FALSE(snapshot.route_failed);

  auto repeated = created.runtime->Shutdown();
  EXPECT_EQ(WaitForResult(repeated), stopped);
  auto drain = created.runtime->Drain();
  EXPECT_EQ(WaitForResult(drain).Outcome(), ulog::OperationOutcome::kCancelled);

  // A submitted append cannot be cancelled: it finishes late, and the queued frame never runs.
  stuck_write_gate.store(true);
  created.runtime.reset();
  EXPECT_EQ(ReadFileBytes(path), kFirstFrame);
}

TEST(RuntimeFileFaults, DestructionStaysBoundedWhileAWriteIsStuck) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "stuck-destruction.log";
  stuck_write_gate.store(false);
  // Static because the detached loop thread may count after this test returns.
  static std::atomic<std::uint64_t> loop_operations{0};
  loop_operations.store(0);
  const FileFaultPlan faults{.gated_write_call = 0,
                             .write_gate = &stuck_write_gate,
                             .loop_thread_operations = &loop_operations};
  auto config = FaultRuntimeConfig();
  config.destruction_timeout = 200ms;
  auto created = RuntimeFactoryAccess::CreateRawFileRuntime(
      config, ulog::RawFileRouteConfig{.path = path}, faults);
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  // The open and the submitted append; a queued append would just be cancelled.
  ASSERT_TRUE(WaitForLoopOperations(loop_operations, 2U));
  const auto started = std::chrono::steady_clock::now();
  created.runtime.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);

  // The detached loop thread finishes the released append and closes the file on its own.
  stuck_write_gate.store(true);
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (ReadFileBytes(path) != kFirstFrame && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(1ms);
  }
  EXPECT_EQ(ReadFileBytes(path), kFirstFrame);
  // Let the loop thread close the file before the directory is removed.
  std::this_thread::sleep_for(50ms);
}

}  // namespace
