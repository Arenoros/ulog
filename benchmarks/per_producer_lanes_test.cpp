#include "prototypes/ingress/per_producer_lanes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

namespace ingress = ulog::benchmark_support::ingress;

bool Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

ingress::RecordHandle Handle(std::uint32_t slot_index, std::uint64_t serialized_bytes = 10,
                             std::uint64_t accounting_charge_bytes = 16) {
  return {
      .slot_index = slot_index,
      .generation = 1,
      .serialized_bytes = serialized_bytes,
      .accounting_charge_bytes = accounting_charge_bytes,
  };
}

template <typename Topology>
bool CheckPublication(const Topology& topology, const ingress::PublishResult& result,
                      ingress::PublishStatus expected_status, std::string_view failure_message) {
  return Check(result.status == expected_status, failure_message) &&
         Check(result.publication_actions <= topology.MaximumPublicationActions(),
               "publication exceeded the documented action bound");
}

bool TestPartitionedCapacityRejectsWithoutConsumingASequence() {
  ingress::PerProducerLanes<8> topology{2};
  bool success = true;

  for (std::uint32_t slot = 0; slot < 4; ++slot) {
    const auto published = topology.TryPublish(0, Handle(slot));
    success &= CheckPublication(topology, published, ingress::PublishStatus::kAccepted,
                                "an available producer lane must accept the Record");
    success &= Check(published.admission_sequence == slot,
                     "accepted Records must receive consecutive admission sequences");
  }

  const auto full = topology.TryPublish(0, Handle(4));
  success &= CheckPublication(topology, full, ingress::PublishStatus::kFull,
                              "a full producer lane must reject even if another lane is empty");
  success &= Check(!full.admission_sequence.has_value(),
                   "a lane-full rejection must not consume an admission sequence");

  const auto other_lane = topology.TryPublish(1, Handle(100, 20, 32));
  success &= CheckPublication(topology, other_lane, ingress::PublishStatus::kAccepted,
                              "an independent producer lane must retain its own capacity");
  success &= Check(other_lane.admission_sequence == 4,
                   "the first admission after a rejection must use the next sequence");

  const auto before_drain = topology.GetSnapshot();
  success &= Check(before_drain.attempted_records == 6, "attempt accounting differs");
  success &= Check(before_drain.enqueued_records == 5, "enqueue accounting differs");
  success &= Check(before_drain.rejected_records == 1 && before_drain.full_rejections == 1,
                   "lane-full rejection accounting differs");
  success &= Check(before_drain.retained_records == 5, "retained Record count differs");
  success &= Check(before_drain.retained_serialized_bytes == 60,
                   "retained serialized-byte accounting differs");
  success &=
      Check(before_drain.retained_charge_bytes == 96, "retained charge-byte accounting differs");

  constexpr std::array<std::uint32_t, 5> kExpectedSlots{0, 1, 2, 3, 100};
  for (std::size_t expected_sequence = 0; expected_sequence < kExpectedSlots.size();
       ++expected_sequence) {
    const auto consumed = topology.TryConsume();
    success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record,
                     "each accepted Record must be consumable");
    if (consumed.record) {
      success &= Check(consumed.record->admission_sequence == expected_sequence,
                       "consumer order must follow the global admission sequence");
      success &= Check(consumed.record->record.slot_index == kExpectedSlots[expected_sequence],
                       "consumer returned Records in a different caller order");
    }
  }

  const auto empty = topology.TryConsume();
  success &= Check(empty.status == ingress::ConsumeStatus::kEmpty && !empty.record,
                   "a drained topology must report empty");
  const auto after_drain = topology.GetSnapshot();
  success &= Check(after_drain.dequeued_records == 5, "dequeue accounting differs");
  success &=
      Check(after_drain.retained_records == 0 && after_drain.retained_serialized_bytes == 0 &&
                after_drain.retained_charge_bytes == 0,
            "a drained topology must retain no Records or bytes");
  return success;
}

bool TestConsumerMergesLanesInCallerOrder() {
  ingress::PerProducerLanes<8> topology{2};
  constexpr std::array<std::size_t, 4> kProducers{1, 0, 1, 0};
  constexpr std::array<std::uint32_t, 4> kSlots{11, 20, 12, 21};
  bool success = true;

  for (std::size_t index = 0; index < kSlots.size(); ++index) {
    const auto published = topology.TryPublish(kProducers[index], Handle(kSlots[index]));
    success &= CheckPublication(topology, published, ingress::PublishStatus::kAccepted,
                                "an available interleaved lane publication must succeed");
    success &= Check(published.admission_sequence == index,
                     "interleaved lanes must share one admission sequence");
  }

  for (std::size_t index = 0; index < kSlots.size(); ++index) {
    const auto consumed = topology.TryConsume();
    success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record,
                     "an interleaved publication must be consumable");
    if (consumed.record) {
      success &= Check(consumed.record->record.slot_index == kSlots[index],
                       "lane scan reordered caller admissions");
      success &= Check(consumed.record->admission_sequence == index,
                       "lane scan returned a non-next admission sequence");
    }
  }
  return success;
}

bool TestLaneStorageWrapsAfterDrain() {
  ingress::PerProducerLanes<6> topology{2};
  bool success = true;

  for (std::uint32_t round = 0; round < 2; ++round) {
    for (std::uint32_t offset = 0; offset < 3; ++offset) {
      const auto published = topology.TryPublish(0, Handle(round * 10 + offset));
      success &= CheckPublication(topology, published, ingress::PublishStatus::kAccepted,
                                  "a drained lane cell must be reusable after wrap");
    }
    for (std::uint32_t offset = 0; offset < 3; ++offset) {
      const auto consumed = topology.TryConsume();
      success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record,
                       "wrapped lane Record must be consumable");
      if (consumed.record) {
        success &= Check(consumed.record->record.slot_index == round * 10 + offset,
                         "wrapped lane changed FIFO order");
      }
    }
  }

  const auto snapshot = topology.GetSnapshot();
  success &= Check(snapshot.attempted_records == 6 && snapshot.enqueued_records == 6 &&
                       snapshot.dequeued_records == 6 && snapshot.rejected_records == 0,
                   "wrap-and-drain accounting differs");
  success &= Check(snapshot.retained_records == 0, "wrapped topology retained Records");
  return success;
}

bool TestInvalidPublicationsAreBoundedAndDoNotConsumeSequences() {
  ingress::PerProducerLanes<8> topology{2};
  bool success = true;

  const auto invalid_producer = topology.TryPublish(2, Handle(1));
  success &= CheckPublication(topology, invalid_producer, ingress::PublishStatus::kInvalid,
                              "producer outside active lanes must be rejected");
  success &= Check(!invalid_producer.admission_sequence,
                   "invalid producer must not receive an admission sequence");

  const auto invalid_footprint = topology.TryPublish(0, Handle(2, 17, 16));
  success &= CheckPublication(topology, invalid_footprint, ingress::PublishStatus::kInvalid,
                              "invalid Record footprint must be rejected");
  success &= Check(!invalid_footprint.admission_sequence,
                   "invalid Record must not receive an admission sequence");

  const auto accepted = topology.TryPublish(0, Handle(3));
  success &= CheckPublication(topology, accepted, ingress::PublishStatus::kAccepted,
                              "a valid Record must remain publishable after invalid attempts");
  success &= Check(accepted.admission_sequence == 0,
                   "invalid attempts must leave the admission sequence unchanged");

  const auto snapshot = topology.GetSnapshot();
  success &= Check(snapshot.attempted_records == 3 && snapshot.enqueued_records == 1 &&
                       snapshot.rejected_records == 2 && snapshot.invalid_rejections == 2,
                   "invalid publication accounting differs");
  return success;
}

}  // namespace

int main() {
  const bool success = TestPartitionedCapacityRejectsWithoutConsumingASequence() &&
                       TestConsumerMergesLanesInCallerOrder() && TestLaneStorageWrapsAfterDrain() &&
                       TestInvalidPublicationsAreBoundedAndDoNotConsumeSequences();
  return success ? 0 : 1;
}
