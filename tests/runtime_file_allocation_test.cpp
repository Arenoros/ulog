#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>

#include "support/allocation_interposer.hpp"
#include "support/temporary_directory.hpp"

namespace {

namespace allocation_tracking = ulog::benchmark_support::allocation_tracking;

using namespace std::chrono_literals;

constexpr std::uint64_t kCycles = 256;
constexpr std::string_view kMessage = "allocation-free-file-runtime";
constexpr std::string_view kExpectedFrame = "tskv\ttext=allocation-free-file-runtime\n";

[[nodiscard]] bool WaitSucceeded(ulog::OperationStartResult& started,
                                 std::uint64_t expected_records) noexcept {
  if (!started) {
    std::fprintf(stderr, "file runtime allocation test: control start failed: in_use=%zu\n",
                 started.failure ? started.failure->controls_in_use : 0U);
    return false;
  }
  const auto waited = started.operation.WaitUntil(std::chrono::steady_clock::now() + 2s);
  if (waited.status != ulog::OperationWaitStatus::kCompleted || !waited.completion ||
      waited.completion->Outcome() != ulog::OperationOutcome::kSucceeded) {
    std::fprintf(stderr, "file runtime allocation test: wait failed: status=%u\n",
                 static_cast<unsigned>(waited.status));
    return false;
  }
  const auto& report = waited.completion->Report();
  return report.delivered_records == expected_records &&
         report.delivered_bytes == expected_records * kExpectedFrame.size() &&
         report.unfinished_records == 0U;
}

[[nodiscard]] bool Run() {
  const ulog::test_support::TemporaryDirectory directory;
  const std::filesystem::path path = directory.Path() / "allocation.log";
  auto created = ulog::Runtime::Create(
      ulog::RuntimeConfig{
          .threshold = ulog::Level::kTrace,
          .payload_capacity_bytes = 512,
          .maximum_record_bytes = 512,
          .producer_slots = 1,
          .ingress_cells = 1,
          .control_operations = 2,
          .worker_threads = 1,
          .startup_timeout = 2s,
          .destruction_timeout = 2s,
      },
      ulog::RawFileRouteConfig{.path = path, .write_buffers = 1});
  if (!created) {
    const std::string_view message =
        created.failure ? created.failure->Message() : "missing Runtime";
    std::fprintf(stderr, "file runtime allocation test: creation failed: %.*s\n",
                 static_cast<int>(message.size()), message.data());
    return false;
  }

  ulog::Runtime& runtime = *created.runtime;
  const ulog::Logger logger = runtime.GetLogger();
  LOG_INFO_TO(logger, kMessage);
  {
    auto warm_up = runtime.Drain();
    if (!WaitSucceeded(warm_up, 1U)) {
      std::fputs("file runtime allocation test: warm-up failed\n", stderr);
      return false;
    }
  }

  const std::uint64_t allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  bool valid = true;
  for (std::uint64_t cycle = 0; valid && cycle < kCycles; ++cycle) {
    LOG_INFO_TO(logger, kMessage);
    auto drain = runtime.Drain();
    valid = WaitSucceeded(drain, cycle + 2U);
  }
  const std::uint64_t allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);

  auto shutdown = runtime.Shutdown();
  valid = WaitSucceeded(shutdown, kCycles + 1U) && valid;
  created.runtime.reset();

  std::string expected;
  for (std::uint64_t record = 0; record < kCycles + 1U; ++record) {
    expected += kExpectedFrame;
  }
  const bool file_valid = ulog::test_support::ReadFileBytes(path) == expected;
  if (allocations_after != allocations_before || !file_valid) {
    std::fprintf(stderr, "file runtime allocation test: allocations=%llu file_valid=%d\n",
                 static_cast<unsigned long long>(allocations_after - allocations_before),
                 file_valid ? 1 : 0);
  }
  return valid && file_valid && allocations_after == allocations_before;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "file runtime allocation test failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("file runtime allocation test failed with an unknown exception\n", stderr);
    return 1;
  }
}
