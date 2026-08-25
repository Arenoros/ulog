#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <new>
#include <string_view>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

#include "prototypes/composed/composed_producer_kernel.hpp"

namespace allocation_tracking {

std::atomic<std::uint64_t> allocation_count{0};
std::atomic<std::uint64_t> allocation_bytes{0};

void* Allocate(std::size_t size) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  allocation_bytes.fetch_add(size, std::memory_order_relaxed);
  if (void* memory = std::malloc(std::max(size, std::size_t{1}))) {
    return memory;
  }
  throw std::bad_alloc{};
}

void* AllocateAligned(std::size_t size, std::size_t alignment) {
  allocation_count.fetch_add(1, std::memory_order_relaxed);
  allocation_bytes.fetch_add(size, std::memory_order_relaxed);
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

namespace composed = ulog::benchmark_support::composed;
namespace ingress = ulog::benchmark_support::ingress;
namespace storage = ulog::benchmark_support::record_storage;

using Path = composed::ComposedProducerPath<2>;
using Writer = composed::Writer;

constexpr std::string_view kMessage = "allocation-free";
constexpr composed::RecordPlan kAcceptedPlan = composed::RecordPlan::Benchmark(kMessage.size());
constexpr composed::RecordPlan kBudgetRejectedPlan =
    composed::RecordPlan::Benchmark(storage::kMaximumBenchmarkStoredMessageBytes);
constexpr std::size_t kCapacityBytes =
    2U * static_cast<std::size_t>(kAcceptedPlan.maximum_footprint().accounting_charge_bytes);
constexpr std::uint64_t kMeasuredCycles = 1'024;
constexpr std::uint64_t kLaneFixtureRecords = 2;
constexpr std::uint64_t kExpectedAcceptedRecords = kMeasuredCycles + kLaneFixtureRecords;

static_assert(kBudgetRejectedPlan.maximum_footprint().accounting_charge_bytes > kCapacityBytes);

int failure_count = 0;

void Check(bool condition, std::string_view message) {
  if (!condition) {
    ++failure_count;
    std::cerr << "composed allocation test: " << message << '\n';
  }
}

[[nodiscard]] composed::ProduceResult Produce(Path& path, const composed::RecordPlan& plan,
                                              std::uint64_t& message_callbacks,
                                              std::uint64_t& context_callbacks,
                                              bool& writer_valid) noexcept {
  return path.TryProduce(
      0, plan,
      [&](Writer& writer) noexcept {
        ++message_callbacks;
        const auto written = writer.Append(kMessage);
        const bool stored = written.stored_bytes == kMessage.size() && !written.truncated;
        writer_valid = writer_valid && stored;
        return stored;
      },
      [&](Writer& writer) noexcept {
        ++context_callbacks;
        const bool stored = composed::AddBenchmarkContext(writer);
        writer_valid = writer_valid && stored;
        return stored;
      });
}

[[nodiscard]] ingress::ConsumeStatus Consume(Path& path, std::uint64_t& consumer_callbacks,
                                             bool& record_valid) noexcept {
  return path.TryConsume([&](std::uint64_t, const storage::RecordView& record) noexcept {
    ++consumer_callbacks;
    record_valid = record_valid && record.message().Equals(kMessage) &&
                   record.field_count() == storage::kBenchmarkFieldCount;
  });
}

void TestWarmPathDoesNotAllocate() {
  const std::uint64_t constructor_allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const std::uint64_t constructor_bytes_before =
      allocation_tracking::allocation_bytes.load(std::memory_order_relaxed);
  Path path{kCapacityBytes, 0, 1};
  const std::uint64_t constructor_allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const std::uint64_t constructor_bytes_after =
      allocation_tracking::allocation_bytes.load(std::memory_order_relaxed);
  Check(constructor_allocations_after - constructor_allocations_before == 1U,
        "path construction did not allocate exactly one Record backing block");
  Check(constructor_bytes_after - constructor_bytes_before == Path::RecordBackingStorageBytes(),
        "path construction allocated an unexpected Record backing size");

  std::uint64_t warm_message_callbacks = 0;
  std::uint64_t warm_context_callbacks = 0;
  std::uint64_t warm_consumer_callbacks = 0;
  bool warm_writer_valid = true;
  bool warm_record_valid = true;
  const auto warm_produced = Produce(path, kAcceptedPlan, warm_message_callbacks,
                                     warm_context_callbacks, warm_writer_valid);
  const auto warm_consumed = Consume(path, warm_consumer_callbacks, warm_record_valid);
  path.ReturnAllCredits();

  const bool warm_success = warm_produced.status == composed::ProduceStatus::kAccepted &&
                            warm_consumed == ingress::ConsumeStatus::kRecord &&
                            warm_message_callbacks == 1U && warm_context_callbacks == 1U &&
                            warm_consumer_callbacks == 1U && warm_writer_valid && warm_record_valid;
  Check(warm_success, "warm produce/consume cycle failed");
  if (!warm_success) {
    return;
  }

  path.BeginMeasurement();
  static_cast<void>(std::chrono::steady_clock::now());

  std::uint64_t accepted_message_callbacks = 0;
  std::uint64_t accepted_context_callbacks = 0;
  std::uint64_t consumer_callbacks = 0;
  std::uint64_t budget_message_callbacks = 0;
  std::uint64_t budget_context_callbacks = 0;
  std::uint64_t lane_message_callbacks = 0;
  std::uint64_t lane_context_callbacks = 0;
  bool accepted_statuses_valid = true;
  bool consumed_statuses_valid = true;
  bool writer_valid = true;
  bool record_valid = true;
  bool rejected_writer_valid = true;

  const auto started = std::chrono::steady_clock::now();
  const std::uint64_t allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);

  for (std::uint64_t cycle = 0; cycle < kMeasuredCycles; ++cycle) {
    const auto produced = Produce(path, kAcceptedPlan, accepted_message_callbacks,
                                  accepted_context_callbacks, writer_valid);
    accepted_statuses_valid = accepted_statuses_valid &&
                              produced.status == composed::ProduceStatus::kAccepted &&
                              produced.admission_sequence.has_value();
    const auto consumed = Consume(path, consumer_callbacks, record_valid);
    consumed_statuses_valid =
        consumed_statuses_valid && consumed == ingress::ConsumeStatus::kRecord;
  }

  const auto budget_rejected = Produce(path, kBudgetRejectedPlan, budget_message_callbacks,
                                       budget_context_callbacks, rejected_writer_valid);

  for (std::uint64_t record = 0; record < kLaneFixtureRecords; ++record) {
    const auto produced = Produce(path, kAcceptedPlan, accepted_message_callbacks,
                                  accepted_context_callbacks, writer_valid);
    accepted_statuses_valid = accepted_statuses_valid &&
                              produced.status == composed::ProduceStatus::kAccepted &&
                              produced.admission_sequence.has_value();
  }
  const auto lane_rejected = Produce(path, kAcceptedPlan, lane_message_callbacks,
                                     lane_context_callbacks, rejected_writer_valid);
  for (std::uint64_t record = 0; record < kLaneFixtureRecords; ++record) {
    const auto consumed = Consume(path, consumer_callbacks, record_valid);
    consumed_statuses_valid =
        consumed_statuses_valid && consumed == ingress::ConsumeStatus::kRecord;
  }

  path.ReturnAllCredits();
  const auto snapshot = path.Snapshot();
  const std::uint64_t allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  Check(allocations_after == allocations_before,
        "warm accepted and rejected paths used general-purpose heap allocation");
  Check(elapsed < std::chrono::seconds{10}, "measured operations exceeded ten seconds");
  Check(accepted_statuses_valid && consumed_statuses_valid,
        "an accepted produce/consume cycle returned an unexpected status");
  Check(writer_valid && record_valid, "an accepted Record differed from its fixed plan");
  Check(budget_rejected.status == composed::ProduceStatus::kBudgetRejected,
        "oversized plan did not return a budget rejection");
  Check(lane_rejected.status == composed::ProduceStatus::kIngressRejected,
        "full lane did not return an ingress rejection");
  Check(!budget_rejected.admission_sequence && !lane_rejected.admission_sequence,
        "a rejected attempt consumed an admission sequence");
  Check(budget_message_callbacks == 0U && budget_context_callbacks == 0U,
        "budget rejection evaluated caller callbacks");
  Check(lane_message_callbacks == 0U && lane_context_callbacks == 0U,
        "lane-full rejection evaluated caller callbacks");
  Check(rejected_writer_valid, "a rejected attempt unexpectedly invoked its writer callback");
  Check(accepted_message_callbacks == kExpectedAcceptedRecords &&
            accepted_context_callbacks == kExpectedAcceptedRecords &&
            consumer_callbacks == kExpectedAcceptedRecords,
        "accepted callback counts differ from the measured cycle count");

  Check(snapshot.attempted_records == kExpectedAcceptedRecords + 2U &&
            snapshot.accepted_records == kExpectedAcceptedRecords &&
            snapshot.rejected_records == 2U,
        "producer admission accounting does not conserve attempts");
  Check(snapshot.message_callback_count == kExpectedAcceptedRecords &&
            snapshot.context_callback_count == kExpectedAcceptedRecords &&
            snapshot.published_records == kExpectedAcceptedRecords &&
            snapshot.consumed_records == kExpectedAcceptedRecords,
        "published, consumed, or callback accounting differs");
  Check(snapshot.logical_retained_bytes == 0U && snapshot.physical_retained_bytes == 0U &&
            snapshot.topology.retained_records == 0U &&
            snapshot.topology.retained_serialized_bytes == 0U &&
            snapshot.topology.retained_charge_bytes == 0U,
        "quiescent cleanup retained Record ownership or byte budget");
  Check(snapshot.fifo_error_count == 0U && snapshot.record_validation_error_count == 0U &&
            snapshot.publication_error_count == 0U,
        "FIFO, Record, publication, or accounting validation failed");
  Check(snapshot.topology.attempted_records == kExpectedAcceptedRecords + 1U &&
            snapshot.topology.enqueued_records == kExpectedAcceptedRecords &&
            snapshot.topology.dequeued_records == kExpectedAcceptedRecords &&
            snapshot.topology.rejected_records == 1U && snapshot.topology.full_rejections == 1U &&
            snapshot.topology.contention_rejections == 0U &&
            snapshot.topology.invalid_rejections == 0U,
        "ingress topology accounting differs");
}

}  // namespace

int main() {
  try {
    TestWarmPathDoesNotAllocate();
    return failure_count == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "composed allocation test failed with exception: " << error.what() << '\n';
    return 1;
  }
}
