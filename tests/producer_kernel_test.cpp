#include "producer/producer_kernel.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "producer/producer_lanes.hpp"
#include "producer/record_storage.hpp"

namespace ulog::detail::producer::ingress {

struct ProducerLanesTestAccess final {
  static void MakeLaterSequenceVisible(ProducerLanes& lanes) noexcept {
    constexpr std::size_t kLaterProducer = 1;
    constexpr std::uint64_t kLaterSequence = 1;
    auto& lane = lanes.lanes_[kLaterProducer];
    const std::size_t cell_index = lane.offset;
    lanes.cells_[cell_index].envelope =
        Envelope{.record = {.slot_index = static_cast<std::uint32_t>(cell_index),
                            .producer_index = static_cast<std::uint32_t>(kLaterProducer),
                            .generation = 1,
                            .serialized_bytes = 64,
                            .accounting_charge_bytes = 64},
                 .admission_sequence = kLaterSequence};
    lane.write_position = 1;
    lane.published_position.store(1, std::memory_order_release);
    lanes.next_admission_sequence_.value.store(kLaterSequence + 1, std::memory_order_release);
  }
};

}  // namespace ulog::detail::producer::ingress

namespace ulog::detail::producer::test {
namespace {

struct FakeClock final {
  static EventTimestamp Now(void* context) noexcept {
    auto& clock = *static_cast<FakeClock*>(context);
    const auto call = clock.calls.fetch_add(1, std::memory_order_relaxed);
    return clock.first_timestamp + static_cast<EventTimestamp>(call);
  }

  [[nodiscard]] EventClock Source() noexcept { return EventClock{this, &Now}; }

  std::atomic<std::uint64_t> calls{0};
  EventTimestamp first_timestamp{1'704'067'200'123'456};
};

struct ObservedRecord final {
  bool callback_failed{false};
  std::uint64_t sequence{0};
  Level level{Level::kNone};
  EventTimestamp timestamp{0};
  std::string source_path;
  std::string source_function;
  std::uint32_t source_line{0};
  std::uint32_t source_column{0};
  std::string message;
  bool truncated{false};
  std::size_t serialized_bytes{0};
  std::size_t accounting_charge_bytes{0};
};

void ObserveRecord(void* context, std::uint64_t sequence, const RecordView& record) noexcept {
  auto& observed = *static_cast<ObservedRecord*>(context);
  try {
    observed.sequence = sequence;
    observed.level = record.level();
    observed.timestamp = record.event_timestamp();
    observed.source_path = record.source_path();
    observed.source_function = record.source_function();
    observed.source_line = record.source_line();
    observed.source_column = record.source_column();
    observed.message = record.message();
    observed.truncated = record.truncated();
    observed.serialized_bytes = record.serialized_bytes();
    observed.accounting_charge_bytes = record.accounting_charge_bytes();
  } catch (...) {
    observed.callback_failed = true;
  }
}

[[nodiscard]] KernelConfig SmallConfig(std::size_t capacity_bytes = 512,
                                       std::size_t producer_slots = 1,
                                       std::size_t ingress_cells = 2) {
  return KernelConfig{
      .threshold = Level::kTrace,
      .payload_capacity_bytes = capacity_bytes,
      .maximum_record_bytes = 256,
      .producer_slots = producer_slots,
      .ingress_cells = ingress_cells,
  };
}

[[nodiscard]] constexpr SourceLocation TestSource() noexcept {
  return SourceLocation::Custom("test.cpp", "Test", 1);
}

[[nodiscard]] constexpr std::size_t ExpectedAccountingCharge(std::size_t serialized_bytes) {
  const std::size_t rounded =
      ((serialized_bytes + kAccountingQuantumBytes - 1U) / kAccountingQuantumBytes) *
      kAccountingQuantumBytes;
  return rounded < kAccountingQuantumBytes ? kAccountingQuantumBytes : rounded;
}

BuildStatus CountBuild(void* context, RecordAppender&) {
  ++*static_cast<std::size_t*>(context);
  return BuildStatus::kComplete;
}

TEST(ProducerKernel, ConfigurationErrorsExplainHowToFixTheValue) {
  FakeClock clock;
  try {
    ProducerKernel kernel{KernelConfig{.payload_capacity_bytes = 255,
                                       .maximum_record_bytes = 256,
                                       .producer_slots = 1,
                                       .ingress_cells = 2},
                          clock.Source()};
    static_cast<void>(kernel);
    FAIL() << "invalid capacity was accepted";
  } catch (const std::invalid_argument& error) {
    const std::string_view message = error.what();
    EXPECT_NE(message.find("payload_capacity_bytes"), std::string_view::npos);
    EXPECT_NE(message.find("64-byte"), std::string_view::npos);
    EXPECT_NE(message.find("Set"), std::string_view::npos);
  }
}

TEST(ProducerKernel, ReportsFixedRecordBackingSeparatelyFromThePayloadBudget) {
  FakeClock clock;
  constexpr std::size_t kIngressCells = 8;
  constexpr std::size_t kPayloadCapacity = 4'096;
  ProducerKernel kernel{KernelConfig{.threshold = Level::kTrace,
                                     .payload_capacity_bytes = kPayloadCapacity,
                                     .maximum_record_bytes = 256,
                                     .producer_slots = 4,
                                     .ingress_cells = kIngressCells},
                        clock.Source()};

  const auto snapshot = kernel.GetSnapshot();
  EXPECT_EQ(snapshot.payload_capacity_bytes, kPayloadCapacity);
  EXPECT_EQ(snapshot.fixed_backing_bytes, kIngressCells * sizeof(record::RecordSlot));
  EXPECT_EQ(snapshot.logical_retained_bytes, 0U);
  EXPECT_EQ(snapshot.physical_retained_bytes, 0U);
  EXPECT_TRUE(snapshot.accounting_sample_consistent);
  EXPECT_EQ(kMaximumIngressCells * sizeof(record::RecordSlot), 1'052'672U);
}

TEST(ProducerKernel, FilteringAndMissingRegistrationPrecedeCallerEvaluation) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(), clock.Source()};
  const Logger logger = kernel.GetLogger();
  std::size_t evaluations = 0;

  logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
    ++evaluations;
    return "missing registration";
  });

  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  kernel.SetLevel(Level::kWarning);
  logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
    ++evaluations;
    return "filtered";
  });
  const auto native = kernel.TryPublish(producer, Level::kInfo, TestSource(),
                                        BuildOperation{&evaluations, &CountBuild});

  const auto snapshot = kernel.GetSnapshot();
  EXPECT_EQ(native.outcome, PublishOutcome::kFiltered);
  EXPECT_EQ(evaluations, 0U);
  EXPECT_EQ(clock.calls.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(snapshot.attempted_records, 1U);
  EXPECT_EQ(snapshot.rejected_no_producer, 1U);
  EXPECT_EQ(snapshot.accepted_records, 0U);
}

TEST(ProducerKernel, AcceptedLoggerCallOwnsRecordAndCapturesClockOnce) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(), clock.Source()};
  {
    auto producer = kernel.TryRegisterProducer();
    ASSERT_TRUE(producer);
    const Logger logger = kernel.GetLogger();
    std::string path = "caller.cpp";
    std::string function = "Caller";
    std::string message = "owned message";
    const SourceLocation source = SourceLocation::Custom(path, function, 42, 7);
    std::size_t evaluations = 0;

    logger.Log<Level::kWarning>(source, [&]() -> std::string_view {
      ++evaluations;
      return message;
    });
    path.assign("mutated.cpp");
    function.assign("Mutated");
    message.assign("mutated message");

    const auto retained = kernel.GetSnapshot();
    ObservedRecord observed;
    ASSERT_EQ(kernel.TryConsume(&observed, &ObserveRecord), ConsumeStatus::kRecord);
    EXPECT_EQ(observed.sequence, 0U);
    EXPECT_EQ(observed.level, Level::kWarning);
    EXPECT_EQ(observed.timestamp, clock.first_timestamp);
    EXPECT_EQ(observed.source_path, "caller.cpp");
    EXPECT_EQ(observed.source_function, "Caller");
    EXPECT_EQ(observed.source_line, 42U);
    EXPECT_EQ(observed.source_column, 7U);
    EXPECT_EQ(observed.message, "owned message");
    EXPECT_FALSE(observed.truncated);
    EXPECT_EQ(evaluations, 1U);
    EXPECT_EQ(clock.calls.load(std::memory_order_relaxed), 1U);
    EXPECT_GT(observed.serialized_bytes, observed.message.size());
    EXPECT_EQ(observed.accounting_charge_bytes,
              ExpectedAccountingCharge(observed.serialized_bytes));
    EXPECT_EQ(retained.retained_records, 1U);
    EXPECT_EQ(retained.logical_retained_bytes, observed.serialized_bytes);
    EXPECT_EQ(retained.physical_retained_bytes, 256U);
    EXPECT_LE(observed.accounting_charge_bytes, retained.physical_retained_bytes);
    EXPECT_TRUE(retained.accounting_sample_consistent);
  }

  const auto snapshot = kernel.GetSnapshot();
  EXPECT_EQ(snapshot.attempted_records, 1U);
  EXPECT_EQ(snapshot.accepted_records, 1U);
  EXPECT_EQ(snapshot.consumed_records, 1U);
  EXPECT_EQ(snapshot.logical_retained_bytes, 0U);
  EXPECT_EQ(snapshot.physical_retained_bytes, 0U);
  EXPECT_EQ(snapshot.active_producer_slots, 0U);
}

TEST(ProducerKernel, BudgetAndLaneRejectionDoNotEvaluateTheMessage) {
  FakeClock budget_clock;
  ProducerKernel budget_kernel{SmallConfig(256, 1, 2), budget_clock.Source()};
  auto budget_producer = budget_kernel.TryRegisterProducer();
  ASSERT_TRUE(budget_producer);
  const Logger budget_logger = budget_kernel.GetLogger();
  budget_logger.Log<Level::kInfo>(SourceLocation::Custom("b.cpp", "B", 1), [] { return "first"; });
  std::size_t budget_evaluations = 0;
  budget_logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
    ++budget_evaluations;
    return "budget rejected";
  });
  EXPECT_EQ(budget_evaluations, 0U);
  EXPECT_EQ(budget_clock.calls.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(budget_kernel.GetSnapshot().rejected_budget, 1U);

  ObservedRecord discarded;
  ASSERT_EQ(budget_kernel.TryConsume(&discarded, &ObserveRecord), ConsumeStatus::kRecord);
  budget_logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
    ++budget_evaluations;
    return "retry";
  });
  EXPECT_EQ(budget_evaluations, 1U);

  FakeClock lane_clock;
  ProducerKernel lane_kernel{SmallConfig(512, 1, 1), lane_clock.Source()};
  auto lane_producer = lane_kernel.TryRegisterProducer();
  ASSERT_TRUE(lane_producer);
  const Logger lane_logger = lane_kernel.GetLogger();
  lane_logger.Log<Level::kInfo>(TestSource(), [] { return "first"; });
  std::size_t lane_evaluations = 0;
  lane_logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
    ++lane_evaluations;
    return "lane rejected";
  });
  EXPECT_EQ(lane_evaluations, 0U);
  EXPECT_EQ(lane_clock.calls.load(std::memory_order_relaxed), 1U);
  EXPECT_EQ(lane_kernel.GetSnapshot().rejected_lane_full, 1U);
}

struct CallerError final : std::exception {};

TEST(ProducerKernel, ThrowingFactoryAbandonsAllCapacityAndConsumesNoSequence) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(256, 1, 1), clock.Source()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const Logger logger = kernel.GetLogger();
  bool should_throw = true;

  EXPECT_THROW(logger.Log<Level::kInfo>(TestSource(),
                                        [&]() -> std::string_view {
                                          if (should_throw) {
                                            throw CallerError{};
                                          }
                                          return "not thrown";
                                        }),
               CallerError);
  const auto abandoned = kernel.GetSnapshot();
  EXPECT_EQ(abandoned.abandoned_builds, 1U);
  EXPECT_EQ(abandoned.logical_retained_bytes, 0U);
  EXPECT_EQ(abandoned.retained_records, 0U);
  EXPECT_EQ(abandoned.physical_retained_bytes, 256U);

  logger.Log<Level::kInfo>(TestSource(), [] { return "after exception"; });
  ObservedRecord observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &ObserveRecord), ConsumeStatus::kRecord);
  EXPECT_EQ(observed.sequence, 0U);
  EXPECT_EQ(observed.message, "after exception");
  producer = {};
  const auto reconciled = kernel.GetSnapshot();
  EXPECT_EQ(reconciled.logical_retained_bytes, 0U);
  EXPECT_EQ(reconciled.physical_retained_bytes, 0U);
  EXPECT_TRUE(reconciled.accounting_sample_consistent);
}

TEST(ProducerKernel, SequenceFollowsPublicationRatherThanReservationOrder) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(512, 2, 2), clock.Source()};
  const Logger logger = kernel.GetLogger();
  std::latch slow_builder_started{1};
  std::latch release_slow_builder{1};

  std::thread slow{[&] {
    auto producer = kernel.TryRegisterProducer();
    ASSERT_TRUE(producer);
    logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
      slow_builder_started.count_down();
      release_slow_builder.wait();
      return "slow";
    });
  }};
  slow_builder_started.wait();

  std::thread fast{[&] {
    auto producer = kernel.TryRegisterProducer();
    ASSERT_TRUE(producer);
    logger.Log<Level::kInfo>(TestSource(), [] { return "fast"; });
  }};
  fast.join();

  ObservedRecord first;
  ASSERT_EQ(kernel.TryConsume(&first, &ObserveRecord), ConsumeStatus::kRecord);
  EXPECT_EQ(first.sequence, 0U);
  EXPECT_EQ(first.message, "fast");

  release_slow_builder.count_down();
  slow.join();
  ObservedRecord second;
  ASSERT_EQ(kernel.TryConsume(&second, &ObserveRecord), ConsumeStatus::kRecord);
  EXPECT_EQ(second.sequence, 1U);
  EXPECT_EQ(second.message, "slow");
}

struct HeldConsumer final {
  std::latch entered{1};
  std::latch release{1};
  std::string message;
  bool callback_failed{false};
};

void HoldRecord(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& held = *static_cast<HeldConsumer*>(context);
  try {
    held.message = record.message();
  } catch (...) {
    held.callback_failed = true;
  }
  held.entered.count_down();
  held.release.wait();
  try {
    held.message.append(record.message());
  } catch (...) {
    held.callback_failed = true;
  }
}

TEST(ProducerKernel, ConsumerClaimPreventsRecordSlotOverwrite) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(512, 1, 2), clock.Source()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const Logger logger = kernel.GetLogger();
  logger.Log<Level::kInfo>(TestSource(), [] { return "one"; });
  logger.Log<Level::kInfo>(TestSource(), [] { return "two"; });

  HeldConsumer held;
  std::thread consumer{
      [&] { EXPECT_EQ(kernel.TryConsume(&held, &HoldRecord), ConsumeStatus::kRecord); }};
  held.entered.wait();

  EXPECT_EQ(kernel.TryConsume(nullptr, nullptr), ConsumeStatus::kPending);

  std::size_t evaluations = 0;
  logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
    ++evaluations;
    return "three";
  });
  EXPECT_EQ(evaluations, 0U);
  EXPECT_EQ(kernel.GetSnapshot().rejected_lane_full, 1U);

  held.release.count_down();
  consumer.join();
  EXPECT_FALSE(held.callback_failed);
  EXPECT_EQ(held.message, "oneone");

  ObservedRecord second;
  ASSERT_EQ(kernel.TryConsume(&second, &ObserveRecord), ConsumeStatus::kRecord);
  EXPECT_EQ(second.message, "two");
  logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
    ++evaluations;
    return "three";
  });
  EXPECT_EQ(evaluations, 1U);
}

TEST(ProducerKernel, IngressReportsPendingWhenALaterSequenceIsAlreadyVisible) {
  ingress::ProducerLanes lanes{2, 2};
  ingress::ProducerLanesTestAccess::MakeLaterSequenceVisible(lanes);

  const auto claim = lanes.TryClaimConsumption();
  EXPECT_FALSE(claim);
  EXPECT_EQ(claim.status(), ingress::ConsumptionStatus::kPending);
}

struct StructuredBuild final {
  static BuildStatus Invoke(void*, RecordAppender& writer) {
    const auto prefix = writer.Append("count=");
    const auto number = writer.Append(std::int64_t{-7});
    auto output = writer.FormatOutput();
    *output++ = ':';
    *output++ = 'o';
    *output++ = 'k';
    const bool fields = writer.AddField("kind", "test") &&
                        writer.AddField("unsigned", std::uint64_t{42}) &&
                        writer.AddField("ratio", 1.25) && writer.AddField("ready", true) &&
                        writer.AddField("optional", kNull);
    return !prefix.truncated && !number.truncated && fields ? BuildStatus::kComplete
                                                            : BuildStatus::kInvalid;
  }
};

struct StructuredObservation final {
  bool callback_failed{false};
  std::string message;
  std::vector<std::string> keys;
  std::uint64_t unsigned_value{0};
  double ratio{0};
  bool ready{false};
  bool optional_is_null{false};
};

void ObserveStructured(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& observed = *static_cast<StructuredObservation*>(context);
  try {
    observed.message = record.message();
    for (std::size_t index = 0; index < record.field_count(); ++index) {
      const auto field = record.FieldAt(index);
      if (!field) {
        continue;
      }
      observed.keys.emplace_back(field->key());
      if (field->key() == "unsigned") {
        observed.unsigned_value = field->AsUInt64().value_or(0);
      } else if (field->key() == "ratio") {
        observed.ratio = field->AsDouble().value_or(0);
      } else if (field->key() == "ready") {
        observed.ready = field->AsBool().value_or(false);
      } else if (field->key() == "optional") {
        observed.optional_is_null = field->IsNull();
      }
    }
  } catch (...) {
    observed.callback_failed = true;
  }
}

TEST(ProducerKernel, NativeScalarStringAndFmtOrientedWritesShareOneTransaction) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(512, 1, 2), clock.Source()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  const auto result = kernel.TryPublish(producer, Level::kDebug,
                                        SourceLocation::Custom("structured.cpp", "Build", 8, 3),
                                        BuildOperation{nullptr, &StructuredBuild::Invoke});
  ASSERT_EQ(result.outcome, PublishOutcome::kAccepted);
  ASSERT_EQ(result.admission_sequence, 0U);
  EXPECT_FALSE(result.truncated);

  StructuredObservation observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &ObserveStructured), ConsumeStatus::kRecord);
  EXPECT_FALSE(observed.callback_failed);
  EXPECT_EQ(observed.message, "count=-7:ok");
  ASSERT_EQ(observed.keys.size(), 5U);
  EXPECT_EQ(observed.keys.front(), "kind");
  EXPECT_EQ(observed.unsigned_value, 42U);
  EXPECT_DOUBLE_EQ(observed.ratio, 1.25);
  EXPECT_TRUE(observed.ready);
  EXPECT_TRUE(observed.optional_is_null);
}

struct OversizedBuild final {
  static BuildStatus Invoke(void* context, RecordAppender& writer) {
    const auto message = *static_cast<const std::string_view*>(context);
    static_cast<void>(writer.Append(message));
    return BuildStatus::kComplete;
  }
};

struct TruncationObservation final {
  bool callback_failed{false};
  bool truncated{false};
  bool has_technical_field{false};
  std::string message;
};

void ObserveTruncation(void* context, std::uint64_t, const RecordView& record) noexcept {
  auto& observed = *static_cast<TruncationObservation*>(context);
  try {
    observed.truncated = record.truncated();
    observed.message = record.message();
    for (std::size_t index = 0; index < record.field_count(); ++index) {
      const auto field = record.FieldAt(index);
      observed.has_technical_field =
          observed.has_technical_field ||
          (field && field->key() == "ulog.truncated" && field->AsBool().value_or(false));
    }
  } catch (...) {
    observed.callback_failed = true;
  }
}

TEST(ProducerKernel, OversizedMessageIsUtf8TruncatedInsideTheReservation) {
  FakeClock clock;
  ProducerKernel kernel{KernelConfig{.threshold = Level::kTrace,
                                     .payload_capacity_bytes = 256,
                                     .maximum_record_bytes = 256,
                                     .producer_slots = 1,
                                     .ingress_cells = 1},
                        clock.Source()};
  auto producer = kernel.TryRegisterProducer();
  ASSERT_TRUE(producer);
  std::string message(300, 'x');
  message.append("\xE2\x82\xAC");
  const std::string_view view = message;
  const auto result = kernel.TryPublish(
      producer, Level::kInfo, SourceLocation::Custom("t.cpp", "T", 1),
      BuildOperation{const_cast<std::string_view*>(&view), &OversizedBuild::Invoke});
  ASSERT_EQ(result.outcome, PublishOutcome::kAccepted);
  EXPECT_TRUE(result.truncated);

  TruncationObservation observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &ObserveTruncation), ConsumeStatus::kRecord);
  EXPECT_FALSE(observed.callback_failed);
  EXPECT_TRUE(observed.truncated);
  EXPECT_TRUE(observed.has_technical_field);
  EXPECT_LT(observed.message.size(), message.size());
  EXPECT_EQ(observed.message.find('\0'), std::string::npos);
}

TEST(ProducerKernel, RetiringSlotIsReusedOnlyAfterItsLaneAndCreditAreDrained) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(256, 1, 1), clock.Source()};
  {
    auto first = kernel.TryRegisterProducer();
    ASSERT_TRUE(first);
    kernel.GetLogger().Log<Level::kInfo>(TestSource(), [] { return "queued"; });
  }

  EXPECT_FALSE(kernel.TryRegisterProducer());
  const auto retiring = kernel.GetSnapshot();
  EXPECT_EQ(retiring.active_producer_slots, 0U);
  EXPECT_EQ(retiring.retiring_producer_slots, 1U);

  ObservedRecord observed;
  ASSERT_EQ(kernel.TryConsume(&observed, &ObserveRecord), ConsumeStatus::kRecord);
  auto replacement = kernel.TryRegisterProducer();
  EXPECT_TRUE(replacement);
}

TEST(ProducerKernel, StaleThreadRegistrationCannotPublishAfterSlotReuse) {
  FakeClock clock;
  ProducerKernel kernel{SmallConfig(256, 1, 1), clock.Source()};
  const Logger logger = kernel.GetLogger();
  std::latch registration_ready{1};
  std::latch use_stale_registration{1};
  std::latch replacement_ready{1};
  std::latch release_replacement{1};
  std::optional<ProducerKernel::ProducerRegistration> transferred_registration;
  std::atomic<bool> initial_registration_succeeded{false};
  std::atomic<bool> replacement_registration_succeeded{false};
  std::atomic<std::size_t> stale_builder_calls{0};

  std::thread original_producer{[&] {
    auto registration = kernel.TryRegisterProducer();
    initial_registration_succeeded.store(static_cast<bool>(registration),
                                         std::memory_order_relaxed);
    if (registration) {
      transferred_registration.emplace(std::move(registration));
    }
    registration_ready.count_down();
    use_stale_registration.wait();
    if (initial_registration_succeeded.load(std::memory_order_relaxed)) {
      logger.Log<Level::kInfo>(TestSource(), [&]() -> std::string_view {
        stale_builder_calls.fetch_add(1, std::memory_order_relaxed);
        return "stale";
      });
    }
  }};

  registration_ready.wait();
  if (!initial_registration_succeeded.load(std::memory_order_relaxed)) {
    use_stale_registration.count_down();
    original_producer.join();
    FAIL() << "initial producer registration failed";
  }
  if (!transferred_registration) {
    use_stale_registration.count_down();
    original_producer.join();
    FAIL() << "producer registration was not transferred";
  }
  auto retired_registration = std::move(*transferred_registration);
  transferred_registration.reset();
  retired_registration = {};

  std::thread replacement_producer{[&] {
    auto registration = kernel.TryRegisterProducer();
    replacement_registration_succeeded.store(static_cast<bool>(registration),
                                             std::memory_order_relaxed);
    replacement_ready.count_down();
    release_replacement.wait();
  }};
  replacement_ready.wait();
  if (!replacement_registration_succeeded.load(std::memory_order_relaxed)) {
    release_replacement.count_down();
    use_stale_registration.count_down();
    replacement_producer.join();
    original_producer.join();
    FAIL() << "replacement producer registration failed";
  }

  use_stale_registration.count_down();
  original_producer.join();
  EXPECT_EQ(stale_builder_calls.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(kernel.GetSnapshot().rejected_no_producer, 1U);

  release_replacement.count_down();
  replacement_producer.join();
  EXPECT_EQ(kernel.GetSnapshot().active_producer_slots, 0U);
}

}  // namespace
}  // namespace ulog::detail::producer::test
