#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
                                                     .failed_bytes = kFirstFrame.size()}));
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

}  // namespace
