#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include <fmt/format.h>

#include "prototypes/record_storage/record_storage.hpp"

namespace allocation_tracking {

std::atomic<std::uint64_t> allocation_count{0};

void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  if (void* memory = std::malloc(std::max(size, std::size_t{1}))) {
    return memory;
  }
  throw std::bad_alloc{};
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
#if defined(_MSC_VER)
  if (void* memory = _aligned_malloc(std::max(size, std::size_t{1}), alignment)) {
    return memory;
  }
#else
  const std::size_t requested_size = std::max(size, std::size_t{1});
  if (requested_size > std::numeric_limits<std::size_t>::max() - alignment + 1U) {
    throw std::bad_alloc{};
  }
  const std::size_t aligned_size = ((requested_size + alignment - 1U) / alignment) * alignment;
  if (void* memory = std::aligned_alloc(alignment, aligned_size)) {
    return memory;
  }
#endif
  throw std::bad_alloc{};
}

void FreeAligned(void* memory) noexcept {
#if defined(_MSC_VER)
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

}  // namespace allocation_tracking

void* operator new(std::size_t size) { return allocation_tracking::Allocate(size); }
void* operator new[](std::size_t size) { return allocation_tracking::Allocate(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

void* operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_tracking::AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return allocation_tracking::AllocateAligned(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* memory, std::align_val_t) noexcept {
  allocation_tracking::FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
  allocation_tracking::FreeAligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
  allocation_tracking::FreeAligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  allocation_tracking::FreeAligned(memory);
}

namespace {

namespace storage = ulog::benchmark_support::record_storage;
using storage::ChunkedPolicy;
using storage::ContiguousPolicy;
using storage::FieldKind;
using storage::HybridPolicy;
using storage::RecordSeed;
using storage::RecordView;
using storage::TextView;
using ulog::benchmark_support::RecordFootprint;

int failure_count = 0;

void Check(bool condition, std::string_view test, std::string_view message) {
  if (!condition) {
    ++failure_count;
    std::cerr << test << ": " << message << '\n';
  }
}

void CheckText(TextView actual, std::string_view expected, std::string_view test,
               std::string_view field) {
  if (!actual.Equals(expected)) {
    ++failure_count;
    std::cerr << test << ": " << field << " differs (actual bytes=" << actual.size()
              << ", expected bytes=" << expected.size() << ")\n";
  }
}

[[nodiscard]] bool SameFootprint(const RecordFootprint& left,
                                 const RecordFootprint& right) noexcept {
  return left.requested_message_bytes == right.requested_message_bytes &&
         left.stored_message_bytes == right.stored_message_bytes &&
         left.owned_payload_bytes == right.owned_payload_bytes &&
         left.metadata_bytes == right.metadata_bytes &&
         left.fragmentation_bytes == right.fragmentation_bytes &&
         left.accounting_charge_bytes == right.accounting_charge_bytes &&
         left.minimum_accounting_charge_bytes == right.minimum_accounting_charge_bytes &&
         left.truncated == right.truncated;
}

void CheckAccounting(const RecordFootprint& footprint, std::string_view test) {
  Check(footprint.SerializedBytes() + footprint.fragmentation_bytes ==
            footprint.accounting_charge_bytes,
        test, "payload + metadata + fragmentation must equal the accounting charge");
}

[[nodiscard]] std::span<const std::byte> Bytes(std::string_view value) noexcept {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

template <typename Policy>
[[nodiscard]] RecordFootprint FullCapacityPlan() noexcept {
  return storage::BenchmarkAsciiRecordShape<Policy>(storage::kMaximumBenchmarkStoredMessageBytes);
}

template <typename Policy>
[[nodiscard]] RecordFootprint MessageCapacityPlan(std::size_t message_capacity) noexcept {
  const std::size_t serialized_bytes = storage::kSerializedRecordMetadataBytes + message_capacity;
  const std::size_t charge = Policy::AccountingCharge(serialized_bytes);
  return RecordFootprint{.requested_message_bytes = message_capacity,
                         .stored_message_bytes = message_capacity,
                         .owned_payload_bytes = message_capacity,
                         .metadata_bytes = storage::kSerializedRecordMetadataBytes,
                         .fragmentation_bytes = charge - serialized_bytes,
                         .accounting_charge_bytes = charge,
                         .minimum_accounting_charge_bytes = Policy::kMinimumAccountingChargeBytes,
                         .truncated = false};
}

template <typename Policy>
void TestCanonicalRecipe(std::string_view candidate) {
  const std::string test = std::string{candidate} + " canonical recipe";
  const std::string payload{"A\0caf\xC3\xA9 \xF0\x9F\x8C\x8D", 12};
  const auto footprint = storage::DescribeRecord<Policy>(Bytes(payload));
  storage::RecordSlot<Policy> slot;
  auto writer = slot.Begin(storage::BenchmarkRecordSeed(), footprint);
  const RecordView view = storage::BuildBenchmarkRecord<Policy>(std::move(writer), Bytes(payload));

  Check(static_cast<bool>(view), test, "canonical record must publish");
  Check(SameFootprint(view.footprint(), footprint), test,
        "pre-admission and published footprints must match exactly");
  CheckAccounting(view.footprint(), test);
  CheckText(view.message(), payload, test, "message");
  std::size_t iterated_message_bytes = 0;
  for (const std::byte value : view.message()) {
    Check(value == Bytes(payload)[iterated_message_bytes], test, "segmented byte iterator differs");
    ++iterated_message_bytes;
  }
  Check(iterated_message_bytes == payload.size(), test, "segmented byte iterator length differs");
  CheckText(view.source_path(), storage::kBenchmarkSourcePath, test, "source path");
  CheckText(view.source_function(), storage::kBenchmarkSourceFunction, test, "source function");
  Check(view.source_line() == storage::kBenchmarkSourceLine, test, "source line differs");
  Check(view.event_timestamp() == storage::kBenchmarkEventTimestamp, test,
        "event timestamp differs");
  Check(view.field_count() == storage::kBenchmarkFieldCount, test, "canonical field count differs");

  const auto string_field = view.FieldAt(0);
  const auto int64_field = view.FieldAt(1);
  const auto uint64_field = view.FieldAt(2);
  const auto double_field = view.FieldAt(3);
  const auto bool_field = view.FieldAt(4);
  const auto null_field = view.FieldAt(5);
  Check(string_field && string_field->key().Equals(storage::kBenchmarkStringFieldKey) &&
            string_field->AsString() &&
            string_field->AsString()->Equals(storage::kBenchmarkStringFieldValue),
        test, "canonical string field differs");
  Check(int64_field && int64_field->key().Equals(storage::kBenchmarkInt64FieldKey) &&
            int64_field->AsInt64() == std::int64_t{-7},
        test, "canonical int64 field differs");
  Check(uint64_field && uint64_field->key().Equals(storage::kBenchmarkUInt64FieldKey) &&
            uint64_field->AsUInt64() == std::uint64_t{42},
        test, "canonical uint64 field differs");
  Check(double_field && double_field->key().Equals(storage::kBenchmarkDoubleFieldKey) &&
            double_field->AsDouble() == 1.25,
        test, "canonical double field differs");
  Check(bool_field && bool_field->key().Equals(storage::kBenchmarkBoolFieldKey) &&
            bool_field->AsBool() == true,
        test, "canonical bool field differs");
  Check(null_field && null_field->key().Equals(storage::kBenchmarkNullFieldKey) &&
            null_field->IsNull(),
        test, "canonical null field differs");

  const std::uint64_t expected_payload =
      storage::kBenchmarkFixedPayloadBytes + static_cast<std::uint64_t>(payload.size());
  Check(view.footprint().owned_payload_bytes == expected_payload, test,
        "canonical owned payload accounting differs");
  Check(view.footprint().metadata_bytes == storage::kBenchmarkFixedMetadataBytes, test,
        "canonical metadata accounting differs");
}

template <typename Policy>
void TestOwnershipTypesAndSegmentBoundaries(std::string_view candidate) {
  const std::string test = std::string{candidate} + " ownership and segment boundaries";
  storage::RecordSlot<Policy> slot;
  RecordView view;
  std::string expected_path;
  std::string expected_function;
  std::string expected_message;
  std::string expected_long_key;
  std::string expected_string_value;

  {
    std::string path(700, 'p');
    std::string function(1'100, 'f');
    std::string message(2'500, 'm');
    message[199] = static_cast<char>(0xC3);
    message[200] = static_cast<char>(0xA9);
    message[710] = static_cast<char>(0xF0);
    message[711] = static_cast<char>(0x9F);
    message[712] = static_cast<char>(0x8C);
    message[713] = static_cast<char>(0x8D);
    std::string long_key(600, 'k');
    std::string string_value(900, 'v');
    string_value[17] = '\0';
    string_value[255] = static_cast<char>(0xC3);
    string_value[256] = static_cast<char>(0xA9);

    expected_path = path;
    expected_function = function;
    expected_message = message;
    expected_long_key = long_key;
    expected_string_value = string_value;

    auto writer = slot.Begin(RecordSeed{.source_path = path,
                                        .source_function = function,
                                        .source_line = 404,
                                        .event_timestamp = -123'456},
                             FullCapacityPlan<Policy>());
    Check(writer.AddField(long_key, string_value), test, "long string field must fit");
    Check(writer.AddField("dup", std::int64_t{-99}), test, "int64 field must fit");
    Check(writer.AddField("dup", std::uint64_t{99}), test, "uint64 field must fit");
    Check(writer.AddField("double", -0.5), test, "double field must fit");
    Check(writer.AddField("bool", false), test, "bool field must fit");
    Check(writer.AddField("null", storage::kNull), test, "null field must fit");
    static_cast<void>(writer.Append(message));
    view = std::move(writer).Publish();

    std::fill(path.begin(), path.end(), 'x');
    std::fill(function.begin(), function.end(), 'x');
    std::fill(message.begin(), message.end(), 'x');
    std::fill(long_key.begin(), long_key.end(), 'x');
    std::fill(string_value.begin(), string_value.end(), 'x');
  }

  CheckText(view.source_path(), expected_path, test, "owned source path");
  CheckText(view.source_function(), expected_function, test, "owned source function");
  CheckText(view.message(), expected_message, test, "owned message");
  Check(view.source_line() == 404 && view.event_timestamp() == -123'456, test,
        "source scalar metadata differs");
  Check(view.field_count() == 6, test, "ordered field count differs");
  Check(view.FieldAt(6) == std::nullopt, test, "out-of-range field must be absent");

  const auto string_field = view.FieldAt(0);
  const auto signed_field = view.FieldAt(1);
  const auto unsigned_field = view.FieldAt(2);
  const auto double_field = view.FieldAt(3);
  const auto bool_field = view.FieldAt(4);
  const auto null_field = view.FieldAt(5);
  Check(string_field && string_field->kind() == FieldKind::kString &&
            string_field->key().Equals(expected_long_key) && string_field->AsString() &&
            string_field->AsString()->Equals(expected_string_value),
        test, "cross-segment string field differs");
  Check(signed_field && signed_field->key().Equals("dup") &&
            signed_field->AsInt64() == std::int64_t{-99},
        test, "first duplicate field differs");
  Check(unsigned_field && unsigned_field->key().Equals("dup") &&
            unsigned_field->AsUInt64() == std::uint64_t{99},
        test, "second duplicate field differs");
  Check(double_field && double_field->AsDouble() == -0.5, test, "double field differs");
  Check(bool_field && bool_field->AsBool() == false, test, "bool field differs");
  Check(null_field && null_field->IsNull(), test, "null field differs");

  const std::size_t field_payload =
      expected_long_key.size() + expected_string_value.size() +
      std::string_view{"dup"}.size() * 2U + std::string_view{"double"}.size() +
      std::string_view{"bool"}.size() + std::string_view{"null"}.size();
  const std::size_t expected_payload =
      expected_path.size() + expected_function.size() + expected_message.size() + field_payload;
  Check(view.footprint().owned_payload_bytes == expected_payload, test,
        "owned payload accounting differs");
  Check(view.footprint().metadata_bytes ==
            storage::kSerializedRecordMetadataBytes + 6U * storage::kSerializedFieldMetadataBytes,
        test, "metadata accounting differs");
  CheckAccounting(view.footprint(), test);

  auto blocked_writer = slot.Begin({}, FullCapacityPlan<Policy>());
  Check(!static_cast<bool>(blocked_writer), test,
        "a published slot must not be reused before quiescent Reset");
  slot.Reset();
  auto reset_writer = slot.Begin({}, FullCapacityPlan<Policy>());
  Check(static_cast<bool>(reset_writer), test, "Reset must enable explicit quiescent reuse");
}

template <typename Policy>
void TestConsumerAfterProducerJoin(std::string_view candidate) {
  const std::string test = std::string{candidate} + " consumer after producer join";
  storage::RecordSlot<Policy> slot;
  RecordView view;
  std::thread producer{[&] {
    auto writer = slot.Begin({}, FullCapacityPlan<Policy>());
    static_cast<void>(writer.Append("published by producer"));
    view = std::move(writer).Publish();
  }};
  producer.join();
  CheckText(view.message(), "published by producer", test, "joined producer message");
}

template <typename Policy>
void TestNativeAndFmtEquivalence(std::string_view candidate) {
  const std::string test = std::string{candidate} + " native and fmt equivalence";
  storage::RecordSlot<Policy> native_slot;
  storage::RecordSlot<Policy> fmt_slot;
  auto native_writer = native_slot.Begin({}, FullCapacityPlan<Policy>());
  static_cast<void>(native_writer.Append("value="));
  static_cast<void>(native_writer.Append(std::int64_t{-17}));
  static_cast<void>(native_writer.Append(", ok="));
  static_cast<void>(native_writer.Append(true));
  const RecordView native_view = std::move(native_writer).Publish();

  auto fmt_writer = fmt_slot.Begin({}, FullCapacityPlan<Policy>());
  static_cast<void>(fmt::format_to(fmt_writer.FormatOutput(), "value={}, ok={}", -17, true));
  const RecordView fmt_view = std::move(fmt_writer).Publish();
  Check(native_view.message().Equals(fmt_view.message()), test,
        "native scalar/string and fmt-oriented paths must produce identical bytes");
}

template <typename Policy>
void TestPublishedChargeShrinksToActualRecord(std::string_view candidate) {
  const std::string test = std::string{candidate} + " published charge shrink";
  storage::RecordSlot<Policy> slot;
  auto writer = slot.Begin({}, FullCapacityPlan<Policy>());
  static_cast<void>(writer.Append("short"));
  const RecordView view = std::move(writer).Publish();
  const std::uint64_t expected_charge =
      Policy::AccountingCharge(static_cast<std::size_t>(view.footprint().SerializedBytes()));

  Check(view.footprint().accounting_charge_bytes == expected_charge, test,
        "published Record kept the worst-case reservation charge instead of its actual charge");
  CheckAccounting(view.footprint(), test);
}

template <typename Policy>
void TestUtf8Truncation(std::string_view candidate, std::size_t boundary_prefix) {
  const std::string test = std::string{candidate} + " UTF-8 truncation";
  const std::string acute = "\xC3\xA9";
  const std::string world = "\xF0\x9F\x8C\x8D";
  for (const std::string_view suffix : {std::string_view{acute}, std::string_view{world}}) {
    std::string message(boundary_prefix, 'a');
    message.append(suffix);
    const std::size_t capacity = message.size() - 1U;
    storage::RecordSlot<Policy> slot;
    auto writer = slot.Begin({}, MessageCapacityPlan<Policy>(capacity));
    static_cast<void>(writer.Append(message));
    const RecordView view = std::move(writer).Publish();
    CheckText(view.message(), std::string_view{message}.substr(0, boundary_prefix), test,
              "message must end before a partial code point");
    Check(view.footprint().truncated, test, "partial code point must set truncated");
    Check(view.footprint().requested_message_bytes == message.size(), test,
          "requested byte count differs");
    CheckAccounting(view.footprint(), test);
  }

  std::string canonical(storage::kMaximumBenchmarkStoredMessageBytes - 1U, 'a');
  canonical.append(acute);
  const auto described = storage::DescribeRecord<Policy>(Bytes(canonical));
  storage::RecordSlot<Policy> slot;
  auto writer = slot.Begin(storage::BenchmarkRecordSeed(), described);
  const RecordView view =
      storage::BuildBenchmarkRecord<Policy>(std::move(writer), Bytes(canonical));
  Check(SameFootprint(described, view.footprint()), test,
        "non-ASCII DescribeRecord and published footprint must match exactly");
  Check(view.message().size() == storage::kMaximumBenchmarkStoredMessageBytes - 1U, test,
        "canonical preflight must remove a partial code point");
}

template <typename Policy>
void TestNoWarmAllocation(std::string_view candidate) {
  const std::string test = std::string{candidate} + " no warm allocation";
  storage::RecordSlot<Policy> slot;
  std::array<std::byte, 128> payload{};
  std::fill(payload.begin(), payload.end(), std::byte{'x'});
  const auto footprint = storage::DescribeRecord<Policy>(std::span<const std::byte>{payload});
  const std::uint64_t before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  auto writer = slot.Begin(storage::BenchmarkRecordSeed(), footprint);
  const RecordView view =
      storage::BuildBenchmarkRecord<Policy>(std::move(writer), std::span<const std::byte>{payload});
  const std::uint64_t after = allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  Check(static_cast<bool>(view), test, "warm canonical build must publish");
  Check(after == before, test, "warm canonical build used general-purpose heap allocation");
}

template <typename Policy>
void TestMalformedFootprintRejected(std::string_view candidate) {
  const std::string test = std::string{candidate} + " malformed footprint";
  RecordFootprint malformed{
      .requested_message_bytes = 0,
      .stored_message_bytes = 0,
      .owned_payload_bytes = std::numeric_limits<std::uint64_t>::max(),
      .metadata_bytes = 1,
      .fragmentation_bytes = 0,
      .accounting_charge_bytes = Policy::kMinimumAccountingChargeBytes,
      .minimum_accounting_charge_bytes = Policy::kMinimumAccountingChargeBytes,
      .truncated = false};
  storage::RecordSlot<Policy> slot;
  Check(!static_cast<bool>(slot.Begin({}, malformed)), test,
        "overflowing footprint must be rejected before arithmetic wraps");
}

template <typename Policy>
void RunPolicyTests(std::string_view candidate, std::size_t boundary_prefix) {
  TestCanonicalRecipe<Policy>(candidate);
  TestOwnershipTypesAndSegmentBoundaries<Policy>(candidate);
  TestConsumerAfterProducerJoin<Policy>(candidate);
  TestNativeAndFmtEquivalence<Policy>(candidate);
  TestPublishedChargeShrinksToActualRecord<Policy>(candidate);
  TestUtf8Truncation<Policy>(candidate, boundary_prefix);
  TestNoWarmAllocation<Policy>(candidate);
  TestMalformedFootprintRejected<Policy>(candidate);
}

void TestPolicyFormulas() {
  constexpr std::string_view test = "policy formulas";
  Check(ContiguousPolicy::AccountingCharge(1) == 64, test, "contiguous minimum differs");
  Check(ContiguousPolicy::AccountingCharge(64) == 64, test, "contiguous exact block differs");
  Check(ContiguousPolicy::AccountingCharge(65) == 128, test, "contiguous rounding differs");
  Check(ChunkedPolicy::AccountingCharge(1) == 256, test, "chunked minimum differs");
  Check(ChunkedPolicy::AccountingCharge(256) == 256, test, "chunked exact block differs");
  Check(ChunkedPolicy::AccountingCharge(257) == 512, test, "chunked rounding differs");
  Check(HybridPolicy::AccountingCharge(1) == 512, test, "hybrid minimum differs");
  Check(HybridPolicy::AccountingCharge(512) == 512, test, "hybrid inline charge differs");
  Check(HybridPolicy::AccountingCharge(513) == 1'536, test, "hybrid first overflow differs");
  Check(HybridPolicy::AccountingCharge(1'536) == 1'536, test, "hybrid exact overflow differs");
  Check(HybridPolicy::AccountingCharge(1'537) == 2'560, test, "hybrid overflow rounding differs");
  Check(HybridPolicy::AccountingCharge(15'872) == 15'872, test,
        "hybrid last full overflow differs");
  Check(HybridPolicy::AccountingCharge(15'873) == 16'384, test, "hybrid final tail charge differs");
  Check(HybridPolicy::AccountingCharge(16'384) == 16'384, test, "hybrid maximum charge differs");

  const auto maximum = storage::DescribeRecord<HybridPolicy>(std::size_t{16'384});
  Check(maximum.truncated &&
            maximum.stored_message_bytes == storage::kMaximumBenchmarkStoredMessageBytes,
        test, "largest maintained request must be represented by truncation");
  CheckAccounting(maximum, test);
}

static_assert(!std::is_same_v<ContiguousPolicy::Storage, ChunkedPolicy::Storage>);
static_assert(!std::is_same_v<ChunkedPolicy::Storage, HybridPolicy::Storage>);
static_assert(sizeof(ContiguousPolicy::Storage) == storage::kMaximumSerializedBytes);
static_assert(sizeof(ChunkedPolicy::Storage) == storage::kMaximumSerializedBytes);
static_assert(sizeof(HybridPolicy::Storage) == storage::kMaximumSerializedBytes);

}  // namespace

int main() {
  TestPolicyFormulas();
  RunPolicyTests<ContiguousPolicy>("contiguous", 15);
  RunPolicyTests<ChunkedPolicy>("chunked", 207);
  RunPolicyTests<HybridPolicy>("hybrid", 463);
  return failure_count == 0 ? 0 : 1;
}
