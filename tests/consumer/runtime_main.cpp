#include <chrono>
#include <cstddef>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_destination.hpp>

namespace {

using namespace std::chrono_literals;

[[nodiscard]] ulog::RuntimeConfig SmallRuntimeConfig() {
  return ulog::RuntimeConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 256,
      .maximum_record_bytes = 256,
      .producer_slots = 1,
      .ingress_cells = 1,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 1s,
      .destruction_timeout = 1s,
  };
}

}  // namespace

int main() {
  ulog::testing::InMemoryDestination destination{{
      .capacity_records = 2,
      .maximum_record_bytes = 256,
  }};
  auto created = ulog::Runtime::Create(SmallRuntimeConfig(), destination);
  if (!created) return 1;

  ulog::Runtime& runtime = *created.runtime;
  const ulog::Logger logger = runtime.GetLogger();
  LOG_INFO_TO(logger, "answer={}", 42);

  auto shutdown = runtime.Shutdown();
  if (shutdown.failure.has_value()) return 2;
  if (!shutdown.operation) return 3;

  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  if (completed.status != ulog::OperationWaitStatus::kCompleted) return 4;
  if (!completed.completion.has_value()) return 5;
  if (completed.completion->Outcome() != ulog::OperationOutcome::kSucceeded) return 6;

  std::size_t late_evaluations = 0;
  LOG_INFO_TO(logger, "late={}", ++late_evaluations);
  if (late_evaluations != 0) return 7;

  const auto record = destination.TryTake();
  if (!record.has_value()) return 8;
  if (record->AdmissionSequence() != 0) return 9;
  if (record->GetLevel() != ulog::Level::kInfo) return 10;
  if (record->Message() != "answer=42") return 11;
  if (destination.TryTake().has_value()) return 12;
  return 0;
}
