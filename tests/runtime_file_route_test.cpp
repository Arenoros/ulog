#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>

#include "support/temporary_directory.hpp"

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;
using ulog::test_support::ReadFileBytes;
using ulog::test_support::TemporaryDirectory;

template <typename T>
[[nodiscard]] const T& RequireValue(const std::optional<T>& value) {
  if (!value.has_value()) {
    throw std::logic_error{"expected an optional value"};
  }
  return *value;
}

[[nodiscard]] ulog::RuntimeConfig FileRuntimeConfig() {
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

TEST(RuntimeFileRoute, AppendsExactFifoRawFramesAndReportsLocalCompletion) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "app.log";
  auto created = ulog::Runtime::Create(FileRuntimeConfig(),
                                       ulog::RawFileRouteConfig{.path = path, .write_buffers = 2});
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  LOG_INFO_TO(logger, "first");
  LOG_WARNING_TO(logger, "second {}", 2);
  LOG_ERROR_TO(logger, "tab\there");
  auto drain = created.runtime->Drain();
  const ulog::OperationResult drained = WaitForResult(drain);
  constexpr std::string_view kExpected =
      "tskv\ttext=first\ntskv\ttext=second 2\ntskv\ttext=tab\\there\n";
  EXPECT_EQ(drained.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(drained.Report().watermark_records, 3U);
  EXPECT_EQ(drained.Report().delivered_records, 3U);
  EXPECT_EQ(drained.Report().delivered_bytes, kExpected.size());
  EXPECT_EQ(drained.Report().failed_records, 0U);
  EXPECT_EQ(drained.Report().unfinished_records, 0U);
  EXPECT_EQ(ReadFileBytes(path), kExpected);

  const auto snapshot = created.runtime->GetSnapshot();
  EXPECT_EQ(snapshot.delivered_records, 3U);
  EXPECT_EQ(snapshot.delivered_bytes, kExpected.size());
  EXPECT_FALSE(snapshot.route_failed);
  EXPECT_EQ(snapshot.route_io_error, 0);
  EXPECT_GT(snapshot.fixed_backing_bytes, 2U * 512U);

  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(stopped.Report(), drained.Report());
  EXPECT_FALSE(created.runtime->GetSnapshot().worker_running);
  created.runtime.reset();
  EXPECT_EQ(ReadFileBytes(path), kExpected);
}

TEST(RuntimeFileRoute, AppendOpenKeepsExistingBytes) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "existing.log";
  {
    std::ofstream existing{path, std::ios::binary};
    existing << "existing line\n";
  }
  auto created = ulog::Runtime::Create(FileRuntimeConfig(), ulog::RawFileRouteConfig{.path = path});
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "appended");
  auto shutdown = created.runtime->Shutdown();
  EXPECT_EQ(WaitForResult(shutdown).Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(ReadFileBytes(path), "existing line\ntskv\ttext=appended\n");
}

TEST(RuntimeFileRoute, UnicodeNativePathReceivesExactUtf8Bytes) {
  const TemporaryDirectory directory;
  const std::filesystem::path path =
      directory.Path() / std::filesystem::path{u8"журнал-ü-\U0001F600.log"};
  auto created = ulog::Runtime::Create(FileRuntimeConfig(), ulog::RawFileRouteConfig{.path = path});
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x8C\x8D");
  auto shutdown = created.runtime->Shutdown();
  EXPECT_EQ(WaitForResult(shutdown).Outcome(), ulog::OperationOutcome::kSucceeded);
  ASSERT_TRUE(std::filesystem::exists(path));
  EXPECT_EQ(ReadFileBytes(path),
            "tskv\ttext=\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 \xF0\x9F\x8C\x8D\n");
}

TEST(RuntimeFileRoute, InvalidPathAndCapacityAreRejectedWithGuidance) {
  const TemporaryDirectory directory;
  const auto expect_failure = [](const ulog::RuntimeCreateResult& result,
                                 ulog::RuntimeCreateErrorCode code, std::string_view hint) {
    ASSERT_FALSE(result);
    ASSERT_TRUE(result.failure.has_value());
    EXPECT_EQ(RequireValue(result.failure).code, code);
    EXPECT_EQ(RequireValue(result.failure).io_error, 0);
    EXPECT_FALSE(RequireValue(result.failure).Message().empty());
    EXPECT_NE(RequireValue(result.failure).HowToFix().find(hint), std::string_view::npos);
  };

  expect_failure(ulog::Runtime::Create(FileRuntimeConfig(), ulog::RawFileRouteConfig{}),
                 ulog::RuntimeCreateErrorCode::kInvalidFilePath, "path");
  expect_failure(
      ulog::Runtime::Create(FileRuntimeConfig(),
                            ulog::RawFileRouteConfig{.path = directory.Path() / "logs" / ""}),
      ulog::RuntimeCreateErrorCode::kInvalidFilePath, "directory");
  expect_failure(ulog::Runtime::Create(FileRuntimeConfig(),
                                       ulog::RawFileRouteConfig{.path = directory.Path() / "a.log",
                                                                .write_buffers = 0}),
                 ulog::RuntimeCreateErrorCode::kInvalidWriteBuffers, "write_buffers");
  expect_failure(ulog::Runtime::Create(FileRuntimeConfig(),
                                       ulog::RawFileRouteConfig{.path = directory.Path() / "a.log",
                                                                .write_buffers = 65}),
                 ulog::RuntimeCreateErrorCode::kInvalidWriteBuffers, "write_buffers");
  auto invalid_runtime = FileRuntimeConfig();
  invalid_runtime.worker_threads = 2;
  expect_failure(ulog::Runtime::Create(
                     invalid_runtime, ulog::RawFileRouteConfig{.path = directory.Path() / "a.log"}),
                 ulog::RuntimeCreateErrorCode::kInvalidWorkerCount, "worker_threads");
  EXPECT_FALSE(std::filesystem::exists(directory.Path() / "a.log"));
}

TEST(RuntimeFileRoute, OpenFailuresReportTheIoErrorAndCorrection) {
  const TemporaryDirectory directory;
  const auto missing_parent = ulog::Runtime::Create(
      FileRuntimeConfig(),
      ulog::RawFileRouteConfig{.path = directory.Path() / "missing" / "app.log"});
  ASSERT_FALSE(missing_parent);
  ASSERT_TRUE(missing_parent.failure.has_value());
  EXPECT_EQ(RequireValue(missing_parent.failure).code,
            ulog::RuntimeCreateErrorCode::kFileOpenFailed);
  EXPECT_LT(RequireValue(missing_parent.failure).io_error, 0);
  EXPECT_EQ(ulog::IoErrorName(RequireValue(missing_parent.failure).io_error), "ENOENT");
  EXPECT_NE(RequireValue(missing_parent.failure).HowToFix().find("parent directory"),
            std::string_view::npos);

  const auto directory_path = ulog::Runtime::Create(
      FileRuntimeConfig(), ulog::RawFileRouteConfig{.path = directory.Path()});
  ASSERT_FALSE(directory_path);
  ASSERT_TRUE(directory_path.failure.has_value());
  EXPECT_EQ(RequireValue(directory_path.failure).code,
            ulog::RuntimeCreateErrorCode::kFileOpenFailed);
  EXPECT_LT(RequireValue(directory_path.failure).io_error, 0);
  EXPECT_EQ(ulog::IoErrorName(RequireValue(directory_path.failure).io_error), "EISDIR");
  EXPECT_EQ(ulog::IoErrorName(0), "UNKNOWN");
}

TEST(RuntimeFileRoute, OneWriteBufferAppliesBoundedPressureWithoutLosingAcceptedFrames) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "pressure.log";
  auto config = FileRuntimeConfig();
  config.ingress_cells = 1;
  config.payload_capacity_bytes = 512;
  auto created =
      ulog::Runtime::Create(config, ulog::RawFileRouteConfig{.path = path, .write_buffers = 1});
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();

  constexpr std::size_t kAttempts = 2'000;
  std::size_t evaluations = 0;
  for (std::size_t attempt = 0; attempt < kAttempts; ++attempt) {
    LOG_INFO_TO(logger, "record {}", ++evaluations);
  }
  auto shutdown = created.runtime->Shutdown();
  const ulog::OperationResult stopped = WaitForResult(shutdown);
  const auto snapshot = created.runtime->GetSnapshot();
  EXPECT_EQ(stopped.Outcome(), ulog::OperationOutcome::kSucceeded);
  EXPECT_EQ(snapshot.accepted_records, evaluations);
  EXPECT_EQ(snapshot.accepted_records + snapshot.dropped_newest_records, kAttempts);
  EXPECT_EQ(stopped.Report().delivered_records, snapshot.accepted_records);

  // Only accepted calls evaluate their operand, so accepted Records are numbered 1..N in FIFO
  // order.
  std::string expected;
  for (std::size_t record = 1; record <= evaluations; ++record) {
    expected += "tskv\ttext=record " + std::to_string(record) + "\n";
  }
  EXPECT_EQ(ReadFileBytes(path), expected);
}

TEST(RuntimeFileRoute, DestructionWithoutShutdownIsBoundedAndLeavesOnlyWholeFrames) {
  const TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "destroyed.log";
  auto config = FileRuntimeConfig();
  config.destruction_timeout = 1s;
  auto created = ulog::Runtime::Create(config, ulog::RawFileRouteConfig{.path = path});
  ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
  const ulog::Logger logger = created.runtime->GetLogger();
  for (int record = 0; record < 64; ++record) {
    LOG_INFO_TO(logger, "whole");
  }
  auto drain = created.runtime->Drain();

  const auto started = std::chrono::steady_clock::now();
  created.runtime.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1500ms);
  const ulog::OperationResult result = WaitForResult(drain);
  EXPECT_TRUE(result.Outcome() == ulog::OperationOutcome::kSucceeded ||
              result.Outcome() == ulog::OperationOutcome::kCancelled);
  const auto& report = result.Report();
  EXPECT_EQ(report.delivered_records + report.failed_records + report.unfinished_records,
            report.watermark_records);

  const std::string bytes = ReadFileBytes(path);
  constexpr std::string_view kFrame = "tskv\ttext=whole\n";
  EXPECT_EQ(bytes.size() % kFrame.size(), 0U);
  for (std::size_t offset = 0; offset < bytes.size(); offset += kFrame.size()) {
    EXPECT_EQ(std::string_view{bytes}.substr(offset, kFrame.size()), kFrame);
  }
  EXPECT_GE(bytes.size() / kFrame.size(), report.delivered_records);
}

}  // namespace
