#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <ulog/fields.hpp>
#include <utility>

namespace ulog::test {
namespace {

static_assert(!std::is_constructible_v<FieldValue, char>);
static_assert(!std::is_constructible_v<FieldValue, char8_t>);
static_assert(!std::is_constructible_v<FieldValue, const int*>);

TEST(FieldValue, MapsEveryNativeTypeToItsFieldKind) {
  EXPECT_EQ(FieldValue{}.Kind(), FieldKind::kNull);
  EXPECT_EQ(FieldValue{kNull}.Kind(), FieldKind::kNull);
  EXPECT_EQ(FieldValue{nullptr}.Kind(), FieldKind::kNull);

  const FieldValue text{"alpha"};
  EXPECT_EQ(text.Kind(), FieldKind::kString);
  EXPECT_EQ(text.Text(), "alpha");
  const std::string owned{"owned"};
  EXPECT_EQ(FieldValue{owned}.Text(), "owned");

  EXPECT_EQ(FieldValue{std::int8_t{-3}}.Kind(), FieldKind::kInt64);
  EXPECT_EQ(FieldValue{-42LL}.Int64(), -42);
  EXPECT_EQ(FieldValue{std::numeric_limits<std::int64_t>::min()}.Int64(),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(FieldValue{std::uint16_t{7}}.Kind(), FieldKind::kUInt64);
  EXPECT_EQ(FieldValue{std::numeric_limits<std::uint64_t>::max()}.UInt64(),
            std::numeric_limits<std::uint64_t>::max());
  EXPECT_EQ(FieldValue{1.5F}.Kind(), FieldKind::kDouble);
  EXPECT_DOUBLE_EQ(FieldValue{-2.25}.Double(), -2.25);
  EXPECT_EQ(FieldValue{true}.Kind(), FieldKind::kBool);
  EXPECT_TRUE(FieldValue{true}.Bool());
  EXPECT_FALSE(FieldValue{false}.Bool());

  const JsonString json{"[1,2]"};
  EXPECT_EQ(FieldValue{json}.Kind(), FieldKind::kJson);
  EXPECT_EQ(FieldValue{json}.Text(), "[1,2]");
}

TEST(JsonString, OwnsOneLineAndTreatsEmptyAsNull) {
  std::string source{"{\n\"x\":\r\n1\r}"};
  const JsonString json{source};
  source.assign("changed");
  EXPECT_EQ(json.View(), "{\"x\":1}");
  EXPECT_EQ(JsonString{}.View(), "null");
  EXPECT_EQ(JsonString{""}.View(), "null");
  EXPECT_EQ(JsonString{"\r\n"}.View(), "null");
  EXPECT_EQ(JsonString{"\"text\""}.View(), "\"text\"");
}

TEST(FieldSet, ReplacesInPlaceAndKeepsInsertionOrder) {
  FieldSet fields;
  EXPECT_EQ(fields.Add("first", "one"), FieldStatus::kAdded);
  EXPECT_EQ(fields.Add("replace", "old"), FieldStatus::kAdded);
  EXPECT_EQ(fields.Add("last", 3), FieldStatus::kAdded);
  EXPECT_EQ(fields.Add("replace", 2.5), FieldStatus::kReplaced);

  ASSERT_EQ(fields.Size(), 3U);
  EXPECT_EQ(fields.Key(0), "first");
  EXPECT_EQ(fields.Key(1), "replace");
  EXPECT_EQ(fields.Key(2), "last");
  EXPECT_EQ(fields.Value(1).Kind(), FieldKind::kDouble);
  EXPECT_DOUBLE_EQ(fields.Value(1).Double(), 2.5);
  EXPECT_EQ(fields.Value(2).Int64(), 3);
}

TEST(FieldSet, FrozenFieldsRejectReplacement) {
  FieldSet fields;
  EXPECT_EQ(fields.Add("frozen", "keep", FieldMode::kFrozen), FieldStatus::kAdded);
  EXPECT_EQ(fields.Add("frozen", "ignored"), FieldStatus::kFrozen);
  EXPECT_EQ(fields.Add("frozen", "ignored", FieldMode::kFrozen), FieldStatus::kFrozen);
  EXPECT_EQ(fields.Value(0).Text(), "keep");
  EXPECT_EQ(fields.Mode(0), FieldMode::kFrozen);

  EXPECT_EQ(fields.Add("later", 1), FieldStatus::kAdded);
  EXPECT_TRUE(fields.Freeze("later"));
  EXPECT_EQ(fields.Add("later", 2), FieldStatus::kFrozen);
  EXPECT_EQ(fields.Value(1).Int64(), 1);

  EXPECT_FALSE(fields.Freeze("missing"));
  EXPECT_EQ(fields.Size(), 2U);
  EXPECT_EQ(fields.Add("missing", 1), FieldStatus::kAdded);
  EXPECT_EQ(fields.Mode(2), FieldMode::kReplaceable);
}

TEST(FieldSet, RejectsEveryTechnicalKeyAndEmptyKeys) {
  constexpr std::string_view kReserved[] = {
      "timestamp",      "level", "module",     "task_id", "thread_id", "text",
      "ulog.truncated", "ulog.", "ulog.other", "TEXT",    "Timestamp", "ULOG.TRUNCATED",
  };
  FieldSet fields;
  for (const std::string_view key : kReserved) {
    SCOPED_TRACE(key);
    EXPECT_TRUE(IsReservedFieldKey(key));
    EXPECT_EQ(fields.Add(key, 1), FieldStatus::kReserved);
  }
  EXPECT_EQ(fields.Add("", 1), FieldStatus::kInvalidKey);
  EXPECT_TRUE(fields.Empty());

  for (const std::string_view key : {"texts", "levels", "ulog", "my.ulog.key", "timestamp_ms"}) {
    SCOPED_TRACE(key);
    EXPECT_FALSE(IsReservedFieldKey(key));
    EXPECT_EQ(fields.Add(key, 1), FieldStatus::kAdded);
  }
}

TEST(FieldSet, CopiesKeysAndValuesOnInsertion) {
  FieldSet fields;
  {
    std::string key{"key"};
    std::string value{"value"};
    fields.Add(key, value);
    key.assign("xxx");
    value.assign("yyyyy");
  }
  EXPECT_EQ(fields.Key(0), "key");
  EXPECT_EQ(fields.Value(0).Text(), "value");
}

TEST(FieldSet, MergePreservesOrderAndFreezeState) {
  FieldSet base;
  base.Add("shared", "base");
  base.Add("locked", "base", FieldMode::kFrozen);

  FieldSet incoming;
  incoming.Add("new", 1, FieldMode::kFrozen);
  incoming.Add("shared", "incoming");
  incoming.Add("locked", "incoming");

  FieldSet copy = base;
  copy.Merge(incoming);
  ASSERT_EQ(copy.Size(), 3U);
  EXPECT_EQ(copy.Key(0), "shared");
  EXPECT_EQ(copy.Value(0).Text(), "incoming");
  EXPECT_EQ(copy.Value(1).Text(), "base");
  EXPECT_EQ(copy.Key(2), "new");
  EXPECT_EQ(copy.Mode(2), FieldMode::kFrozen);
  EXPECT_EQ(incoming.Size(), 3U);

  FieldSet moved = base;
  moved.Merge(std::move(incoming));
  EXPECT_EQ(moved.Size(), 3U);
  EXPECT_EQ(moved.Value(1).Text(), "base");
  EXPECT_EQ(moved.Mode(2), FieldMode::kFrozen);

  // Merging a set into itself must leave it unchanged.
  // NOLINTBEGIN(bugprone-use-after-move)
  FieldSet self = base;
  self.Merge(self);
  self.Merge(std::move(self));
  ASSERT_EQ(self.Size(), 2U);
  EXPECT_EQ(self.Value(0).Text(), "base");
  // NOLINTEND(bugprone-use-after-move)

  FieldSet empty;
  FieldSet source = base;
  empty.Merge(std::move(source));
  ASSERT_EQ(empty.Size(), 2U);
  EXPECT_EQ(empty.Mode(1), FieldMode::kFrozen);
  EXPECT_EQ(empty.Add("locked", "again"), FieldStatus::kFrozen);
}

}  // namespace
}  // namespace ulog::test
