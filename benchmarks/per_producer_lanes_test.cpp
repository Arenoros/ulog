#include "prototypes/ingress/per_producer_lanes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <utility>

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

bool TestAbandonedPublicationClaimIsInvisibleToTopologyAccounting() {
  ingress::PerProducerLanes<1> topology{1};
  bool success = true;

  {
    auto abandoned = topology.TryClaimPublication(0);
    success &= Check(static_cast<bool>(abandoned),
                     "an available lane cell must produce an abandonable claim");
    success &=
        Check(abandoned.cell_index() == 0, "an abandonable claim must identify its held cell");
    success &= Check(abandoned.publication_actions() <= topology.MaximumPublicationActions(),
                     "claiming publication exceeded the documented action bound");
  }

  const auto after_abandon = topology.GetSnapshot();
  success &= Check(after_abandon.attempted_records == 0 && after_abandon.enqueued_records == 0 &&
                       after_abandon.rejected_records == 0 && after_abandon.retained_records == 0,
                   "abandoning a pre-publication claim must not change topology accounting");

  auto replacement_claim = topology.TryClaimPublication(0);
  success &=
      Check(static_cast<bool>(replacement_claim), "abandoning a claim must release its lane cell");
  const auto replacement = topology.Publish(std::move(replacement_claim), Handle(9));
  success &= CheckPublication(topology, replacement, ingress::PublishStatus::kAccepted,
                              "a released lane claim must be publishable again");
  success &= Check(replacement.admission_sequence == 0,
                   "abandoning a publication claim must not consume a sequence");
  return success;
}

bool TestLaneFullPublicationClaimDoesNotConsumeASequence() {
  ingress::PerProducerLanes<2> topology{1};
  bool success = true;

  auto first_claim = topology.TryClaimPublication(0);
  success &= Check(static_cast<bool>(first_claim),
                   "an available lane cell must produce a publication claim");
  success &= Check(first_claim.cell_index() == 0,
                   "the first publication claim must identify its reserved cell");
  const auto first = topology.Publish(std::move(first_claim), Handle(10));
  success &= CheckPublication(topology, first, ingress::PublishStatus::kAccepted,
                              "the first claimed publication must succeed");
  success &= Check(first.admission_sequence == 0,
                   "the first claimed publication must receive sequence zero");

  auto second_claim = topology.TryClaimPublication(0);
  success &= Check(static_cast<bool>(second_claim),
                   "the second available lane cell must produce a publication claim");
  success &= Check(second_claim.cell_index() == 1,
                   "the second publication claim must identify its reserved cell");
  const auto second = topology.Publish(std::move(second_claim), Handle(11));
  success &= CheckPublication(topology, second, ingress::PublishStatus::kAccepted,
                              "the second claimed publication must succeed");
  success &= Check(second.admission_sequence == 1,
                   "claimed publications must receive consecutive sequences");

  const auto full_claim = topology.TryClaimPublication(0);
  success &= Check(!full_claim, "a full lane must not produce a publication claim");
  success &= Check(full_claim.status() == ingress::PublishStatus::kFull,
                   "a failed lane claim must report full");
  success &=
      Check(!full_claim.cell_index(), "a failed lane claim must not identify a reusable cell");
  success &= Check(full_claim.publication_actions() <= topology.MaximumPublicationActions(),
                   "a failed lane claim exceeded the documented action bound");
  const auto after_full_claim = topology.GetSnapshot();
  success &=
      Check(after_full_claim.attempted_records == 3 && after_full_claim.enqueued_records == 2 &&
                after_full_claim.rejected_records == 1 && after_full_claim.full_rejections == 1,
            "a failed lane claim must contribute one exact rejected attempt");

  const auto consumed = topology.TryConsume();
  success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record,
                   "the first claimed publication must be consumable");

  auto wrapped_claim = topology.TryClaimPublication(0);
  success &= Check(static_cast<bool>(wrapped_claim),
                   "acknowledged lane capacity must become claimable again");
  success &= Check(wrapped_claim.cell_index() == 0,
                   "the wrapped publication claim must identify the released cell");
  const auto wrapped = topology.Publish(std::move(wrapped_claim), Handle(12));
  success &= CheckPublication(topology, wrapped, ingress::PublishStatus::kAccepted,
                              "publication through a wrapped claim must succeed");
  success &= Check(wrapped.admission_sequence == 2,
                   "a lane-full claim must not consume an admission sequence");
  return success;
}

bool TestHeldConsumptionClaimPreventsCellReuseUntilAcknowledged() {
  ingress::PerProducerLanes<1> topology{1};
  bool success = true;

  const auto published = topology.TryPublish(0, Handle(20));
  success &= CheckPublication(topology, published, ingress::PublishStatus::kAccepted,
                              "the single lane cell must accept its first Record");

  auto consumption = topology.TryClaimConsumption();
  success &= Check(consumption.status() == ingress::ConsumeStatus::kRecord && consumption.record(),
                   "a ready Record must produce a held consumption claim");
  if (consumption.record()) {
    success &= Check(consumption.record()->record.slot_index == 20,
                     "the consumption claim must expose the claimed Record");
  }

  const auto while_held = topology.TryClaimPublication(0);
  success &= Check(!while_held && while_held.status() == ingress::PublishStatus::kFull,
                   "a held consumption claim must keep its lane cell unavailable");
  const auto held_snapshot = topology.GetSnapshot();
  success &= Check(held_snapshot.dequeued_records == 0 && held_snapshot.retained_records == 1,
                   "claiming consumption must not dequeue before acknowledgement");

  consumption.Acknowledge();
  const auto acknowledged_snapshot = topology.GetSnapshot();
  success &= Check(
      acknowledged_snapshot.dequeued_records == 1 && acknowledged_snapshot.retained_records == 0,
      "acknowledgement must dequeue and release the held lane cell");

  auto reusable = topology.TryClaimPublication(0);
  success &= Check(static_cast<bool>(reusable),
                   "an acknowledged consumption claim must make its cell reusable");
  const auto republished = topology.Publish(std::move(reusable), Handle(21));
  success &= CheckPublication(topology, republished, ingress::PublishStatus::kAccepted,
                              "the acknowledged cell must accept a later publication");
  success &= Check(republished.admission_sequence == 1,
                   "a publication rejected while consumption was held must not consume a sequence");
  return success;
}

}  // namespace

int main() {
  const bool success = TestPartitionedCapacityRejectsWithoutConsumingASequence() &&
                       TestConsumerMergesLanesInCallerOrder() && TestLaneStorageWrapsAfterDrain() &&
                       TestInvalidPublicationsAreBoundedAndDoNotConsumeSequences() &&
                       TestAbandonedPublicationClaimIsInvisibleToTopologyAccounting() &&
                       TestLaneFullPublicationClaimDoesNotConsumeASequence() &&
                       TestHeldConsumptionClaimPreventsCellReuseUntilAcknowledged();
  return success ? 0 : 1;
}
