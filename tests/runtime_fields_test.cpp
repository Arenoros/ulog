#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <ulog/fields.hpp>
#include <ulog/log.hpp>
#include <ulog/runtime.hpp>
#include <ulog/testing/in_memory_destination.hpp>
#include <ulog/testing/in_memory_encoded_destination.hpp>
#include <utility>
#include <vector>

#include "consumer/raw_corpus_cases.hpp"

namespace ulog::test {
namespace {

using namespace std::chrono_literals;

constexpr std::size_t kRecordBytes = 1024;

[[nodiscard]] RuntimeConfig FieldRuntimeConfig(std::size_t records = 4) {
  return RuntimeConfig{
      .threshold = Level::kInfo,
      .payload_capacity_bytes = records * kRecordBytes,
      .maximum_record_bytes = kRecordBytes,
      .producer_slots = 1,
      .ingress_cells = records,
      .control_operations = 2,
      .worker_threads = 1,
      .startup_timeout = 1s,
      .destruction_timeout = 1s,
  };
}

template <typename T>
[[nodiscard]] T& RequireValue(std::optional<T>& value) {
  if (!value.has_value()) {
    throw std::logic_error{"expected an optional value"};
  }
  return *value;
}

template <typename T>
[[nodiscard]] const T& RequireValue(const std::optional<T>& value) {
  if (!value.has_value()) {
    throw std::logic_error{"expected an optional value"};
  }
  return *value;
}

[[nodiscard]] testing::ObservedField FieldAt(const testing::ObservedRecord& record,
                                             std::size_t index) {
  auto field = record.Field(index);
  return RequireValue(field);
}

void ExpectShutdown(Runtime& runtime) {
  auto shutdown = runtime.Shutdown();
  ASSERT_TRUE(shutdown);
  const auto completed = shutdown.operation.WaitUntil(std::chrono::steady_clock::now() + 1s);
  ASSERT_EQ(completed.status, OperationWaitStatus::kCompleted);
  ASSERT_TRUE(completed.completion);
  EXPECT_EQ(RequireValue(completed.completion).Outcome(), OperationOutcome::kSucceeded);
}

struct Expected final {
  std::string_view key;
  FieldKind kind;
};

void ExpectKeys(const testing::ObservedRecord& record, std::initializer_list<Expected> expected) {
  ASSERT_EQ(record.FieldCount(), expected.size());
  std::size_t index = 0;
  for (const Expected& field : expected) {
    SCOPED_TRACE(field.key);
    ASSERT_TRUE(record.Field(index));
    const auto observed = FieldAt(record, index++);
    EXPECT_EQ(observed.key, field.key);
    EXPECT_EQ(observed.value.Kind(), field.kind);
  }
  EXPECT_FALSE(record.Field(expected.size()));
}

class RuntimeFields : public ::testing::Test {
 protected:
  void SetUp() override {
    auto created = Runtime::Create(FieldRuntimeConfig(), destination_);
    ASSERT_TRUE(created) << (created.failure ? created.failure->Message() : "missing Runtime");
    runtime_ = std::move(created.runtime);
    logger_ = runtime_->GetLogger();
  }

  [[nodiscard]] testing::ObservedRecord TakeAfterShutdown() {
    ExpectShutdown(*runtime_);
    auto record = destination_.TryTake();
    return std::move(RequireValue(record));
  }

  testing::InMemoryDestination destination_{
      {.capacity_records = 4, .maximum_record_bytes = kRecordBytes}};
  std::unique_ptr<Runtime> runtime_;
  Logger logger_;
};

TEST_F(RuntimeFields, MacroFieldsArriveTypedAndOrderedBeforeTheMessage) {
  const std::string path{"/v1/items"};
  LOG_INFO_TO(logger_,
              Fields{{"string", "alpha"},
                     {"path", path},
                     {"signed", -42},
                     {"unsigned", std::numeric_limits<std::uint64_t>::max()},
                     {"double", -2.25},
                     {"flag", true},
                     {"none", kNull},
                     {"json", JsonString{"{\"x\":\n1}"}}},
              "done in {}ms", 7);

  const auto record = TakeAfterShutdown();
  EXPECT_EQ(record.Message(), "done in 7ms");
  EXPECT_FALSE(record.Truncated());
  ExpectKeys(record, {{"string", FieldKind::kString},
                      {"path", FieldKind::kString},
                      {"signed", FieldKind::kInt64},
                      {"unsigned", FieldKind::kUInt64},
                      {"double", FieldKind::kDouble},
                      {"flag", FieldKind::kBool},
                      {"none", FieldKind::kNull},
                      {"json", FieldKind::kJson}});
  EXPECT_EQ(FieldAt(record, 0).value.Text(), "alpha");
  EXPECT_EQ(FieldAt(record, 1).value.Text(), "/v1/items");
  EXPECT_EQ(FieldAt(record, 2).value.Int64(), -42);
  EXPECT_EQ(FieldAt(record, 3).value.UInt64(), std::numeric_limits<std::uint64_t>::max());
  EXPECT_DOUBLE_EQ(FieldAt(record, 4).value.Double(), -2.25);
  EXPECT_TRUE(FieldAt(record, 5).value.Bool());
  EXPECT_EQ(FieldAt(record, 7).value.Text(), "{\"x\":1}");
}

TEST_F(RuntimeFields, RecordOwnsFieldSetCopiesAfterTheSourceIsDestroyed) {
  {
    FieldSet extra;
    std::string value{"owned-value"};
    extra.Add("owned", value);
    extra.Add("locked", 1, FieldMode::kFrozen);
    LOG_INFO_TO(logger_, extra, "text");
    value.assign("overwritten");
  }
  const auto record = TakeAfterShutdown();
  ExpectKeys(record, {{"owned", FieldKind::kString}, {"locked", FieldKind::kInt64}});
  EXPECT_EQ(FieldAt(record, 0).value.Text(), "owned-value");
  EXPECT_EQ(FieldAt(record, 0).mode, FieldMode::kReplaceable);
  EXPECT_EQ(FieldAt(record, 1).mode, FieldMode::kFrozen);
}

TEST_F(RuntimeFields, RecordAppliesTheCollisionAndReservedKeyPolicy) {
  FieldSet extra;
  extra.Add("first", "one");
  extra.Add("frozen", "keep", FieldMode::kFrozen);
  LOG_INFO_TO(logger_,
              Fields{extra,
                     {"first", "replaced"},
                     {"frozen", "ignored"},
                     {"text", "spoof"},
                     {"Level", "spoof"},
                     {"ulog.truncated", true},
                     {"", 1},
                     {"after", 2},
                     {"after", 3, FieldMode::kFrozen},
                     {"after", 4}},
              "collisions");

  const auto record = TakeAfterShutdown();
  EXPECT_FALSE(record.Truncated());
  ExpectKeys(record, {{"first", FieldKind::kString},
                      {"frozen", FieldKind::kString},
                      {"after", FieldKind::kInt64}});
  EXPECT_EQ(FieldAt(record, 0).value.Text(), "replaced");
  EXPECT_EQ(FieldAt(record, 1).value.Text(), "keep");
  EXPECT_EQ(FieldAt(record, 2).value.Int64(), 3);
  EXPECT_EQ(FieldAt(record, 2).mode, FieldMode::kFrozen);
}

TEST_F(RuntimeFields, LoggerApiReportsEveryFieldStatus) {
  std::vector<FieldStatus> statuses;
  logger_.Log<Level::kWarning>(
      SourceLocation::Current(),
      [&](FieldWriter& fields) {
        statuses.push_back(fields.Add("key", 1));
        statuses.push_back(fields.Add("key", 2));
        statuses.push_back(fields.Add("locked", "x", FieldMode::kFrozen));
        statuses.push_back(fields.Add("locked", "y"));
        statuses.push_back(fields.Add("module", "spoof"));
        statuses.push_back(fields.Add("", "empty"));
        statuses.push_back(fields.Add("huge", std::string(kRecordBytes, 'h')));
      },
      [] { return std::string_view{"api"}; });

  EXPECT_EQ(statuses, (std::vector<FieldStatus>{FieldStatus::kAdded, FieldStatus::kReplaced,
                                                FieldStatus::kAdded, FieldStatus::kFrozen,
                                                FieldStatus::kReserved, FieldStatus::kInvalidKey,
                                                FieldStatus::kTruncated}));
  const auto record = TakeAfterShutdown();
  EXPECT_EQ(record.GetLevel(), Level::kWarning);
  EXPECT_EQ(record.Message(), "api");
  EXPECT_TRUE(record.Truncated());
  ExpectKeys(record, {{"key", FieldKind::kInt64},
                      {"locked", FieldKind::kString},
                      {"ulog.truncated", FieldKind::kBool}});
  EXPECT_EQ(FieldAt(record, 0).value.Int64(), 2);
  EXPECT_TRUE(FieldAt(record, 2).value.Bool());
}

TEST(RuntimeFieldsLaziness, FilteredNullAndRejectedCallsEvaluateNoFieldOperand) {
  testing::InMemoryDestination destination{
      {.capacity_records = 1, .maximum_record_bytes = kRecordBytes, .start_paused = true}};
  auto created = Runtime::Create(FieldRuntimeConfig(1), destination);
  ASSERT_TRUE(created);
  const Logger logger = created.runtime->GetLogger();
  int evaluations = 0;
  const auto counted = [&evaluations] { return ++evaluations; };

  LOG_DEBUG_TO(logger, Fields{{"value", counted()}}, "filtered");
  LOG_INFO_TO(GetNullLogger(), Fields{{"value", counted()}}, "null");
  logger.Log<Level::kDebug>(
      SourceLocation::Current(), [&](FieldWriter& fields) { fields.Add("value", counted()); },
      [] { return std::string_view{"filtered"}; });
  EXPECT_EQ(evaluations, 0);

  LOG_INFO_TO(logger, Fields{{"value", counted()}}, "accepted");
  EXPECT_EQ(evaluations, 1);
  LOG_INFO_TO(logger, Fields{{"value", counted()}}, "lane-full");
  EXPECT_EQ(evaluations, 1);
  EXPECT_EQ(created.runtime->GetSnapshot().dropped_newest_records, 1U);

  destination.Resume();
  ExpectShutdown(*created.runtime);
  auto record = destination.TryTake();
  ASSERT_TRUE(record);
  ASSERT_EQ(RequireValue(record).FieldCount(), 1U);
  EXPECT_EQ(FieldAt(RequireValue(record), 0).value.Int64(), 1);
}

TEST_F(RuntimeFields, OversizedFieldIsDroppedAndMarksTheRecordTruncated) {
  const std::string huge(kRecordBytes, 'x');
  LOG_INFO_TO(logger_, Fields{{"before", 1}, {"huge", huge}, {"after", 2}}, "message");

  const auto record = TakeAfterShutdown();
  EXPECT_TRUE(record.Truncated());
  EXPECT_EQ(record.Message(), "message");
  ExpectKeys(record, {{"before", FieldKind::kInt64},
                      {"after", FieldKind::kInt64},
                      {"ulog.truncated", FieldKind::kBool}});
  EXPECT_LE(record.SerializedBytes(), kRecordBytes);
  EXPECT_LE(record.AccountingChargeBytes(), kRecordBytes);
}

TEST_F(RuntimeFields, FieldsThatFillTheRecordMakeRoomForTheTruncationMarker) {
  logger_.Log<Level::kInfo>(
      SourceLocation::Current(),
      [](FieldWriter& fields) {
        std::string key{"k000"};
        for (int index = 0; index < 64; ++index) {
          key[1] = static_cast<char>('0' + index / 100);
          key[2] = static_cast<char>('0' + index / 10 % 10);
          key[3] = static_cast<char>('0' + index % 10);
          static_cast<void>(fields.Add(key, std::uint64_t{0}));
        }
      },
      [] { return std::string_view{}; });

  const auto record = TakeAfterShutdown();
  EXPECT_TRUE(record.Truncated());
  ASSERT_GT(record.FieldCount(), 1U);
  EXPECT_EQ(FieldAt(record, record.FieldCount() - 1U).key, "ulog.truncated");
  EXPECT_EQ(FieldAt(record, 0).key, "k000");
  EXPECT_LE(record.SerializedBytes(), kRecordBytes);
}

TEST_F(RuntimeFields, ReplacedValuesStayAccountedWithinTheReservation) {
  logger_.Log<Level::kInfo>(
      SourceLocation::Current(),
      [](FieldWriter& fields) {
        const std::string value(64, 'v');
        for (int round = 0; round < 32; ++round) {
          static_cast<void>(fields.Add("same", value));
        }
      },
      [] { return std::string_view{"replaced"}; });

  const auto record = TakeAfterShutdown();
  EXPECT_TRUE(record.Truncated());
  ExpectKeys(record, {{"same", FieldKind::kString}, {"ulog.truncated", FieldKind::kBool}});
  EXPECT_EQ(FieldAt(record, 0).value.Text(), std::string(64, 'v'));
  EXPECT_LE(record.SerializedBytes(), kRecordBytes);
}

TEST(RuntimeFieldsRaw, CommittedCorpusCasesEncodeExactly) {
  testing::InMemoryEncodedDestination destination{
      {.capacity_records = 3, .maximum_record_bytes = kRecordBytes}};
  auto created = Runtime::Create(FieldRuntimeConfig(3), destination);
  ASSERT_TRUE(created);
  raw_corpus::LogAll(created.runtime->GetLogger());
  ExpectShutdown(*created.runtime);

  for (const std::string_view expected : raw_corpus::kFrames) {
    auto frame = destination.TryTake();
    ASSERT_TRUE(frame);
    EXPECT_EQ(RequireValue(frame).Bytes(), expected);
  }
  EXPECT_FALSE(destination.TryTake());
}

}  // namespace
}  // namespace ulog::test
