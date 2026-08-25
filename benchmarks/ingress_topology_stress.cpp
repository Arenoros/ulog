#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string_view>
#include <thread>

#include "prototypes/ingress/bounded_mpsc_ring.hpp"
#include "prototypes/ingress/chunked_mpsc.hpp"
#include "prototypes/ingress/per_producer_lanes.hpp"

namespace {

namespace ingress = ulog::benchmark_support::ingress;

constexpr std::size_t kProducerCount = ingress::kMaximumConcurrentPublishers;
constexpr std::size_t kTopologyCapacity = 64;
constexpr std::size_t kInitialAttemptsPerProducer = 8;
constexpr std::size_t kAttemptsPerProducer = 4096;
constexpr std::size_t kAttemptCount = kProducerCount * kAttemptsPerProducer;
constexpr std::uint64_t kMinimumSerializedBytes = 64;
constexpr std::uint64_t kSerializedByteVariants = 193;
constexpr std::uint64_t kChargeByteVariants = 31;
constexpr auto kStressDeadline = std::chrono::seconds{5};
constexpr auto kNearFullDeadline = std::chrono::seconds{2};
constexpr std::ptrdiff_t kStartParticipants = static_cast<std::ptrdiff_t>(kProducerCount + 2U);
constexpr std::ptrdiff_t kPhaseParticipants = static_cast<std::ptrdiff_t>(kProducerCount + 1U);

struct StressBookkeeping final {
  std::array<std::atomic<std::uint8_t>, kAttemptCount> accepted{};
  std::array<std::atomic<std::uint8_t>, kAttemptCount> consumed{};
  std::array<std::atomic<std::uint64_t>, kAttemptCount> accepted_sequences{};
  std::array<std::atomic<std::uint64_t>, kAttemptCount> consumed_sequences{};
};

[[nodiscard]] constexpr ingress::RecordHandle ExpectedRecord(std::size_t attempt_id) noexcept {
  const std::uint64_t serialized_bytes =
      kMinimumSerializedBytes + attempt_id % kSerializedByteVariants;
  return {
      .slot_index = static_cast<std::uint32_t>(attempt_id),
      .generation = static_cast<std::uint64_t>(attempt_id) + 1U,
      .serialized_bytes = serialized_bytes,
      .accounting_charge_bytes = serialized_bytes + attempt_id % kChargeByteVariants,
  };
}

bool Check(bool condition, std::string_view topology_name, std::string_view message) {
  if (!condition) {
    std::cerr << topology_name << ": " << message << '\n';
  }
  return condition;
}

template <typename Topology, typename... ConstructorArgs>
bool RunNearFullStress(ConstructorArgs... constructor_args) {
  Topology topology{constructor_args...};
  const std::string_view topology_name = Topology::Name();
  constexpr std::size_t kPreloadedRecords = kTopologyCapacity - 1U;
  constexpr std::size_t kConcurrentAttemptBase = kPreloadedRecords;
  std::uint64_t retained_serialized = 0;
  std::uint64_t retained_charge = 0;
  std::size_t attempt_id = 0;
  bool success = true;

  const auto preload = [&](std::size_t producer_index) {
    const ingress::RecordHandle record = ExpectedRecord(attempt_id);
    const ingress::PublishResult result = topology.TryPublish(producer_index, record);
    success &= Check(result.status == ingress::PublishStatus::kAccepted &&
                         result.admission_sequence == attempt_id,
                     topology_name, "near-full preload did not preserve admission order");
    success &= Check(result.publication_actions <= topology.MaximumPublicationActions(),
                     topology_name, "near-full preload exceeded its publication action bound");
    retained_serialized += record.serialized_bytes;
    retained_charge += record.accounting_charge_bytes;
    ++attempt_id;
  };

  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    preload(producer_index);
  }
  for (std::size_t producer_index = 1; producer_index < kProducerCount; ++producer_index) {
    preload(producer_index);
  }

  const ingress::TopologySnapshot near_full = topology.GetSnapshot();
  success &=
      Check(attempt_id == kPreloadedRecords && near_full.attempted_records == kPreloadedRecords &&
                near_full.enqueued_records == kPreloadedRecords &&
                near_full.retained_records == kPreloadedRecords &&
                near_full.retained_serialized_bytes == retained_serialized &&
                near_full.retained_charge_bytes == retained_charge,
            topology_name, "the explicit near-full preload accounting differs");

  std::array<std::thread, kProducerCount> producers;
  std::array<ingress::PublishResult, kProducerCount> results;
  std::barrier start{kPhaseParticipants};
  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    producers[producer_index] = std::thread{[&, producer_index] {
      start.arrive_and_wait();
      results[producer_index] = topology.TryPublish(
          producer_index, ExpectedRecord(kConcurrentAttemptBase + producer_index));
    }};
  }
  const auto concurrent_start = std::chrono::steady_clock::now();
  start.arrive_and_wait();
  for (auto& producer : producers) {
    producer.join();
  }
  const auto concurrent_elapsed = std::chrono::steady_clock::now() - concurrent_start;

  std::size_t accepted_attempt_id = kAttemptCount;
  std::uint64_t accepted_count = 0;
  std::uint64_t rejected_count = 0;
  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    const ingress::PublishResult& result = results[producer_index];
    success &= Check(result.publication_actions <= topology.MaximumPublicationActions(),
                     topology_name, "a near-full concurrent publication exceeded its action bound");
    if (result.status == ingress::PublishStatus::kAccepted) {
      ++accepted_count;
      accepted_attempt_id = kConcurrentAttemptBase + producer_index;
      success &= Check(result.admission_sequence == kPreloadedRecords, topology_name,
                       "the remaining near-full slot received a different sequence");
      retained_serialized += ExpectedRecord(accepted_attempt_id).serialized_bytes;
      retained_charge += ExpectedRecord(accepted_attempt_id).accounting_charge_bytes;
    } else {
      ++rejected_count;
      success &=
          Check(!result.admission_sequence && (result.status == ingress::PublishStatus::kFull ||
                                               result.status == ingress::PublishStatus::kContended),
                topology_name, "near-full excess publication returned an inconsistent rejection");
    }
  }

  const ingress::TopologySnapshot full = topology.GetSnapshot();
  success &= Check(concurrent_elapsed <= kNearFullDeadline, topology_name,
                   "near-full concurrent publication exceeded its two-second deadline");
  success &= Check(accepted_count == 1 && rejected_count == kProducerCount - 1U, topology_name,
                   "near-full concurrency did not admit exactly the remaining slot");
  success &= Check(full.attempted_records == kPreloadedRecords + kProducerCount &&
                       full.enqueued_records == kTopologyCapacity &&
                       full.rejected_records == kProducerCount - 1U &&
                       full.full_rejections + full.contention_rejections == full.rejected_records &&
                       full.invalid_rejections == 0 && full.retained_records == kTopologyCapacity &&
                       full.retained_serialized_bytes == retained_serialized &&
                       full.retained_charge_bytes == retained_charge,
                   topology_name, "near-full concurrent accounting differs");

  for (std::uint64_t expected_sequence = 0; expected_sequence < kTopologyCapacity;
       ++expected_sequence) {
    const ingress::ConsumeResult consumed = topology.TryConsume();
    success &= Check(consumed.status == ingress::ConsumeStatus::kRecord && consumed.record,
                     topology_name, "near-full accepted Record was not consumable");
    if (!consumed.record) {
      continue;
    }
    const std::size_t expected_attempt_id = expected_sequence < kPreloadedRecords
                                                ? static_cast<std::size_t>(expected_sequence)
                                                : accepted_attempt_id;
    success &= Check(consumed.record->admission_sequence == expected_sequence &&
                         consumed.record->record == ExpectedRecord(expected_attempt_id),
                     topology_name, "near-full drain changed FIFO sequence or Record metadata");
  }

  const ingress::TopologySnapshot drained = topology.GetSnapshot();
  success &= Check(drained.dequeued_records == kTopologyCapacity && drained.retained_records == 0 &&
                       drained.retained_serialized_bytes == 0 && drained.retained_charge_bytes == 0,
                   topology_name, "near-full topology did not drain exactly");
  return success;
}

template <typename Topology, typename... ConstructorArgs>
bool RunConcurrentStress(ConstructorArgs... constructor_args) {
  Topology topology{constructor_args...};
  const std::string_view topology_name = Topology::Name();
  auto bookkeeping = std::make_unique<StressBookkeeping>();
  std::array<std::thread, kProducerCount> producers;
  std::barrier start{kStartParticipants};
  std::barrier first_wave_finished{kPhaseParticipants};
  std::barrier drain_gate{kPhaseParticipants};
  std::atomic<bool> stop{false};
  std::atomic<bool> watchdog_done{false};
  std::atomic<bool> timed_out{false};
  std::atomic<std::uint64_t> attempts_completed{0};
  std::atomic<std::uint64_t> accepted_count{0};
  std::atomic<std::uint64_t> consumed_count{0};
  std::atomic<std::size_t> producers_done{0};
  std::atomic<std::uint64_t> result_errors{0};
  std::atomic<std::uint64_t> publication_bound_errors{0};
  std::atomic<std::uint64_t> sequence_errors{0};
  std::atomic<std::uint64_t> record_errors{0};
  std::atomic<std::uint64_t> duplicate_errors{0};
  std::atomic<std::uint64_t> watchdog_polls{0};
  ingress::TopologySnapshot saturated_snapshot{};
  const auto deadline = std::chrono::steady_clock::now() + kStressDeadline;

  std::thread watchdog{[&] {
    start.arrive_and_wait();
    std::uint64_t polls = 0;
    do {
      ++polls;
      if ((polls & 1023U) == 0U && std::chrono::steady_clock::now() >= deadline) {
        timed_out.store(true, std::memory_order_relaxed);
        stop.store(true, std::memory_order_release);
      }
      if ((polls & 1023U) == 0U) {
        std::this_thread::yield();
      }
    } while (!watchdog_done.load(std::memory_order_acquire));
    watchdog_polls.store(polls, std::memory_order_relaxed);
  }};

  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    producers[producer_index] = std::thread{[&, producer_index] {
      start.arrive_and_wait();

      const auto publish = [&](std::size_t attempt) {
        const std::size_t attempt_id = producer_index * kAttemptsPerProducer + attempt;
        const ingress::RecordHandle record = ExpectedRecord(attempt_id);
        const ingress::PublishResult result = topology.TryPublish(producer_index, record);
        attempts_completed.fetch_add(1, std::memory_order_relaxed);
        if (result.publication_actions > topology.MaximumPublicationActions()) {
          publication_bound_errors.fetch_add(1, std::memory_order_relaxed);
        }

        if (result.status == ingress::PublishStatus::kAccepted) {
          if (!result.admission_sequence) {
            result_errors.fetch_add(1, std::memory_order_relaxed);
            return;
          }
          bookkeeping->accepted_sequences[attempt_id].store(*result.admission_sequence + 1U,
                                                            std::memory_order_relaxed);
          bookkeeping->accepted[attempt_id].store(1, std::memory_order_release);
          accepted_count.fetch_add(1, std::memory_order_relaxed);
          return;
        }

        if (result.admission_sequence || (result.status != ingress::PublishStatus::kFull &&
                                          result.status != ingress::PublishStatus::kContended)) {
          result_errors.fetch_add(1, std::memory_order_relaxed);
        }
      };

      for (std::size_t attempt = 0; attempt < kInitialAttemptsPerProducer; ++attempt) {
        publish(attempt);
      }
      first_wave_finished.arrive_and_wait();
      drain_gate.arrive_and_wait();

      for (std::size_t attempt = kInitialAttemptsPerProducer;
           attempt < kAttemptsPerProducer && !stop.load(std::memory_order_acquire); ++attempt) {
        publish(attempt);
        if ((attempt & 255U) == 0U && std::chrono::steady_clock::now() >= deadline) {
          timed_out.store(true, std::memory_order_relaxed);
          stop.store(true, std::memory_order_release);
        }
      }
      producers_done.fetch_add(1, std::memory_order_release);
    }};
  }

  std::thread consumer{[&] {
    start.arrive_and_wait();
    first_wave_finished.arrive_and_wait();
    saturated_snapshot = topology.GetSnapshot();
    drain_gate.arrive_and_wait();

    std::uint64_t expected_sequence = 0;
    std::uint64_t polls = 0;
    while (!stop.load(std::memory_order_acquire)) {
      const ingress::ConsumeResult result = topology.TryConsume();
      if (result.status == ingress::ConsumeStatus::kRecord) {
        if (!result.record) {
          result_errors.fetch_add(1, std::memory_order_relaxed);
          continue;
        }

        const ingress::ConsumedRecord consumed = *result.record;
        if (consumed.admission_sequence != expected_sequence) {
          sequence_errors.fetch_add(1, std::memory_order_relaxed);
        }
        ++expected_sequence;

        const std::size_t attempt_id = consumed.record.slot_index;
        if (attempt_id >= kAttemptCount) {
          record_errors.fetch_add(1, std::memory_order_relaxed);
        } else {
          if (!(consumed.record == ExpectedRecord(attempt_id))) {
            record_errors.fetch_add(1, std::memory_order_relaxed);
          }
          if (bookkeeping->consumed[attempt_id].exchange(1, std::memory_order_relaxed) != 0U) {
            duplicate_errors.fetch_add(1, std::memory_order_relaxed);
          }
          bookkeeping->consumed_sequences[attempt_id].store(consumed.admission_sequence + 1U,
                                                            std::memory_order_relaxed);
        }
        consumed_count.fetch_add(1, std::memory_order_relaxed);
      } else if (result.record) {
        result_errors.fetch_add(1, std::memory_order_relaxed);
      } else {
        std::this_thread::yield();
      }

      ++polls;
      if (producers_done.load(std::memory_order_acquire) == kProducerCount &&
          consumed_count.load(std::memory_order_relaxed) ==
              accepted_count.load(std::memory_order_relaxed)) {
        break;
      }
      if ((polls & 1023U) == 0U && std::chrono::steady_clock::now() >= deadline) {
        timed_out.store(true, std::memory_order_relaxed);
        stop.store(true, std::memory_order_release);
      }
    }
  }};

  for (auto& producer : producers) {
    producer.join();
  }
  consumer.join();
  watchdog_done.store(true, std::memory_order_release);
  watchdog.join();

  std::uint64_t accepted_in_bookkeeping = 0;
  std::uint64_t consumed_in_bookkeeping = 0;
  std::uint64_t acceptance_mismatches = 0;
  std::uint64_t admission_sequence_mismatches = 0;
  for (std::size_t attempt_id = 0; attempt_id < kAttemptCount; ++attempt_id) {
    const bool accepted = bookkeeping->accepted[attempt_id].load(std::memory_order_relaxed) != 0U;
    const bool consumed = bookkeeping->consumed[attempt_id].load(std::memory_order_relaxed) != 0U;
    accepted_in_bookkeeping += accepted ? 1U : 0U;
    consumed_in_bookkeeping += consumed ? 1U : 0U;
    acceptance_mismatches += accepted != consumed ? 1U : 0U;
    if (accepted && consumed &&
        bookkeeping->accepted_sequences[attempt_id].load(std::memory_order_relaxed) !=
            bookkeeping->consumed_sequences[attempt_id].load(std::memory_order_relaxed)) {
      ++admission_sequence_mismatches;
    }
  }

  const ingress::TopologySnapshot final_snapshot = topology.GetSnapshot();
  const std::uint64_t completed = attempts_completed.load(std::memory_order_relaxed);
  const std::uint64_t accepted = accepted_count.load(std::memory_order_relaxed);
  const std::uint64_t consumed = consumed_count.load(std::memory_order_relaxed);
  bool success = true;
  success &= Check(!timed_out.load(std::memory_order_relaxed), topology_name,
                   "stress exceeded its five-second internal deadline");
  success &= Check(completed == kAttemptCount, topology_name,
                   "not every fixed producer attempt completed");
  success &=
      Check(saturated_snapshot.attempted_records == kProducerCount * kInitialAttemptsPerProducer &&
                saturated_snapshot.retained_records == kTopologyCapacity &&
                saturated_snapshot.enqueued_records == kTopologyCapacity,
            topology_name, "the consumer-gated first wave did not saturate capacity");
  success &= Check(saturated_snapshot.rejected_records > 0, topology_name,
                   "saturated first wave did not exercise rejection");
  success &= Check(publication_bound_errors.load(std::memory_order_relaxed) == 0, topology_name,
                   "a producer call exceeded the documented publication action bound");
  success &= Check(result_errors.load(std::memory_order_relaxed) == 0, topology_name,
                   "publish or consume returned an inconsistent result");
  success &= Check(sequence_errors.load(std::memory_order_relaxed) == 0, topology_name,
                   "consumption did not follow strict admission sequence");
  success &= Check(record_errors.load(std::memory_order_relaxed) == 0, topology_name,
                   "consumed RecordHandle metadata was corrupted");
  success &= Check(duplicate_errors.load(std::memory_order_relaxed) == 0, topology_name,
                   "an accepted RecordHandle was consumed more than once");
  success &= Check(acceptance_mismatches == 0 && admission_sequence_mismatches == 0, topology_name,
                   "accepted and consumed envelopes differ");
  success &= Check(accepted == accepted_in_bookkeeping && consumed == consumed_in_bookkeeping &&
                       accepted == consumed,
                   topology_name, "accepted or consumed accounting differs from bookkeeping");
  success &= Check(final_snapshot.attempted_records == completed &&
                       final_snapshot.enqueued_records == accepted &&
                       final_snapshot.dequeued_records == consumed &&
                       final_snapshot.rejected_records == completed - accepted &&
                       final_snapshot.full_rejections + final_snapshot.contention_rejections +
                               final_snapshot.invalid_rejections ==
                           final_snapshot.rejected_records,
                   topology_name, "final topology accounting is not exact");
  success &= Check(final_snapshot.invalid_rejections == 0 && final_snapshot.retained_records == 0 &&
                       final_snapshot.retained_serialized_bytes == 0 &&
                       final_snapshot.retained_charge_bytes == 0,
                   topology_name, "the drained topology retained records or bytes");
  success &= Check(watchdog_polls.load(std::memory_order_relaxed) > 0, topology_name,
                   "cooperative stress watchdog did not run");
  return success;
}

}  // namespace

int main() {
  const bool ring = RunNearFullStress<ingress::BoundedMpscRing<kTopologyCapacity>>() &&
                    RunConcurrentStress<ingress::BoundedMpscRing<kTopologyCapacity>>();
  const bool chunked = RunNearFullStress<ingress::ChunkedMpsc<kTopologyCapacity, 8>>() &&
                       RunConcurrentStress<ingress::ChunkedMpsc<kTopologyCapacity, 8>>();
  const bool lanes =
      RunNearFullStress<ingress::PerProducerLanes<kTopologyCapacity>>(kProducerCount) &&
      RunConcurrentStress<ingress::PerProducerLanes<kTopologyCapacity>>(kProducerCount);
  return ring && chunked && lanes ? 0 : 1;
}
