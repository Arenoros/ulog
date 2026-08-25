#include "prototypes/ingress/chunked_mpsc.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

namespace ingress = ulog::benchmark_support::ingress;

using TestTopology = ingress::ChunkedMpsc<8, 4>;

static_assert(TestTopology::Capacity() == 8);
static_assert(TestTopology::StorageChunkSize() == 4);
static_assert(TestTopology::StorageChunkCount() == 2);

bool Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

ingress::RecordHandle MakeRecord(std::uint32_t slot_index) {
  const std::uint64_t serialized_bytes = 48U + slot_index * 7U;
  return {
      .slot_index = slot_index,
      .generation = 1000U + slot_index,
      .serialized_bytes = serialized_bytes,
      .accounting_charge_bytes = serialized_bytes + 16U,
  };
}

bool Matches(const ingress::ConsumedRecord& actual, const ingress::ConsumedRecord& expected) {
  return actual.admission_sequence == expected.admission_sequence &&
         actual.record == expected.record;
}

bool TestChunkLocalFullDoesNotBlockAnotherChunk() {
  TestTopology topology;
  bool success = true;

  for (std::uint32_t index = 0; index < TestTopology::StorageChunkSize(); ++index) {
    const auto published = topology.TryPublish(0, MakeRecord(index));
    success &= Check(published.status == ingress::PublishStatus::kAccepted,
                     "mapped chunk must accept up to its local capacity");
  }

  const auto local_full = topology.TryPublish(0, MakeRecord(4));
  success &=
      Check(local_full.status == ingress::PublishStatus::kFull && !local_full.admission_sequence,
            "a full producer chunk must reject without consuming a sequence");

  const auto other_chunk = topology.TryPublish(1, MakeRecord(5));
  return success && Check(other_chunk.status == ingress::PublishStatus::kAccepted &&
                              other_chunk.admission_sequence == TestTopology::StorageChunkSize(),
                          "a free producer chunk must accept after another chunk is full");
}

bool TestCrossChunkGlobalFifo() {
  constexpr std::array<std::size_t, 6> kProducers{1, 0, 3, 2, 0, 1};
  TestTopology topology;
  bool success = true;

  for (std::uint32_t index = 0; index < kProducers.size(); ++index) {
    const auto published = topology.TryPublish(kProducers[index], MakeRecord(index));
    success &= Check(published.status == ingress::PublishStatus::kAccepted &&
                         published.admission_sequence == index,
                     "cross-chunk publication must receive the next global sequence");
  }

  for (std::uint32_t index = 0; index < kProducers.size(); ++index) {
    const auto consumed = topology.TryConsume();
    success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record &&
                         consumed.record->admission_sequence == index &&
                         consumed.record->record == MakeRecord(index),
                     "physical chunk heads must merge in global FIFO order");
  }

  return success;
}

bool TestSameChunkPublishersDoNotBlockAnotherChunk() {
  constexpr std::size_t kWorkerCount = 16;
  constexpr std::size_t kAttemptsPerWorker = 20'000;
  TestTopology topology;
  std::array<std::thread, kWorkerCount> workers;
  std::atomic<std::size_t> ready_workers{0};
  std::atomic<bool> start{false};
  std::atomic<bool> results_valid{true};

  for (std::size_t worker_index = 0; worker_index < workers.size(); ++worker_index) {
    workers[worker_index] = std::thread([&, worker_index] {
      const std::size_t producer_index = worker_index * TestTopology::StorageChunkCount();
      const auto record = MakeRecord(static_cast<std::uint32_t>(2000U + worker_index));
      ready_workers.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (std::size_t attempt = 0; attempt < kAttemptsPerWorker; ++attempt) {
        const auto published = topology.TryPublish(producer_index, record);
        const bool expected_status = published.status == ingress::PublishStatus::kAccepted ||
                                     published.status == ingress::PublishStatus::kFull ||
                                     published.status == ingress::PublishStatus::kContended;
        if (!expected_status ||
            published.admission_sequence.has_value() !=
                (published.status == ingress::PublishStatus::kAccepted) ||
            published.publication_actions > topology.MaximumPublicationActions()) {
          results_valid.store(false, std::memory_order_relaxed);
        }
      }
    });
  }

  while (ready_workers.load(std::memory_order_acquire) != workers.size()) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);
  for (auto& worker : workers) {
    worker.join();
  }

  const std::uint64_t expected_attempts = kWorkerCount * kAttemptsPerWorker;
  const auto contended_snapshot = topology.GetSnapshot();
  bool success =
      Check(results_valid.load(std::memory_order_relaxed),
            "contended publication returned an invalid result") &&
      Check(contended_snapshot.attempted_records == expected_attempts &&
                contended_snapshot.enqueued_records == TestTopology::StorageChunkSize() &&
                contended_snapshot.rejected_records ==
                    expected_attempts - TestTopology::StorageChunkSize() &&
                contended_snapshot.full_rejections > 0 &&
                contended_snapshot.full_rejections + contended_snapshot.contention_rejections ==
                    contended_snapshot.rejected_records,
            "same-chunk contention/rejection accounting differs");

  const auto other_chunk = topology.TryPublish(1, MakeRecord(3000));
  success &= Check(other_chunk.status == ingress::PublishStatus::kAccepted &&
                       other_chunk.admission_sequence == TestTopology::StorageChunkSize(),
                   "same-chunk contention must not block a free independent chunk");
  return success;
}

bool TestChunkBoundaryWrapFullAndFifo() {
  TestTopology topology;
  bool success = true;
  std::uint64_t retained_serialized = 0;
  std::uint64_t retained_charge = 0;

  for (std::uint32_t index = 0; index < TestTopology::Capacity(); ++index) {
    const auto record = MakeRecord(index);
    const auto published = topology.TryPublish(index, record);
    success &= Check(published.status == ingress::PublishStatus::kAccepted,
                     "available chunked cell must accept the Record");
    success &= Check(published.admission_sequence == index,
                     "admission sequence must cross chunk boundaries monotonically");
    success &= Check(published.publication_actions <= topology.MaximumPublicationActions(),
                     "accepted publication exceeded its action bound");
    retained_serialized += record.serialized_bytes;
    retained_charge += record.accounting_charge_bytes;
  }

  const auto full = topology.TryPublish(0, MakeRecord(8));
  success &= Check(full.status == ingress::PublishStatus::kFull,
                   "full chunked storage must reject publication");
  success &= Check(!full.admission_sequence.has_value(),
                   "full rejection must not consume an admission sequence");
  success &= Check(full.publication_actions <= topology.MaximumPublicationActions(),
                   "full rejection exceeded its action bound");

  for (std::uint64_t expected_sequence = 0; expected_sequence < 3; ++expected_sequence) {
    const auto consumed = topology.TryConsume();
    success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record,
                     "published Record must be consumable before wrap");
    if (!consumed.record) {
      continue;
    }
    const auto expected = MakeRecord(static_cast<std::uint32_t>(expected_sequence));
    success &= Check(consumed.record->admission_sequence == expected_sequence,
                     "consumer order changed at a chunk boundary");
    success &=
        Check(consumed.record->record == expected, "consumer returned a different Record handle");
    retained_serialized -= expected.serialized_bytes;
    retained_charge -= expected.accounting_charge_bytes;
  }

  for (std::uint32_t index = 8; index < 11; ++index) {
    const auto record = MakeRecord(index);
    const auto published = topology.TryPublish(index, record);
    success &= Check(published.status == ingress::PublishStatus::kAccepted,
                     "released cells must accept wrapped publication");
    success &= Check(published.admission_sequence == index,
                     "wrapped publication must retain global sequence order");
    retained_serialized += record.serialized_bytes;
    retained_charge += record.accounting_charge_bytes;
  }

  const auto wrapped_snapshot = topology.GetSnapshot();
  success &=
      Check(wrapped_snapshot.attempted_records == 12 && wrapped_snapshot.enqueued_records == 11 &&
                wrapped_snapshot.dequeued_records == 3 && wrapped_snapshot.rejected_records == 1 &&
                wrapped_snapshot.full_rejections == 1,
            "chunked full/wrap Record accounting differs");
  success &= Check(wrapped_snapshot.retained_records == TestTopology::Capacity() &&
                       wrapped_snapshot.retained_serialized_bytes == retained_serialized &&
                       wrapped_snapshot.retained_charge_bytes == retained_charge,
                   "chunked full/wrap retained-byte accounting differs");

  for (std::uint64_t expected_sequence = 3; expected_sequence < 11; ++expected_sequence) {
    const auto consumed = topology.TryConsume();
    success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record,
                     "wrapped Record must remain consumable");
    if (!consumed.record) {
      continue;
    }
    success &= Check(consumed.record->admission_sequence == expected_sequence,
                     "wrapped consumer order must follow admission sequence");
    success &=
        Check(consumed.record->record == MakeRecord(static_cast<std::uint32_t>(expected_sequence)),
              "wrapped consumer returned a different Record handle");
  }

  const auto empty = topology.TryConsume();
  const auto drained_snapshot = topology.GetSnapshot();
  return success &&
         Check(empty.status == ingress::ConsumeStatus::kEmpty && !empty.record,
               "drained chunked storage must report empty") &&
         Check(drained_snapshot.dequeued_records == 11 && drained_snapshot.retained_records == 0 &&
                   drained_snapshot.retained_serialized_bytes == 0 &&
                   drained_snapshot.retained_charge_bytes == 0,
               "drained chunked accounting differs");
}

std::uint64_t NextRandom(std::uint64_t& state) {
  state += 0x9e3779b97f4a7c15ULL;
  std::uint64_t value = state;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

bool TestDeterministicModel() {
  constexpr std::size_t kSteps = 1024;
  struct ModelEntry final {
    ingress::ConsumedRecord consumed;
    std::size_t chunk_index{0};
  };

  TestTopology topology;
  std::array<ModelEntry, TestTopology::Capacity()> model{};
  std::array<std::size_t, TestTopology::StorageChunkCount()> chunk_occupancy{};
  std::size_t model_head = 0;
  std::size_t model_size = 0;
  std::uint64_t next_sequence = 0;
  std::uint64_t attempted = 0;
  std::uint64_t enqueued = 0;
  std::uint64_t dequeued = 0;
  std::uint64_t rejected = 0;
  std::uint64_t retained_serialized = 0;
  std::uint64_t retained_charge = 0;
  std::uint64_t random_state = 0x6a09e667f3bcc909ULL;
  bool success = true;

  for (std::size_t step = 0; step < kSteps; ++step) {
    const std::uint64_t random = NextRandom(random_state);
    const bool publish = (random & 3U) != 0U;
    if (publish) {
      const std::size_t producer_index = static_cast<std::size_t>(random % 32U);
      const std::size_t chunk_index = TestTopology::ChunkIndexForProducer(producer_index);
      const auto record = MakeRecord(static_cast<std::uint32_t>(step + 32U));
      const auto result = topology.TryPublish(producer_index, record);
      ++attempted;
      success &= Check(result.publication_actions <= topology.MaximumPublicationActions(),
                       "model publication exceeded its action bound");
      if (chunk_occupancy[chunk_index] == TestTopology::StorageChunkSize()) {
        ++rejected;
        success &=
            Check(result.status == ingress::PublishStatus::kFull && !result.admission_sequence,
                  "model chunk-full publication result differs");
      } else {
        success &= Check(result.status == ingress::PublishStatus::kAccepted &&
                             result.admission_sequence == next_sequence,
                         "model-accepted publication result differs");
        const std::size_t model_tail = (model_head + model_size) % model.size();
        model[model_tail] = {
            .consumed = {.record = record, .admission_sequence = next_sequence},
            .chunk_index = chunk_index,
        };
        ++chunk_occupancy[chunk_index];
        ++model_size;
        ++next_sequence;
        ++enqueued;
        retained_serialized += record.serialized_bytes;
        retained_charge += record.accounting_charge_bytes;
      }
    } else {
      const auto result = topology.TryConsume();
      if (model_size == 0) {
        success &= Check(result.status == ingress::ConsumeStatus::kEmpty && !result.record,
                         "model-empty consume result differs");
      } else {
        const auto expected = model[model_head];
        success &= Check(result.status == ingress::ConsumeStatus::kRecord && result.record &&
                             Matches(*result.record, expected.consumed),
                         "model FIFO consume result differs");
        --chunk_occupancy[expected.chunk_index];
        model_head = (model_head + 1U) % model.size();
        --model_size;
        ++dequeued;
        retained_serialized -= expected.consumed.record.serialized_bytes;
        retained_charge -= expected.consumed.record.accounting_charge_bytes;
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
              "model snapshot differs");
  }
  return success;
}

bool TestInvalidPublicationDoesNotConsumeSequence() {
  TestTopology topology;
  const auto invalid_producer = topology.TryPublish(
      ingress::kMaximumConcurrentPublishers,
      {.slot_index = 0, .generation = 1, .serialized_bytes = 64, .accounting_charge_bytes = 64});
  const auto invalid_record = topology.TryPublish(
      0, {.slot_index = 0, .generation = 1, .serialized_bytes = 0, .accounting_charge_bytes = 0});
  const auto accepted = topology.TryPublish(0, MakeRecord(0));
  const auto snapshot = topology.GetSnapshot();
  return Check(invalid_producer.status == ingress::PublishStatus::kInvalid &&
                   !invalid_producer.admission_sequence &&
                   invalid_record.status == ingress::PublishStatus::kInvalid &&
                   !invalid_record.admission_sequence,
               "invalid producer or Record must be rejected without a sequence") &&
         Check(accepted.status == ingress::PublishStatus::kAccepted &&
                   accepted.admission_sequence == 0,
               "invalid publication must not advance the global sequence") &&
         Check(snapshot.attempted_records == 3 && snapshot.enqueued_records == 1 &&
                   snapshot.rejected_records == 2 && snapshot.invalid_rejections == 2,
               "invalid publication accounting differs");
}

}  // namespace

int main() {
  const bool success = TestChunkLocalFullDoesNotBlockAnotherChunk() && TestCrossChunkGlobalFifo() &&
                       TestSameChunkPublishersDoNotBlockAnotherChunk() &&
                       TestChunkBoundaryWrapFullAndFifo() && TestDeterministicModel() &&
                       TestInvalidPublicationDoesNotConsumeSequence();
  return success ? 0 : 1;
}
