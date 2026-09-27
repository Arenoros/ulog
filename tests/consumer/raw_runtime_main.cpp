#include <chrono>
#include <cstddef>
#include <optional>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>

namespace {

using namespace std::chrono_literals;
using namespace std::string_view_literals;

[[nodiscard]] ulog::RuntimeConfig SmallRuntimeConfig() {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 1'536,
      .maximum_record_bytes = 512,
      .producer_slots = 1,
      .ingress_cells = 3,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 1s,
      .destruction_timeout = 1s,
  };
}

int RunDeferredDelivery() {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 1,
      .maximum_record_bytes = 512,
  }};
  destination.SetDeliveryMode(ulog::testing::EncodedDeliveryMode::kHold);
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  if (!created) return 20;

  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "held");
  auto drain = created.runtime->Drain();
  if (!drain) return 21;

  std::optional<ulog::testing::PendingEncodedDelivery> pending;
  const auto deadline = std::chrono::steady_clock::now() + 1s;
  while (!pending && std::chrono::steady_clock::now() < deadline) {
    pending = destination.TryTakePendingDelivery();
    if (!pending) std::this_thread::yield();
  }
  if (!pending || pending->AdmissionSequence() != 0U || pending->Bytes() != "tskv\ttext=held\n") {
    return 22;
  }
  if (pending->Complete() != ulog::testing::EncodedDeliveryCompletionStatus::kCompleted) {
    return 23;
  }

  const auto drained = drain.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  if (drained.status != ulog::OperationWaitStatus::kCompleted || !drained.completion) return 24;
  const ulog::OperationReport& report = drained.completion->Report();
  if (report.watermark_records != 1U || report.processed_records != 1U ||
      report.delivered_records != 1U || report.delivered_bytes != 15U ||
      report.failed_records != 0U || report.unfinished_records != 0U) {
    return 25;
  }
  auto frame = destination.TryTake();
  if (!frame || frame->Bytes() != "tskv\ttext=held\n") return 26;

  auto shutdown = created.runtime->Shutdown();
  if (!shutdown) return 27;
  const auto stopped = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  if (stopped.status != ulog::OperationWaitStatus::kCompleted || !stopped.completion ||
      stopped.completion->Outcome() != ulog::OperationOutcome::kSucceeded) {
    return 28;
  }
  return 0;
}

}  // namespace

int main() {
  ulog::testing::InMemoryEncodedDestination destination{{
      .capacity_records = 3,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  if (!created) return 1;

  const ulog::Logger logger = created.runtime->GetLogger();
  LOG_INFO_TO(logger, "");
  LOG_INFO_TO(logger, "hello");
  constexpr std::string_view kMessage =
      "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|\r\n\0\t\\|literal:\\r\\n\\0\\t\\\\"sv;
  constexpr std::string_view kExpectedFrame =
      "tskv\ttext=\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82\xF0\x9F\x8C\x8D|"
      "\\r\\n\\0\\t\\\\|literal:\\\\r\\\\n\\\\0\\\\t\\\\\\\\\n"sv;
  LOG_INFO_TO(logger, "{}", kMessage);

  auto shutdown = created.runtime->Shutdown();
  if (!shutdown) return 2;
  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  if (completed.status != ulog::OperationWaitStatus::kCompleted) return 3;
  if (!completed.completion.has_value()) return 4;
  if (completed.completion->Outcome() != ulog::OperationOutcome::kSucceeded) return 5;

  auto empty = destination.TryTake();
  auto simple = destination.TryTake();
  auto controls = destination.TryTake();
  if (!empty || !simple || !controls) return 6;
  if (empty->AdmissionSequence() != 0U || empty->Bytes() != "tskv\ttext=\n") return 7;
  if (simple->AdmissionSequence() != 1U || simple->Bytes() != "tskv\ttext=hello\n") return 8;
  if (controls->AdmissionSequence() != 2U || controls->Bytes() != kExpectedFrame) {
    return 9;
  }
  if (destination.TryTake().has_value()) return 10;

  const auto snapshot = created.runtime->GetSnapshot();
  if (snapshot.accepted_records != 3U || snapshot.completed_records != 3U ||
      snapshot.delivered_records != 3U || snapshot.delivered_bytes != 90U ||
      snapshot.encoding_failed_records != 0U || snapshot.retained_records != 0U) {
    return 11;
  }
  return RunDeferredDelivery();
}
