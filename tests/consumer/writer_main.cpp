// Uses the scope writer's extension points through the installed package: a native writer overload
// for an application type and a lazy writer invocable that runs only after admission (VAL-005).

#include <chrono>
#include <cstddef>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/log.hpp>
#include <ulog/log_writer.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <vector>

namespace app {

struct Point final {
  int x;
  int y;
};

ulog::LogWriter& operator<<(ulog::LogWriter& writer, const Point& point) {
  return writer << '(' << point.x << ", " << point.y << ')';
}

}  // namespace app

int main() {
  using namespace std::chrono_literals;
  ulog::testing::InMemoryDestination destination{{
      .capacity_records = 4,
      .maximum_record_bytes = 512,
  }};
  auto created = ulog::Runtime::Create(
      ulog::RuntimeConfig{
          .threshold = ulog::Level::kInfo,
          .payload_capacity_bytes = 2'048,
          .maximum_record_bytes = 512,
          .producer_slots = 1,
          .ingress_cells = 4,
          .control_operations = 2,
          .worker_threads = 1,
          .startup_timeout = 2s,
          .destruction_timeout = 2s,
      },
      destination);
  if (!created) return 1;
  const ulog::Logger logger = created.runtime->GetLogger();

  int lazy_calls = 0;
  const auto lazy = [&lazy_calls](ulog::LogWriter& writer) {
    ++lazy_calls;
    writer << "lazy";
  };
  LOG_DEBUG_TO(logger) << lazy;
  if (lazy_calls != 0) return 2;

  LOG_INFO_TO(logger) << app::Point{1, 2} << ' ' << std::vector<app::Point>{{3, 4}} << ' ' << lazy;
  {
    ulog::LogWriter writer{logger, ulog::Level::kWarning,
                           ulog::SourceLocation::Custom("writer_main.cpp", "main", 1)};
    writer("points={}", 2) << ": " << app::Point{5, 6};
  }
  if (lazy_calls != 1) return 3;

  auto shutdown = created.runtime->Shutdown();
  if (!shutdown) return 4;
  const auto stopped = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 2s);
  if (stopped.status != ulog::OperationWaitStatus::kCompleted || !stopped.completion ||
      stopped.completion->Outcome() != ulog::OperationOutcome::kSucceeded) {
    return 5;
  }
  const auto first = destination.TryTake();
  if (!first || first->Message() != "(1, 2) [(3, 4)] lazy") return 6;
  const auto second = destination.TryTake();
  if (!second || second->Message() != "points=2: (5, 6)" ||
      second->GetLevel() != ulog::Level::kWarning) {
    return 7;
  }
  return destination.TryTake().has_value() ? 8 : 0;
}
