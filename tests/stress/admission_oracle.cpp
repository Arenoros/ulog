// Model-based randomized test: drives the real Runtime and the reference admission model with the
// same seeded event trace and requires the same observable state after every event.
//
//   ulog-admission-oracle [--seeds=N] [--first-seed=S] [--events=E] [--replay=SEED:I,J,...]
//
// A failure prints the seed and a minimized trace; --replay runs exactly that trace again.

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <functional>
#include <iterator>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/operation.hpp>
#include <ulog/runtime.hpp>
#include <ulog/source_location.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>
#include <utility>
#include <vector>

#include "model/admission_model.hpp"
#include "producer/record_storage.hpp"
#include "producer/thinning.hpp"

namespace {

using namespace std::chrono_literals;
namespace model = ulog::test::admission;

// Private test seam: the model's byte charge uses Record storage's header size.
static_assert(model::kRecordMetadataBytes ==
              ulog::detail::producer::record::kSerializedRecordMetadataBytes);

constexpr std::string_view kSourcePath = "oracle.cpp";
constexpr std::string_view kSourceFunction = "Produce";
constexpr std::uint8_t kThinningPercent = 25;
constexpr ulog::SourceLocation kSource =
    ulog::SourceLocation::Custom(kSourcePath, kSourceFunction, 1);
constexpr std::size_t kMinimumMessageBytes = 16;
constexpr std::int64_t kEventTime = 1'700'000'000'000'000;
/// Deadline of a waiting Logger::LogUntil. The worker is settled before every event, so nothing
/// frees capacity during the wait and the outcome stays deterministic.
constexpr auto kWaitingDeadline = 3ms;

std::int64_t FixedNow(void*) noexcept { return kEventTime; }

// ---- Deterministic randomness ------------------------------------------------------------------

/// SplitMix64: identical sequences on every standard library, unlike <random> distributions.
class Random final {
 public:
  explicit Random(std::uint64_t seed) noexcept : state_(seed) {}

  std::uint64_t Next() noexcept {
    std::uint64_t value = (state_ += 0x9E3779B97F4A7C15ULL);
    value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
    return value ^ (value >> 31U);
  }

  /// A value in [low, high].
  std::size_t Between(std::size_t low, std::size_t high) noexcept {
    return low + static_cast<std::size_t>(Next() % (high - low + 1U));
  }

 private:
  std::uint64_t state_;
};

// ---- Scenario ----------------------------------------------------------------------------------

enum class EventKind : std::uint8_t {
  kLog,
  kBurst,
  kMode,
  kFinish,
  kTake,
  kRelease,
  kDrain,
  kShutdown,
  kReleaseOperation,
  kFinishAll,
  kTakeAll,
  kReleaseAll,
};

/// How a kLog event calls: Log, or LogUntil with an expired or a future deadline.
enum class Blocking : std::uint8_t { kNone, kExpired, kWaiting };

struct Event final {
  EventKind kind{EventKind::kLog};
  std::uint32_t first{0};
  std::uint32_t second{0};
  /// Log level of a kLog event: Info, Error, or Critical.
  ulog::Level level{ulog::Level::kInfo};
  Blocking blocking{Blocking::kNone};
};

struct Scenario final {
  std::uint64_t seed{0};
  model::Config config{};
  std::size_t threads{1};
  std::vector<Event> events;
};

[[nodiscard]] std::string Describe(const Event& event) {
  const std::string first = std::to_string(event.first);
  const std::string second = std::to_string(event.second);
  switch (event.kind) {
    case EventKind::kLog:
      return "Log thread=" + first + " bytes-choice=" + second +
             " level=" + std::string{ulog::ToString(event.level)} +
             (event.blocking == Blocking::kWaiting   ? " until=future"
              : event.blocking == Blocking::kExpired ? " until=expired"
                                                     : "");
    case EventKind::kBurst:
      return "Burst attempts=" + first + " bytes-choice=" + second;
    case EventKind::kMode:
      return "SetDeliveryMode " + first;
    case EventKind::kFinish:
      return "FinishHeld choice=" + first +
             (event.second == 0U   ? " complete"
              : event.second == 1U ? " fail"
                                   : " abandon");
    case EventKind::kTake:
      return std::string{"TryTake "} + (event.first != 0U ? "keep" : "release");
    case EventKind::kRelease:
      return "ReleaseObservation choice=" + first;
    case EventKind::kDrain:
      return "Drain";
    case EventKind::kShutdown:
      return "Shutdown";
    case EventKind::kReleaseOperation:
      return "ReleaseOperation choice=" + first;
    case EventKind::kFinishAll:
      return "CompleteAllHeld";
    case EventKind::kTakeAll:
      return "TakeAndReleaseAllReady";
    case EventKind::kReleaseAll:
      return "ReleaseAllObservationsAndOperations";
  }
  return "Unknown";
}

[[nodiscard]] std::string Describe(const Scenario& scenario) {
  const model::Config& config = scenario.config;
  return "producer_slots=" + std::to_string(config.producer_slots) +
         " threads=" + std::to_string(scenario.threads) +
         " ingress_cells=" + std::to_string(config.ingress_cells) +
         " payload_capacity_bytes=" + std::to_string(config.payload_capacity_bytes) +
         " maximum_record_bytes=" + std::to_string(config.maximum_record_bytes) +
         " priority_reserve_bytes=" + std::to_string(config.priority_reserve_bytes) + " policy=" +
         (config.drop_oldest         ? "drop-oldest"
          : config.adaptive_thinning ? "adaptive-thinning"
                                     : "drop-newest") +
         " destination_slots=" + std::to_string(config.destination_slots) +
         " control_operations=" + std::to_string(config.control_operations);
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a seed and a count, named at every call.
[[nodiscard]] Scenario Generate(std::uint64_t seed, std::size_t event_count) {
  Random random{seed};
  Scenario scenario;
  scenario.seed = seed;
  model::Config& config = scenario.config;
  config.producer_slots = random.Between(1, 3);
  config.ingress_cells = random.Between(config.producer_slots, config.producer_slots + 4U);
  config.maximum_record_bytes = random.Between(0, 1) == 0U ? 256U : 512U;
  config.payload_capacity_bytes = config.maximum_record_bytes * random.Between(1, 4) +
                                  model::kQuantumBytes * random.Between(0, 3);
  config.destination_slots = random.Between(1, 4);
  config.control_operations = random.Between(1, 3);
  // A third of the scenarios run without the priority reserve. The choice and the log levels
  // derive from values already drawn, so the random stream and the other events stay unchanged.
  config.priority_reserve_bytes = config.maximum_record_bytes * ((seed + 1U) % 3U);
  // Seeds rotate through the three policies in pairs.
  config.drop_oldest = seed / 2U % 3U == 1U;
  config.adaptive_thinning = seed / 2U % 3U == 2U;
  config.thinning_low_watermark_bytes = config.payload_capacity_bytes * kThinningPercent / 100U;
  config.site_hash = ulog::detail::producer::CallSiteHash(kSourcePath, 1);
  config.source_bytes = kSourcePath.size() + kSourceFunction.size();
  // One extra thread sometimes registers after every slot is taken.
  scenario.threads = config.producer_slots + random.Between(0, 1);

  for (std::size_t index = 0; index < event_count; ++index) {
    Event event;
    const std::size_t roll = random.Between(0, 99);
    const auto first = static_cast<std::uint32_t>(random.Next() % 1'000U);
    const auto second = static_cast<std::uint32_t>(random.Next() % 1'000U);
    if (roll < 34) {
      // A quarter of the attempts are Error or Critical and may use the priority reserve.
      constexpr std::array<ulog::Level, 4> kLevels{ulog::Level::kInfo, ulog::Level::kInfo,
                                                   ulog::Level::kError, ulog::Level::kCritical};
      // A quarter of the attempts are LogUntil calls, half of them with an expired deadline.
      // Both choices derive from `first`, so the random stream stays unchanged.
      const Blocking blocking = first / 64U % 4U != 0U    ? Blocking::kNone
                                : first / 256U % 2U == 0U ? Blocking::kWaiting
                                                          : Blocking::kExpired;
      event = {EventKind::kLog, first, second, kLevels[first / 16U % kLevels.size()], blocking};
    } else if (roll < 42) {
      event = {EventKind::kBurst, 1U + first % 4U, second};
    } else if (roll < 50) {
      // Holding is chosen twice as often: held deliveries drive back-pressure and reordering.
      constexpr std::array<std::uint32_t, 4> kModes{0, 1, 1, 2};
      event = {EventKind::kMode, kModes[first % kModes.size()], 0};
    } else if (roll < 64) {
      event = {EventKind::kFinish, first, second % 3U};
    } else if (roll < 75) {
      event = {EventKind::kTake, first % 2U, 0};
    } else if (roll < 79) {
      event = {EventKind::kRelease, first, 0};
    } else if (roll < 88) {
      event = {EventKind::kDrain, 0, 0};
    } else if (roll < 91) {
      event = {EventKind::kShutdown, 0, 0};
    } else {
      event = {EventKind::kReleaseOperation, first, 0};
    }
    scenario.events.push_back(event);
  }
  // Settle every delivery, then stop the Runtime explicitly.
  scenario.events.push_back({EventKind::kMode, 0, 0});
  scenario.events.push_back({EventKind::kFinishAll, 0, 0});
  scenario.events.push_back({EventKind::kTakeAll, 0, 0});
  scenario.events.push_back({EventKind::kReleaseAll, 0, 0});
  scenario.events.push_back({EventKind::kShutdown, 0, 0});
  scenario.events.push_back({EventKind::kTakeAll, 0, 0});
  return scenario;
}

// ---- Producer threads --------------------------------------------------------------------------

/// One application thread that registers with the Runtime and runs commands synchronously.
class ProducerThread final {
 public:
  explicit ProducerThread(ulog::Runtime& runtime) : runtime_(&runtime) {
    thread_ = std::thread{[this] { Run(); }};
  }
  ProducerThread(const ProducerThread&) = delete;
  ProducerThread& operator=(const ProducerThread&) = delete;
  ~ProducerThread() {
    Post({});
    thread_.join();
  }

  /// Hands a command to the thread without waiting for it.
  void Post(std::function<void(ulog::Logger)> command) {
    {
      const std::lock_guard lock{mutex_};
      command_ = std::move(command);
      stop_ = !command_;
      pending_ = true;
    }
    condition_.notify_all();
  }

  void Wait() {
    std::unique_lock lock{mutex_};
    condition_.wait(lock, [this] { return !pending_; });
  }

  void Execute(std::function<void(ulog::Logger)> command) {
    Post(std::move(command));
    Wait();
  }

  /// Waits until the thread registered; registration runs before the first command.
  void AwaitRegistration() {
    Execute([](ulog::Logger) {});
  }

 private:
  void Run() {
    const ulog::Logger logger = runtime_->GetLogger();
    while (true) {
      std::function<void(ulog::Logger)> command;
      {
        std::unique_lock lock{mutex_};
        condition_.wait(lock, [this] { return pending_; });
        if (stop_) {
          pending_ = false;
          condition_.notify_all();
          return;
        }
        command = std::move(command_);
      }
      command(logger);
      {
        const std::lock_guard lock{mutex_};
        pending_ = false;
      }
      condition_.notify_all();
    }
  }

  ulog::Runtime* runtime_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::function<void(ulog::Logger)> command_;
  bool pending_{false};
  bool stop_{false};
  std::thread thread_;
};

template <ulog::Level kLevel, typename MessageFactory>
void LogAt(ulog::Logger logger, Blocking blocking, MessageFactory& make_message) {
  switch (blocking) {
    case Blocking::kNone:
      logger.Log<kLevel>(kSource, make_message);
      return;
    case Blocking::kExpired:
      logger.LogUntil<kLevel>(std::chrono::steady_clock::now() - 1ms, kSource, make_message);
      return;
    case Blocking::kWaiting:
      logger.LogUntil<kLevel>(std::chrono::steady_clock::now() + kWaitingDeadline, kSource,
                              make_message);
      return;
  }
}

/// Logs one message and returns how many times its factory ran.
std::size_t LogOnce(ulog::Logger logger, std::string_view message,
                    ulog::Level level = ulog::Level::kInfo, Blocking blocking = Blocking::kNone) {
  std::size_t evaluations = 0;
  const auto make_message = [&] {
    ++evaluations;
    return message;
  };
  switch (level) {
    case ulog::Level::kError:
      LogAt<ulog::Level::kError>(logger, blocking, make_message);
      break;
    case ulog::Level::kCritical:
      LogAt<ulog::Level::kCritical>(logger, blocking, make_message);
      break;
    default:
      LogAt<ulog::Level::kInfo>(logger, blocking, make_message);
      break;
  }
  return evaluations;
}

// ---- Runner ------------------------------------------------------------------------------------

struct Failure final {
  std::size_t event{0};
  std::string message;
};

/// Which modeled behaviors a seed set reached; the default set must reach all of them.
enum class Covered : std::uint8_t {
  kAccepted,
  kFiltered,
  kNoProducer,
  kLaneFull,
  kBudget,
  kPriorityReserveAccepted,
  kPriorityReserveExhausted,
  kEvicted,
  kThinned,
  kBlockingAdmitted,
  kBlockingTimedOut,
  kConcurrentBurst,
  kImmediateDrain,
  kPendingDrain,
  kPendingShutdown,
  kReserveExhausted,
  kHeldCompleted,
  kHeldFailed,
  kHeldAbandoned,
  kOutOfOrderCompletion,
  kKeptObservation,
  kCount,
};

constexpr std::array<std::string_view, static_cast<std::size_t>(Covered::kCount)> kCoveredNames{
    "accepted",
    "filtered",
    "rejected-no-producer",
    "rejected-lane-full",
    "rejected-budget",
    "priority-reserve-accepted",
    "rejected-priority-reserve",
    "evicted-oldest",
    "rejected-thinned",
    "blocking-admitted",
    "blocking-timed-out",
    "concurrent-burst",
    "immediate-drain",
    "pending-drain",
    "pending-shutdown",
    "reserve-exhausted",
    "held-completed",
    "held-failed",
    "held-abandoned",
    "out-of-order-completion",
    "kept-observation",
};

using Coverage = std::array<std::uint64_t, static_cast<std::size_t>(Covered::kCount)>;

struct RunOptions final {
  std::chrono::milliseconds converge_deadline{5'000};
  Coverage* coverage{nullptr};
};

[[nodiscard]] std::string Mismatch(std::string_view what, std::uint64_t expected,
                                   std::uint64_t observed) {
  return std::string{what} + " expected " + std::to_string(expected) + ", observed " +
         std::to_string(observed);
}

[[nodiscard]] std::string ReportText(const ulog::OperationReport& report) {
  return "{watermark=" + std::to_string(report.watermark_records) +
         " processed=" + std::to_string(report.processed_records) + "/" +
         std::to_string(report.processed_bytes) +
         " delivered=" + std::to_string(report.delivered_records) + "/" +
         std::to_string(report.delivered_bytes) +
         " failed=" + std::to_string(report.failed_records) + "/" +
         std::to_string(report.failed_bytes) +
         " unfinished=" + std::to_string(report.unfinished_records) + "/" +
         std::to_string(report.unfinished_bytes) + "}";
}

class ScenarioRun final {
 public:
  ScenarioRun(const Scenario& scenario, RunOptions options)
      : scenario_(scenario),
        options_(options),
        model_(scenario.config),
        destination_({.capacity_records = scenario.config.destination_slots,
                      .maximum_record_bytes = scenario.config.maximum_record_bytes}) {}

  /// Runs the selected events in order; returns the first divergence.
  std::optional<Failure> Run(const std::vector<std::size_t>& selected) {
    const model::Config& config = scenario_.config;
    auto created = ulog::Runtime::Create(
        ulog::RuntimeConfig{
            .threshold = ulog::Level::kInfo,
            .payload_capacity_bytes = config.payload_capacity_bytes,
            .maximum_record_bytes = config.maximum_record_bytes,
            .producer_slots = config.producer_slots,
            .ingress_cells = config.ingress_cells,
            .control_operations = config.control_operations,
            .worker_threads = 1,
            .startup_timeout = 5s,
            .destruction_timeout = 5s,
            .event_clock = ulog::EventClock{nullptr, &FixedNow},
            .admission_policy = config.drop_oldest ? ulog::AdmissionPolicy::kDropOldest
                                : config.adaptive_thinning
                                    ? ulog::AdmissionPolicy::kAdaptiveThinning
                                    : ulog::AdmissionPolicy::kDropNewest,
            .priority_reserve_bytes = config.priority_reserve_bytes,
            .thinning_low_watermark_percent = kThinningPercent,
        },
        destination_);
    if (!created) {
      return Failure{0, "Runtime::Create failed: " +
                            std::string{created.failure ? created.failure->Message() : ""}};
    }
    runtime_ = std::move(created.runtime);
    // Threads register in index order, so thread i owns producer slot i.
    for (std::size_t thread = 0; thread < scenario_.threads; ++thread) {
      threads_.push_back(std::make_unique<ProducerThread>(*runtime_));
      threads_.back()->AwaitRegistration();
      model_.Register(thread);
    }
    if (auto failure = Converge(); failure) {
      return Failure{0, "initial state: " + *failure};
    }
    for (const std::size_t index : selected) {
      std::optional<std::string> failure = Apply(scenario_.events.at(index));
      if (failure) {
        // Show the state divergence behind an early failure too.
        if (const auto state = Converge(); state && *state != *failure) {
          *failure += "; " + *state;
        }
      } else {
        failure = Converge();
      }
      if (failure) {
        return Failure{index, Describe(scenario_.events.at(index)) + ": " + *failure};
      }
    }
    return std::nullopt;
  }

  ~ScenarioRun() {
    observations_.clear();
    handles_.clear();
    operations_.clear();
    threads_.clear();
    runtime_.reset();
  }

  ScenarioRun(const ScenarioRun&) = delete;
  ScenarioRun& operator=(const ScenarioRun&) = delete;

 private:
  void Cover(Covered what, std::uint64_t count = 1) const noexcept {
    if (options_.coverage != nullptr && what != Covered::kCount) {
      (*options_.coverage)[static_cast<std::size_t>(what)] += count;
    }
  }

  struct RealOperation final {
    ulog::Operation operation;
    bool released{false};
  };

  [[nodiscard]] std::size_t MessageBytes(std::uint32_t choice) const noexcept {
    const std::size_t maximum = model_.MaximumMessageBytes();
    return kMinimumMessageBytes + choice % (maximum - kMinimumMessageBytes + 1U);
  }

  [[nodiscard]] static std::string Pad(std::string text, std::size_t bytes) {
    text.resize(bytes, 'x');
    return text;
  }

  std::optional<std::string> Apply(const Event& event) {
    switch (event.kind) {
      case EventKind::kLog:
        return ApplyLog(event.first % scenario_.threads, MessageBytes(event.second), event.level,
                        event.blocking);
      case EventKind::kBurst:
        return ApplyBurst(event.first, MessageBytes(event.second));
      case EventKind::kMode:
        return ApplyMode(static_cast<model::DeliveryMode>(event.first % 3U));
      case EventKind::kFinish:
        return ApplyFinish(event.first, static_cast<FinishHow>(event.second % 3U));
      case EventKind::kTake:
        return ApplyTake(event.first != 0U);
      case EventKind::kRelease:
        return ApplyRelease(event.first);
      case EventKind::kDrain:
        return ApplyStart(model::OperationKind::kDrain);
      case EventKind::kShutdown:
        return ApplyStart(model::OperationKind::kShutdown);
      case EventKind::kReleaseOperation:
        return ApplyReleaseOperation(event.first);
      case EventKind::kFinishAll:
        while (!model_.Held().empty()) {
          if (auto failure = ApplyFinish(0, FinishHow::kComplete)) {
            return failure;
          }
          if (auto failure = Converge()) {
            return failure;
          }
        }
        return std::nullopt;
      case EventKind::kTakeAll:
        while (model_.NextReady()) {
          if (auto failure = ApplyTake(false)) {
            return failure;
          }
          if (auto failure = Converge()) {
            return failure;
          }
        }
        return ApplyTake(false);  // nothing is ready now, in the model and the Runtime
      case EventKind::kReleaseAll:
        observations_.clear();
        for (const std::uint64_t sequence : model_.Observed()) {
          model_.Release(sequence);
        }
        for (std::size_t index = 0; index < operations_.size(); ++index) {
          ReleaseOperationAt(index);
        }
        return std::nullopt;
    }
    return std::string{"unknown event"};
  }

  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters): a thread index and a message size.
  std::optional<std::string> ApplyLog(std::size_t thread, std::size_t bytes,
                                      ulog::Level level = ulog::Level::kInfo,
                                      Blocking blocking = Blocking::kNone) {
    if (model_.IsBusy(thread)) {
      return std::nullopt;  // its lane and credit depend on an unresolved burst order
    }
    std::string message = Pad(fmt::format("m{}:", next_message_++), bytes);
    const std::uint64_t evicted_before = model_.counters().shed_records;
    const bool waiting = blocking == Blocking::kWaiting;
    const model::Admission admission = model_.Log(thread, message, level, waiting);
    if (waiting && admission != model::Admission::kFiltered &&
        admission != model::Admission::kNoProducer) {
      Cover(model_.last_wait_timed_out() ? Covered::kBlockingTimedOut : Covered::kBlockingAdmitted);
    }
    if (model_.counters().shed_records != evicted_before) {
      Cover(Covered::kEvicted);
    }
    Cover(admission == model::Admission::kAccepted && model_.last_used_reserve()
              ? Covered::kPriorityReserveAccepted
          : admission == model::Admission::kAccepted        ? Covered::kAccepted
          : admission == model::Admission::kFiltered        ? Covered::kFiltered
          : admission == model::Admission::kNoProducer      ? Covered::kNoProducer
          : admission == model::Admission::kLaneFull        ? Covered::kLaneFull
          : admission == model::Admission::kPriorityReserve ? Covered::kPriorityReserveExhausted
          : admission == model::Admission::kThinned         ? Covered::kThinned
                                                            : Covered::kBudget);
    std::size_t evaluations = 0;
    threads_[thread]->Execute(
        [&](ulog::Logger logger) { evaluations = LogOnce(logger, message, level, blocking); });
    const std::size_t expected = admission == model::Admission::kAccepted ? 1U : 0U;
    if (evaluations != expected) {
      return Mismatch("message evaluations", expected, evaluations);
    }
    return std::nullopt;
  }

  std::optional<std::string> ApplyBurst(std::size_t attempts, std::size_t bytes) {
    std::vector<std::size_t> participants;
    for (std::size_t thread = 0; thread < scenario_.threads; ++thread) {
      if (!model_.IsBusy(thread)) {
        participants.push_back(thread);
      }
    }
    if (!model_.IsBurstDeterministic(participants, attempts, bytes)) {
      // Interleavings could change the outcome, so run the same attempts one thread at a time.
      for (const std::size_t thread : participants) {
        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
          if (auto failure = ApplyLog(thread, bytes)) {
            return failure;
          }
          if (auto failure = Converge()) {
            return failure;
          }
        }
      }
      return std::nullopt;
    }

    const std::size_t burst = ++last_burst_;
    const std::vector<std::size_t> expected = model_.Burst(participants, attempts, bytes);
    Cover(Covered::kConcurrentBurst);
    std::vector<std::size_t> evaluations(participants.size(), 0);
    std::latch start{static_cast<std::ptrdiff_t>(participants.size())};
    for (std::size_t index = 0; index < participants.size(); ++index) {
      const std::size_t thread = participants[index];
      threads_[thread]->Post([&, index, thread](ulog::Logger logger) {
        std::vector<std::string> messages;
        for (std::size_t attempt = 0; attempt < attempts; ++attempt) {
          messages.push_back(Pad(fmt::format("b{}p{}n{}:", burst, thread, attempt), bytes));
        }
        start.arrive_and_wait();
        for (const std::string& message : messages) {
          evaluations[index] += LogOnce(logger, message);
        }
      });
    }
    for (const std::size_t thread : participants) {
      threads_[thread]->Wait();
    }
    for (std::size_t index = 0; index < participants.size(); ++index) {
      burst_accepted_[{burst, participants[index]}] = expected[index];
      if (evaluations[index] != expected[index]) {
        return Mismatch("burst evaluations of thread " + std::to_string(participants[index]),
                        expected[index], evaluations[index]);
      }
    }
    return std::nullopt;
  }

  std::optional<std::string> ApplyMode(model::DeliveryMode mode) {
    model_.SetMode(mode);
    destination_.SetDeliveryMode(mode == model::DeliveryMode::kCompleteInline
                                     ? ulog::testing::EncodedDeliveryMode::kCompleteInline
                                 : mode == model::DeliveryMode::kHold
                                     ? ulog::testing::EncodedDeliveryMode::kHold
                                     : ulog::testing::EncodedDeliveryMode::kFailInline);
    return std::nullopt;
  }

  /// Checks a frame against the model's Record: exact bytes, or the burst pattern and per-producer
  /// FIFO order when the producer order inside a burst is unknown.
  std::optional<std::string> CheckFrame(std::uint64_t sequence, std::uint64_t observed_sequence,
                                        std::string_view bytes) {
    if (observed_sequence != sequence) {
      return Mismatch("admission sequence", sequence, observed_sequence);
    }
    const model::Record& record = model_.records().at(sequence);
    if (bytes.size() != record.frame_bytes() || !bytes.starts_with("tskv\ttext=") ||
        !bytes.ends_with('\n')) {
      return "frame of sequence " + std::to_string(sequence) + " has unexpected bytes '" +
             std::string{bytes} + "'";
    }
    const std::string_view message = bytes.substr(10, bytes.size() - 11U);
    if (record.burst == 0U) {
      if (message != record.message) {
        return "sequence " + std::to_string(sequence) + " carries '" + std::string{message} +
               "' instead of '" + record.message + "'";
      }
      return std::nullopt;
    }
    std::size_t burst = 0;
    std::size_t thread = 0;
    std::size_t attempt = 0;
    if (!ParseBurstMessage(message, burst, thread, attempt) || burst != record.burst ||
        attempt >= burst_accepted_[{burst, thread}]) {
      return "sequence " + std::to_string(sequence) + " carries unexpected burst message '" +
             std::string{message} + "'";
    }
    auto& order = burst_order_[{burst, thread}];
    const auto [position, inserted] = order.emplace(sequence, attempt);
    if (!inserted) {
      return std::nullopt;  // the same frame observed again through another channel
    }
    const bool after_previous = position == order.begin() || std::prev(position)->second < attempt;
    const bool before_next =
        std::next(position) == order.end() || attempt < std::next(position)->second;
    if (!after_previous || !before_next || !burst_seen_.insert({burst, thread, attempt}).second) {
      return "burst " + std::to_string(burst) + " thread " + std::to_string(thread) +
             " lost FIFO order at sequence " + std::to_string(sequence);
    }
    return std::nullopt;
  }

  static bool ParseBurstMessage(std::string_view message, std::size_t& burst, std::size_t& thread,
                                std::size_t& attempt) {
    const auto parse = [&message](char tag, std::size_t& value) {
      if (message.empty() || message.front() != tag) {
        return false;
      }
      message.remove_prefix(1);
      const auto [end, error] =
          std::from_chars(message.data(), message.data() + message.size(), value);
      if (error != std::errc{}) {
        return false;
      }
      message.remove_prefix(static_cast<std::size_t>(end - message.data()));
      return true;
    };
    return parse('b', burst) && parse('p', thread) && parse('n', attempt) &&
           message.starts_with(':');
  }

  std::optional<std::string> TakeHandlesThrough(std::uint64_t sequence) {
    while (!handles_.contains(sequence)) {
      const std::optional<std::uint64_t> expected = model_.NextUntakenHeld();
      std::optional<ulog::testing::PendingEncodedDelivery> handle =
          destination_.TryTakePendingDelivery();
      if (!expected || !handle) {
        return std::string{"pending delivery "} + (expected ? "missing" : "unexpected");
      }
      if (auto failure = CheckFrame(*expected, handle->AdmissionSequence(), handle->Bytes())) {
        return failure;
      }
      model_.TakeHandle(*expected);
      handles_.emplace(*expected, std::move(*handle));
    }
    return std::nullopt;
  }

  enum class FinishHow : std::uint8_t { kComplete, kFail, kAbandon };

  std::optional<std::string> ApplyFinish(std::uint32_t choice, FinishHow how) {
    const std::vector<std::uint64_t> held = model_.Held();
    if (held.empty()) {
      if (destination_.TryTakePendingDelivery()) {
        return std::string{"unexpected pending delivery"};
      }
      return std::nullopt;
    }
    const std::uint64_t sequence = held[choice % held.size()];
    if (auto failure = TakeHandlesThrough(sequence)) {
      return failure;
    }
    ulog::testing::PendingEncodedDelivery handle = std::move(handles_.extract(sequence).mapped());
    if (how == FinishHow::kAbandon) {
      // Destroying an uncompleted handle fails its delivery.
      const ulog::testing::PendingEncodedDelivery abandoned{std::move(handle)};
    } else {
      const auto status = how == FinishHow::kComplete ? handle.Complete() : handle.Fail();
      if (status != ulog::testing::EncodedDeliveryCompletionStatus::kCompleted) {
        return std::string{"delivery completion was not accepted"};
      }
    }
    Cover(how == FinishHow::kComplete ? Covered::kHeldCompleted
          : how == FinishHow::kFail   ? Covered::kHeldFailed
                                      : Covered::kHeldAbandoned);
    if (sequence != held.front()) {
      Cover(Covered::kOutOfOrderCompletion);
    }
    model_.Finish(sequence, how == FinishHow::kComplete);
    return std::nullopt;
  }

  std::optional<std::string> ApplyTake(bool keep) {
    const std::optional<std::uint64_t> expected = model_.NextReady();
    std::optional<ulog::testing::ObservedEncodedRecord> observed = destination_.TryTake();
    if (!expected || !observed) {
      if (expected || observed) {
        return std::string{"ready frame "} + (expected ? "missing" : "unexpected");
      }
      return std::nullopt;
    }
    if (auto failure = CheckFrame(*expected, observed->AdmissionSequence(), observed->Bytes())) {
      return failure;
    }
    model_.Take(*expected);
    if (keep) {
      Cover(Covered::kKeptObservation);
      observations_.emplace(*expected, std::move(*observed));
    } else {
      observed.reset();
      model_.Release(*expected);
    }
    return std::nullopt;
  }

  std::optional<std::string> ApplyRelease(std::uint32_t choice) {
    const std::vector<std::uint64_t> observed = model_.Observed();
    if (observed.empty()) {
      return std::nullopt;
    }
    const std::uint64_t sequence = observed[choice % observed.size()];
    observations_.erase(sequence);
    model_.Release(sequence);
    return std::nullopt;
  }

  std::optional<std::string> ApplyStart(model::OperationKind kind) {
    const std::optional<std::size_t> started = model_.Start(kind);
    const auto start = [&] {
      return kind == model::OperationKind::kDrain ? runtime_->Drain() : runtime_->Shutdown();
    };
    ulog::OperationStartResult result = start();
    // The worker publishes a result before it drops its own reference to the control slot, so a
    // slot the model already counts as free can stay occupied briefly. A start rejected for an
    // exhausted reserve changes nothing, so retry it until the slot is released.
    const auto deadline = std::chrono::steady_clock::now() + options_.converge_deadline;
    while (started && !result && result.failure &&
           result.failure->code == ulog::OperationStartErrorCode::kControlReserveExhausted &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(100us);
      result = start();
    }
    if (!started) {
      Cover(Covered::kReserveExhausted);
      if (result || !result.failure ||
          result.failure->code != ulog::OperationStartErrorCode::kControlReserveExhausted ||
          result.failure->control_capacity != scenario_.config.control_operations ||
          result.failure->controls_in_use != scenario_.config.control_operations) {
        return std::string{"expected control reserve exhaustion"};
      }
      return std::nullopt;
    }
    if (!result) {
      operations_.push_back({ulog::Operation{}, true});  // keeps indices aligned with the model
      return std::string{"Operation did not start: "} +
             std::string{result.failure ? result.failure->Message() : ""};
    }
    const bool pending = !model_.operations().at(*started).completed;
    Cover(kind == model::OperationKind::kShutdown
              ? (pending ? Covered::kPendingShutdown : Covered::kCount)
          : pending ? Covered::kPendingDrain
                    : Covered::kImmediateDrain);
    operations_.push_back({std::move(result.operation), false});
    return std::nullopt;
  }

  void ReleaseOperationAt(std::size_t index) {
    if (!operations_[index].released) {
      operations_[index].operation = ulog::Operation{};
      operations_[index].released = true;
      model_.ReleaseOperation(index);
    }
  }

  std::optional<std::string> ApplyReleaseOperation(std::uint32_t choice) {
    std::vector<std::size_t> held;
    for (std::size_t index = 0; index < operations_.size(); ++index) {
      if (!operations_[index].released) {
        held.push_back(index);
      }
    }
    if (!held.empty()) {
      ReleaseOperationAt(held[choice % held.size()]);
    }
    return std::nullopt;
  }

  /// Compares the Runtime with the model, retrying until it matches or the deadline passes.
  std::optional<std::string> Converge() {
    const auto deadline = std::chrono::steady_clock::now() + options_.converge_deadline;
    std::optional<std::string> difference;
    do {
      difference = Compare();
      if (!difference) {
        return std::nullopt;
      }
      std::this_thread::sleep_for(100us);
    } while (std::chrono::steady_clock::now() < deadline);
    return difference;
  }

  [[nodiscard]] std::optional<std::string> Compare() const {
    const model::Counters expected = model_.counters();
    const ulog::RuntimeSnapshot observed = runtime_->GetSnapshot();
    const std::array<std::tuple<std::string_view, std::uint64_t, std::uint64_t>, 23> fields{{
        {"accepted_records", expected.accepted, observed.accepted_records},
        {"processed_records", expected.processed, observed.processed_records},
        {"processed_bytes", expected.processed_bytes, observed.processed_bytes},
        {"completed_records", expected.delivered + expected.failed, observed.completed_records},
        {"delivered_records", expected.delivered, observed.delivered_records},
        {"delivered_bytes", expected.delivered_bytes, observed.delivered_bytes},
        {"delivery_failed_records", expected.failed, observed.delivery_failed_records},
        {"delivery_failed_bytes", expected.failed_bytes, observed.delivery_failed_bytes},
        {"encoding_failed_records", 0, observed.encoding_failed_records},
        {"rejected_no_producer", expected.rejected_no_producer, observed.rejected_no_producer},
        {"rejected_lane_full", expected.rejected_lane_full, observed.rejected_lane_full},
        {"rejected_budget", expected.rejected_budget, observed.rejected_budget},
        {"rejected_priority_reserve", expected.rejected_priority_reserve,
         observed.rejected_priority_reserve},
        {"rejected_thinned", expected.rejected_thinned, observed.rejected_thinned},
        // Producer threads are application threads, so no LogUntil call is ever refused.
        {"rejected_blocking_refused", 0, observed.rejected_blocking_refused},
        {"dropped_newest_records",
         expected.rejected_no_producer + expected.rejected_lane_full + expected.rejected_budget +
             expected.rejected_priority_reserve + expected.rejected_thinned,
         observed.dropped_newest_records},
        {"shed_records", expected.shed_records, observed.shed_records},
        {"retained_records", expected.retained_records, observed.retained_records},
        {"logical_retained_bytes", expected.logical_retained_bytes,
         observed.logical_retained_bytes},
        {"payload_capacity_bytes", scenario_.config.payload_capacity_bytes,
         observed.payload_capacity_bytes},
        {"priority_reserve_bytes", scenario_.config.priority_reserve_bytes,
         observed.priority_reserve_bytes},
        {"admission_open", expected.admission_open ? 1U : 0U, observed.admission_open ? 1U : 0U},
        {"worker_running", model_.worker_stopped() ? 0U : 1U, observed.worker_running ? 1U : 0U},
    }};
    for (const auto& [name, want, got] : fields) {
      if (want != got) {
        return Mismatch(std::string{"snapshot."} + std::string{name}, want, got);
      }
    }
    if (expected.physical_retained_bytes &&
        *expected.physical_retained_bytes != observed.physical_retained_bytes) {
      return Mismatch("snapshot.physical_retained_bytes", *expected.physical_retained_bytes,
                      observed.physical_retained_bytes);
    }
    if (observed.route_failed) {
      return std::string{"snapshot.route_failed is set"};
    }
    const auto& operations = model_.operations();
    if (operations.size() != operations_.size()) {
      return Mismatch("started Operations", operations.size(), operations_.size());
    }
    for (std::size_t index = 0; index < operations.size(); ++index) {
      if (operations_[index].released) {
        continue;
      }
      const ulog::OperationPollResult poll = operations_[index].operation.Poll();
      const model::ModelOperation& want = operations[index];
      const bool completed = poll.status == ulog::OperationPollStatus::kCompleted;
      if (completed != want.completed) {
        return "Operation " + std::to_string(index) +
               (want.completed ? " is pending" : " completed");
      }
      if (completed && (!poll.completion || poll.completion->Outcome() != want.outcome ||
                        poll.completion->Report() != want.report)) {
        return "Operation " + std::to_string(index) + " report " +
               (poll.completion ? ReportText(poll.completion->Report()) : std::string{"missing"}) +
               ", expected " + ReportText(want.report);
      }
    }
    return std::nullopt;
  }

  const Scenario& scenario_;
  RunOptions options_;
  model::AdmissionModel model_;
  ulog::testing::InMemoryEncodedDestination destination_;
  std::unique_ptr<ulog::Runtime> runtime_;
  std::vector<std::unique_ptr<ProducerThread>> threads_;
  std::map<std::uint64_t, ulog::testing::PendingEncodedDelivery> handles_;
  std::map<std::uint64_t, ulog::testing::ObservedEncodedRecord> observations_;
  std::vector<RealOperation> operations_;
  std::map<std::pair<std::size_t, std::size_t>, std::size_t> burst_accepted_;
  std::map<std::pair<std::size_t, std::size_t>, std::map<std::uint64_t, std::size_t>> burst_order_;
  std::set<std::tuple<std::size_t, std::size_t, std::size_t>> burst_seen_;
  std::size_t next_message_{0};
  std::size_t last_burst_{0};
};

[[nodiscard]] std::optional<Failure> RunScenario(const Scenario& scenario,
                                                 const std::vector<std::size_t>& selected,
                                                 RunOptions options) {
  ScenarioRun run{scenario, options};
  return run.Run(selected);
}

// ---- Trace minimization ------------------------------------------------------------------------

/// Greedily removes chunks of events while the trace still fails, halving the chunk size down to
/// single events. Stops early at `deadline` and returns the smallest failing trace found.
[[nodiscard]] std::vector<std::size_t> Minimize(
    std::vector<std::size_t> trace,
    const std::function<bool(const std::vector<std::size_t>&)>& fails,
    std::chrono::steady_clock::time_point deadline) {
  for (std::size_t chunk = std::max<std::size_t>(trace.size() / 2U, 1U); chunk >= 1U; chunk /= 2U) {
    bool removed = true;
    while (removed) {
      removed = false;
      for (std::size_t start = 0; start < trace.size(); start += chunk) {
        if (std::chrono::steady_clock::now() >= deadline) {
          return trace;
        }
        std::vector<std::size_t> candidate;
        candidate.insert(candidate.end(), trace.begin(),
                         trace.begin() + static_cast<std::ptrdiff_t>(start));
        candidate.insert(
            candidate.end(),
            trace.begin() + static_cast<std::ptrdiff_t>(std::min(start + chunk, trace.size())),
            trace.end());
        if (fails(candidate)) {
          trace = std::move(candidate);
          removed = true;
          break;
        }
      }
    }
    if (chunk == 1U) {
      break;
    }
  }
  return trace;
}

[[nodiscard]] bool MinimizerSelfCheck() {
  std::vector<std::size_t> trace(40);
  for (std::size_t index = 0; index < trace.size(); ++index) {
    trace[index] = index;
  }
  const auto fails = [](const std::vector<std::size_t>& candidate) {
    return std::ranges::find(candidate, 7U) != candidate.end() &&
           std::ranges::find(candidate, 31U) != candidate.end();
  };
  const auto minimized = Minimize(trace, fails, std::chrono::steady_clock::now() + 10s);
  return minimized == std::vector<std::size_t>{7, 31};
}

// ---- Driver ------------------------------------------------------------------------------------

struct Arguments final {
  std::uint64_t first_seed{1};
  std::uint64_t seeds{12};
  std::size_t events{40};
  std::optional<std::pair<std::uint64_t, std::vector<std::size_t>>> replay{};
  bool require_coverage{false};
};

[[nodiscard]] bool ParseNumber(std::string_view text, std::uint64_t& value) {
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  return error == std::errc{} && end == text.data() + text.size();
}

[[nodiscard]] std::optional<Arguments> Parse(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument{argv[index]};
    std::uint64_t value = 0;
    if (argument.starts_with("--seeds=") && ParseNumber(argument.substr(8), value)) {
      arguments.seeds = value;
    } else if (argument.starts_with("--first-seed=") && ParseNumber(argument.substr(13), value)) {
      arguments.first_seed = value;
    } else if (argument.starts_with("--events=") && ParseNumber(argument.substr(9), value)) {
      arguments.events = static_cast<std::size_t>(value);
    } else if (argument == "--require-coverage") {
      arguments.require_coverage = true;
    } else if (argument.starts_with("--replay=")) {
      std::string_view text = argument.substr(9);
      const std::size_t colon = text.find(':');
      std::uint64_t seed = 0;
      if (colon == std::string_view::npos || !ParseNumber(text.substr(0, colon), seed)) {
        return std::nullopt;
      }
      text.remove_prefix(colon + 1U);
      std::vector<std::size_t> events;
      while (!text.empty()) {
        const std::size_t comma = text.find(',');
        if (!ParseNumber(text.substr(0, comma), value)) {
          return std::nullopt;
        }
        events.push_back(static_cast<std::size_t>(value));
        text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 1U);
      }
      arguments.replay = std::pair{seed, std::move(events)};
    } else {
      return std::nullopt;
    }
  }
  return arguments;
}

void PrintFailure(const Scenario& scenario, const Failure& failure,
                  const std::vector<std::size_t>& trace) {
  std::fprintf(stderr, "admission oracle: seed %llu diverged at event %zu: %s\n",
               static_cast<unsigned long long>(scenario.seed), failure.event,
               failure.message.c_str());
  std::fprintf(stderr, "  config: %s\n", Describe(scenario).c_str());
  std::fprintf(stderr, "  minimized trace (%zu of %zu events):\n", trace.size(),
               scenario.events.size());
  std::string replay;
  for (const std::size_t index : trace) {
    std::fprintf(stderr, "    [%zu] %s\n", index, Describe(scenario.events[index]).c_str());
    replay += fmt::format("{}{}", replay.empty() ? "" : ",", index);
  }
  std::fprintf(stderr, "  replay: ulog-admission-oracle --events=%zu --replay=%llu:%s\n",
               scenario.events.size() - 6U, static_cast<unsigned long long>(scenario.seed),
               replay.c_str());
}

[[nodiscard]] bool RunSeed(const Scenario& scenario, const std::vector<std::size_t>& trace,
                           Coverage* coverage) {
  const std::optional<Failure> failure =
      RunScenario(scenario, trace, {.converge_deadline = 5'000ms, .coverage = coverage});
  if (!failure) {
    return true;
  }
  // Keep only the events up to the divergence, then shrink with a short convergence deadline.
  std::vector<std::size_t> prefix;
  for (const std::size_t index : trace) {
    prefix.push_back(index);
    if (index == failure->event) {
      break;
    }
  }
  const RunOptions quick{.converge_deadline = 250ms};
  const auto fails = [&](const std::vector<std::size_t>& candidate) {
    return RunScenario(scenario, candidate, quick).has_value();
  };
  const std::vector<std::size_t> minimized =
      Minimize(prefix, fails, std::chrono::steady_clock::now() + 60s);
  const std::optional<Failure> confirmed = RunScenario(scenario, minimized, {});
  PrintFailure(scenario, confirmed.value_or(*failure), confirmed ? minimized : prefix);
  return false;
}

[[nodiscard]] int Main(int argc, char** argv) {
  const std::optional<Arguments> arguments = Parse(argc, argv);
  if (!arguments) {
    std::fputs(
        "usage: ulog-admission-oracle [--seeds=N] [--first-seed=S] [--events=E] "
        "[--replay=SEED:I,J,...]\n",
        stderr);
    return 2;
  }
  if (!MinimizerSelfCheck()) {
    std::fputs("admission oracle: trace minimizer self-check failed\n", stderr);
    return 1;
  }
  if (arguments->replay) {
    const Scenario scenario = Generate(arguments->replay->first, arguments->events);
    for (const std::size_t index : arguments->replay->second) {
      if (index >= scenario.events.size()) {
        std::fputs("admission oracle: replay event index out of range\n", stderr);
        return 2;
      }
    }
    return RunSeed(scenario, arguments->replay->second, nullptr) ? 0 : 1;
  }
  bool passed = true;
  Coverage coverage{};
  for (std::uint64_t seed = arguments->first_seed; seed < arguments->first_seed + arguments->seeds;
       ++seed) {
    const Scenario scenario = Generate(seed, arguments->events);
    std::vector<std::size_t> trace(scenario.events.size());
    for (std::size_t index = 0; index < trace.size(); ++index) {
      trace[index] = index;
    }
    passed = RunSeed(scenario, trace, &coverage) && passed;
  }
  std::printf("admission oracle: %llu seeds from %llu, %zu events each\n",
              static_cast<unsigned long long>(arguments->seeds),
              static_cast<unsigned long long>(arguments->first_seed), arguments->events);
  bool covered = true;
  for (std::size_t index = 0; index < coverage.size(); ++index) {
    std::printf("  %-24s %llu\n", std::string{kCoveredNames[index]}.c_str(),
                static_cast<unsigned long long>(coverage[index]));
    covered = covered && coverage[index] != 0U;
  }
  if (!arguments->require_coverage) {
    return passed ? 0 : 1;
  }
  if (passed && !covered) {
    std::fputs("admission oracle: the seed set left a modeled behavior unexercised\n", stderr);
  }
  return passed && covered ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) noexcept {
  try {
    return Main(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "admission oracle failed: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("admission oracle failed with an unknown exception\n", stderr);
    return 1;
  }
}
