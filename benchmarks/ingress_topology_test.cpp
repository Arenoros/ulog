#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "prototypes/ingress/bounded_mpsc_ring.hpp"
#include "prototypes/ingress/chunked_mpsc.hpp"
#include "prototypes/ingress/ingress_kernel.hpp"
#include "prototypes/ingress/per_producer_lanes.hpp"
#include "prototypes/record_storage/record_storage.hpp"
#include "support/workload_harness.hpp"

namespace {

namespace ingress = ulog::benchmark_support::ingress;
namespace storage = ulog::benchmark_support::record_storage;

std::span<const std::byte> Bytes(std::string_view value) {
  return {reinterpret_cast<const std::byte*>(value.data()), value.size()};
}

bool Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

bool TestBoundedRingTransfersOwnedRecordsInFifoOrder() {
  constexpr std::size_t kCapacity = 8;
  constexpr std::uint64_t kGeneration = 1;
  ingress::BoundedMpscRing<kCapacity> topology;
  std::array<storage::ContiguousRecordSlot, kCapacity> slots;
  std::array<storage::RecordView, kCapacity> views;
  std::array<std::string, kCapacity> expected_messages;
  std::uint64_t expected_serialized_bytes = 0;
  std::uint64_t expected_charge_bytes = 0;
  bool success = true;

  for (std::size_t index = 0; index < kCapacity; ++index) {
    std::string caller_message = "record-" + std::to_string(index);
    expected_messages[index] = caller_message;
    const auto footprint =
        storage::DescribeRecord<storage::ContiguousPolicy>(Bytes(caller_message));
    auto writer = slots[index].Begin(storage::BenchmarkRecordSeed(), footprint);
    views[index] = storage::BuildBenchmarkRecord<storage::ContiguousPolicy>(std::move(writer),
                                                                            Bytes(caller_message));
    success &= Check(static_cast<bool>(views[index]), "fixture Record must publish");

    const auto published =
        topology.TryPublish(index, ingress::RecordHandle{
                                       .slot_index = static_cast<std::uint32_t>(index),
                                       .generation = kGeneration,
                                       .serialized_bytes = footprint.SerializedBytes(),
                                       .accounting_charge_bytes = footprint.accounting_charge_bytes,
                                   });
    success &= Check(published.status == ingress::PublishStatus::kAccepted,
                     "available ring slot must accept the Record");
    success &= Check(published.admission_sequence == index,
                     "accepted Record must receive the next admission sequence");
    success &= Check(published.publication_actions <= topology.MaximumPublicationActions(),
                     "producer publication must remain within the documented action bound");
    expected_serialized_bytes += footprint.SerializedBytes();
    expected_charge_bytes += footprint.accounting_charge_bytes;

    caller_message.assign(caller_message.size(), 'x');
  }

  const auto full = topology.TryPublish(0, ingress::RecordHandle{
                                               .slot_index = static_cast<std::uint32_t>(kCapacity),
                                               .generation = kGeneration,
                                               .serialized_bytes = 64,
                                               .accounting_charge_bytes = 64,
                                           });
  success &= Check(full.status == ingress::PublishStatus::kFull,
                   "publication beyond fixed capacity must be rejected");
  success &= Check(!full.admission_sequence.has_value(),
                   "rejected publication must not consume an admission sequence");
  success &= Check(full.publication_actions <= topology.MaximumPublicationActions(),
                   "full rejection must remain within the documented action bound");

  const auto before_drain = topology.GetSnapshot();
  success &= Check(before_drain.attempted_records == 9, "attempt accounting differs");
  success &= Check(before_drain.enqueued_records == 8, "enqueue accounting differs");
  success &= Check(before_drain.dequeued_records == 0, "premature dequeue was recorded");
  success &= Check(before_drain.rejected_records == 1, "reject accounting differs");
  success &= Check(before_drain.retained_records == 8, "retained Record count differs");
  success &= Check(before_drain.retained_serialized_bytes == expected_serialized_bytes,
                   "retained serialized-byte accounting differs");
  success &= Check(before_drain.retained_charge_bytes == expected_charge_bytes,
                   "retained charge-byte accounting differs");

  for (std::size_t expected_index = 0; expected_index < kCapacity; ++expected_index) {
    const auto consumed = topology.TryConsume();
    success &= Check(consumed.status == ingress::ConsumeStatus::kRecord,
                     "published Record must become consumable");
    success &= Check(consumed.record.has_value(), "Record consume result must carry an envelope");
    if (!consumed.record) {
      continue;
    }
    success &= Check(consumed.record->admission_sequence == expected_index,
                     "consumer order must follow admission sequence");
    success &= Check(consumed.record->record.slot_index == expected_index,
                     "consumer received a different Record handle");
    success &= Check(consumed.record->record.generation == kGeneration,
                     "consumer received a different Record generation");
    success &= Check(views[consumed.record->record.slot_index].message().Equals(
                         expected_messages[expected_index]),
                     "Record must retain caller-owned message bytes after publication");
  }

  const auto empty = topology.TryConsume();
  success &=
      Check(empty.status == ingress::ConsumeStatus::kEmpty, "drained ring must report empty");
  success &= Check(!empty.record.has_value(), "empty consume result must not carry a Record");

  const auto after_drain = topology.GetSnapshot();
  success &= Check(after_drain.enqueued_records == 8, "final enqueue accounting differs");
  success &= Check(after_drain.dequeued_records == 8, "final dequeue accounting differs");
  success &= Check(after_drain.rejected_records == 1, "final reject accounting differs");
  success &= Check(after_drain.retained_records == 0, "drained ring retained a Record");
  success &=
      Check(after_drain.retained_serialized_bytes == 0, "drained ring retained serialized bytes");
  success &= Check(after_drain.retained_charge_bytes == 0, "drained ring retained charge bytes");
  return success;
}

bool TestProducerIndexOutsideMatrixIsRejected() {
  ingress::BoundedMpscRing<8> topology;
  const auto rejected =
      topology.TryPublish(ingress::kMaximumConcurrentPublishers, ingress::RecordHandle{
                                                                     .slot_index = 0,
                                                                     .generation = 1,
                                                                     .serialized_bytes = 64,
                                                                     .accounting_charge_bytes = 64,
                                                                 });
  const auto snapshot = topology.GetSnapshot();
  return Check(rejected.status == ingress::PublishStatus::kInvalid,
               "producer index outside the maintained matrix must be rejected") &&
         Check(!rejected.admission_sequence.has_value(),
               "invalid producer must not receive an admission sequence") &&
         Check(snapshot.attempted_records == 1 && snapshot.rejected_records == 1 &&
                   snapshot.invalid_rejections == 1,
               "invalid producer accounting differs");
}

std::uint64_t NextRandom(std::uint64_t& state) noexcept {
  state += 0x9e3779b97f4a7c15ULL;
  std::uint64_t value = state;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

template <typename Topology>
bool TestTopologyMatchesRandomizedFifoModel(Topology& topology, bool partitioned_capacity) {
  constexpr std::size_t kCapacity = 8;
  constexpr std::size_t kProducerCount = 2;
  constexpr std::size_t kSteps = 2048;
  struct ModeledRecord final {
    ingress::ConsumedRecord envelope;
    std::size_t producer_index;
  };
  std::array<ModeledRecord, kCapacity> model{};
  std::array<std::size_t, kProducerCount> lane_sizes{};
  std::size_t model_head = 0;
  std::size_t model_size = 0;
  std::uint64_t next_sequence = 0;
  std::uint64_t attempted = 0;
  std::uint64_t enqueued = 0;
  std::uint64_t dequeued = 0;
  std::uint64_t rejected = 0;
  std::uint64_t retained_serialized = 0;
  std::uint64_t retained_charge = 0;
  std::uint64_t random_state = 0x243f6a8885a308d3ULL;
  bool success = true;

  for (std::size_t step = 0; step < kSteps; ++step) {
    const std::uint64_t random = NextRandom(random_state);
    if ((random & 3U) != 0U) {
      const std::size_t producer_index = static_cast<std::size_t>((random >> 8U) % kProducerCount);
      const std::uint64_t serialized_bytes = 1U + random % 512U;
      const ingress::RecordHandle record{
          .slot_index = static_cast<std::uint32_t>(step),
          .generation = step + 1U,
          .serialized_bytes = serialized_bytes,
          .accounting_charge_bytes = serialized_bytes + random % 64U,
      };
      const auto published = topology.TryPublish(producer_index, record);
      ++attempted;
      success &= Check(published.publication_actions <= topology.MaximumPublicationActions(),
                       "randomized publication exceeded its action bound");
      const bool expected_full = partitioned_capacity
                                     ? lane_sizes[producer_index] == kCapacity / kProducerCount
                                     : model_size == kCapacity;
      if (expected_full) {
        ++rejected;
        success &= Check(
            published.status == ingress::PublishStatus::kFull && !published.admission_sequence,
            "randomized full result differs from the model");
      } else {
        success &= Check(published.status == ingress::PublishStatus::kAccepted &&
                             published.admission_sequence == next_sequence,
                         "randomized admission differs from the model");
        model[(model_head + model_size) % kCapacity] = {
            .envelope = {.record = record, .admission_sequence = next_sequence},
            .producer_index = producer_index,
        };
        ++model_size;
        ++lane_sizes[producer_index];
        ++next_sequence;
        ++enqueued;
        retained_serialized += record.serialized_bytes;
        retained_charge += record.accounting_charge_bytes;
      }
    } else {
      const auto consumed = topology.TryConsume();
      if (model_size == 0) {
        success &= Check(consumed.status == ingress::ConsumeStatus::kEmpty && !consumed.record,
                         "randomized empty consume differs from the model");
      } else {
        const auto expected = model[model_head];
        success &=
            Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record &&
                      consumed.record->admission_sequence == expected.envelope.admission_sequence &&
                      consumed.record->record == expected.envelope.record,
                  "randomized FIFO consume differs from the model");
        model_head = (model_head + 1U) % kCapacity;
        --model_size;
        --lane_sizes[expected.producer_index];
        ++dequeued;
        retained_serialized -= expected.envelope.record.serialized_bytes;
        retained_charge -= expected.envelope.record.accounting_charge_bytes;
      }
    }

    const auto snapshot = topology.GetSnapshot();
    success &=
        Check(snapshot.attempted_records == attempted && snapshot.enqueued_records == enqueued &&
                  snapshot.dequeued_records == dequeued && snapshot.rejected_records == rejected &&
                  snapshot.full_rejections == rejected && snapshot.contention_rejections == 0 &&
                  snapshot.invalid_rejections == 0 && snapshot.retained_records == model_size &&
                  snapshot.retained_serialized_bytes == retained_serialized &&
                  snapshot.retained_charge_bytes == retained_charge,
              "randomized accounting differs from the model");
  }

  return success;
}

bool TestEveryTopologyMatchesTheSameRandomizedModel() {
  ingress::BoundedMpscRing<8> ring;
  ingress::ChunkedMpsc<8, 4> chunked;
  ingress::PerProducerLanes<8> lanes{2};
  return TestTopologyMatchesRandomizedFifoModel(ring, false) &&
         TestTopologyMatchesRandomizedFifoModel(chunked, true) &&
         TestTopologyMatchesRandomizedFifoModel(lanes, true);
}

bool TestChunkedTopologyReportsCapacityAsAnUpperBound() {
  constexpr std::size_t kRepetitions = 16;
  const ulog::benchmark_support::WorkloadCase workload{
      .producer_count = 16,
      .record_size_bytes = 64,
      .occupancy = ulog::benchmark_support::Occupancy::kEmpty,
      .capacity_bytes = ulog::benchmark_support::kPayloadCapacityBytes,
      .warmup_rounds = 1,
      .measured_rounds = 4,
      .repetition = 0,
  };
  bool success = true;
  for (std::size_t repetition = 0; repetition < kRepetitions; ++repetition) {
    ingress::ChunkedMpscIngressKernel kernel;
    const auto result = ulog::benchmark_support::RunWorkload(workload, kernel);
    const auto topology = kernel.MeasurementTopologySnapshot();
    success &= Check(result.accounting_error_count == 0 && result.retained_bound_error_count == 0,
                     "candidate admission below capacity must remain valid workload accounting");
    success &= Check(result.attempted_records == 64 &&
                         result.accepted_records + result.rejected_records == 64 &&
                         result.maximum_accepted_per_round <= workload.producer_count,
                     "candidate-limited workload admission counters differ");
    success &= Check(
        topology.attempted_records == 64 && topology.enqueued_records == result.accepted_records &&
            topology.dequeued_records == result.accepted_records &&
            topology.rejected_records == result.rejected_records && topology.retained_records == 0,
        "candidate-limited topology accounting differs");
  }
  return success;
}

template <typename Kernel>
bool TestTopologyRunsThroughCommonWorkload(std::string_view expected_name) {
  const ulog::benchmark_support::WorkloadCase workload{
      .producer_count = 4,
      .record_size_bytes = 64,
      .occupancy = ulog::benchmark_support::Occupancy::kEmpty,
      .capacity_bytes = ulog::benchmark_support::kPayloadCapacityBytes,
      .warmup_rounds = 1,
      .measured_rounds = 2,
      .repetition = 0,
  };
  Kernel kernel;
  const auto result = ulog::benchmark_support::RunWorkload(workload, kernel);
  const auto topology = kernel.MeasurementTopologySnapshot();

  return Check(Kernel::Name() == expected_name, "common workload topology name differs") &&
         Check(result.attempted_records == 8, "common workload attempt count differs") &&
         Check(result.accepted_records == 8 && result.rejected_records == 0,
               "common workload admission count differs") &&
         Check(
             result.logical_retained_final_bytes == 0 && result.physical_retained_final_bytes == 0,
             "common workload retained bytes did not drain") &&
         Check(topology.attempted_records == 8 && topology.enqueued_records == 8 &&
                   topology.dequeued_records == 8 && topology.rejected_records == 0,
               "measurement topology accounting differs") &&
         Check(topology.retained_records == 0 && topology.retained_serialized_bytes == 0 &&
                   topology.retained_charge_bytes == 0,
               "measurement topology retained state did not drain") &&
         Check(kernel.fifo_error_count() == 0 && kernel.sequence_error_count() == 0 &&
                   kernel.record_validation_error_count() == 0,
               "common workload Record/FIFO validation failed") &&
         Check(kernel.maximum_publication_actions_observed() <= kernel.publication_action_limit(),
               "common workload exceeded the publication action bound");
}

}  // namespace

int main() {
  const bool success =
      TestBoundedRingTransfersOwnedRecordsInFifoOrder() &&
      TestProducerIndexOutsideMatrixIsRejected() &&
      TestEveryTopologyMatchesTheSameRandomizedModel() &&
      TestChunkedTopologyReportsCapacityAsAnUpperBound() &&
      TestTopologyRunsThroughCommonWorkload<ingress::BoundedRingIngressKernel>(
          "bounded-mpsc-ring") &&
      TestTopologyRunsThroughCommonWorkload<ingress::ChunkedMpscIngressKernel>("chunked-mpsc") &&
      TestTopologyRunsThroughCommonWorkload<ingress::PerProducerLanesIngressKernel>(
          "per-producer-lanes");
  return success ? 0 : 1;
}
