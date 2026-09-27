#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::filesystem::path UniqueLogPath() {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("ulog-file-consumer-" + std::to_string(nonce) + ".log");
}

[[nodiscard]] int WriteThroughRuntime(const std::filesystem::path& path) {
  auto created = ulog::Runtime::Create(
      ulog::RuntimeConfig{
          .threshold = ulog::Level::kTrace,
          .payload_capacity_bytes = 1'536,
          .maximum_record_bytes = 512,
          .producer_slots = 1,
          .ingress_cells = 3,
          .control_operations = 2,
          .worker_threads = 1,
          .startup_timeout = 2s,
          .destruction_timeout = 2s,
      },
      ulog::RawFileRouteConfig{.path = path, .write_buffers = 2});
  if (!created) return 1;

  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "first");
  LOG_WARNING_TO(logger, "second {}", 2);

  auto shutdown = created.runtime->Shutdown();
  if (!shutdown) return 2;
  const auto stopped = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 2s);
  if (stopped.status != ulog::OperationWaitStatus::kCompleted || !stopped.completion ||
      stopped.completion->Outcome() != ulog::OperationOutcome::kSucceeded) {
    return 3;
  }
  const ulog::OperationReport& report = stopped.completion->Report();
  if (report.delivered_records != 2U || report.delivered_bytes != 35U ||
      report.unfinished_records != 0U) {
    return 4;
  }
  return 0;
}

}  // namespace

int main() {
  const std::filesystem::path path = UniqueLogPath();
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  int status = WriteThroughRuntime(path);
  if (status == 0) {
    std::ifstream input{path, std::ios::binary};
    const std::string bytes{std::istreambuf_iterator<char>{input},
                            std::istreambuf_iterator<char>{}};
    if (bytes != "tskv\ttext=first\ntskv\ttext=second 2\n") status = 5;
  }
  std::filesystem::remove(path, ignored);
  return status;
}
