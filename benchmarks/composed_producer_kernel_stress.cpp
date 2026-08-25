#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <thread>

#include "prototypes/composed/composed_producer_kernel.hpp"
#include "prototypes/record_storage/record_storage.hpp"

namespace {

namespace composed = ulog::benchmark_support::composed;
namespace ingress = ulog::benchmark_support::ingress;
namespace storage = ulog::benchmark_support::record_storage;

constexpr std::size_t kProducerCount = ingress::kMaximumConcurrentPublishers;
constexpr std::size_t kIngressCapacity = 64;
constexpr std::size_t kInitialAttemptsPerProducer = 8;
constexpr std::size_t kInitialAcceptedPerProducer = 2;
constexpr std::size_t kRandomAttemptsPerProducer = 1024;
constexpr std::size_t kAttemptsPerProducer =
    kInitialAttemptsPerProducer + kRandomAttemptsPerProducer;
constexpr std::size_t kInitialAttemptCount = kProducerCount * kInitialAttemptsPerProducer;
constexpr std::size_t kInitialAcceptedCount = kProducerCount * kInitialAcceptedPerProducer;
constexpr std::size_t kInitialRejectedCount = kInitialAttemptCount - kInitialAcceptedCount;
constexpr std::size_t kAttemptCount = kProducerCount * kAttemptsPerProducer;
constexpr std::size_t kMessageBytes = 64;
constexpr std::size_t kEncodedIdBytes = 8;
constexpr auto kWatchdogDeadline = std::chrono::seconds{8};
constexpr std::ptrdiff_t kProducerPhaseParticipants =
    static_cast<std::ptrdiff_t>(kProducerCount + 1U);

using Message = std::array<std::byte, kMessageBytes>;
using Writer = storage::ContiguousRecordSlot::Writer;

enum class Phase : std::uint8_t {
  kStarting,
  kInitialWave,
  kInitialDrain,
  kRandomizedWave,
  kFinalDrain,
  kValidation,
  kComplete,
};

struct StressBookkeeping final {
  std::array<std::atomic<std::uint8_t>, kAttemptCount> attempted{};
  std::array<std::atomic<std::uint8_t>, kAttemptCount> accepted{};
  std::array<std::atomic<std::uint8_t>, kAttemptCount> consumed{};
  std::array<std::atomic<std::uint64_t>, kAttemptCount> accepted_sequences{};
  std::array<std::atomic<std::uint64_t>, kAttemptCount> consumed_sequences{};
  std::array<std::atomic<std::uint64_t>, kAttemptCount> accepted_sequence_owners{};
  std::array<std::atomic<std::uint64_t>, kAttemptCount> consumed_sequence_owners{};
};

struct StressCounters final {
  std::atomic<std::uint64_t> attempts_completed{0};
  std::atomic<std::uint64_t> accepted{0};
  std::atomic<std::uint64_t> ingress_rejected{0};
  std::atomic<std::uint64_t> budget_rejected{0};
  std::atomic<std::uint64_t> invalid{0};
  std::atomic<std::uint64_t> message_callbacks{0};
  std::atomic<std::uint64_t> context_callbacks{0};
  std::atomic<std::uint64_t> records_dequeued{0};
  std::atomic<std::uint64_t> observer_callbacks{0};
  std::atomic<std::uint64_t> producers_done{0};
  std::atomic<std::uint64_t> result_errors{0};
  std::atomic<std::uint64_t> writer_errors{0};
  std::atomic<std::uint64_t> rejected_callback_errors{0};
  std::atomic<std::uint64_t> attempt_duplicate_errors{0};
  std::atomic<std::uint64_t> sequence_range_errors{0};
  std::atomic<std::uint64_t> sequence_duplicate_errors{0};
  std::atomic<std::uint64_t> fifo_errors{0};
  std::atomic<std::uint64_t> content_errors{0};
  std::atomic<std::uint64_t> consumption_duplicate_errors{0};
};

[[nodiscard]] constexpr std::string_view PhaseName(Phase phase) noexcept {
  switch (phase) {
    case Phase::kStarting:
      return "starting";
    case Phase::kInitialWave:
      return "initial-wave";
    case Phase::kInitialDrain:
      return "initial-drain";
    case Phase::kRandomizedWave:
      return "randomized-wave";
    case Phase::kFinalDrain:
      return "final-drain";
    case Phase::kValidation:
      return "validation";
    case Phase::kComplete:
      return "complete";
  }
  return "unknown";
}

[[nodiscard]] constexpr Message MakeMessage(std::size_t attempt_id) noexcept {
  constexpr std::string_view kHexDigits = "0123456789abcdef";
  Message message{};
  for (std::size_t index = 0; index < message.size(); ++index) {
    message[index] =
        static_cast<std::byte>('a' + static_cast<unsigned char>((attempt_id + index) % 26U));
  }
  for (std::size_t index = 0; index < kEncodedIdBytes; ++index) {
    const std::size_t shift = 4U * (kEncodedIdBytes - index - 1U);
    message[index] = static_cast<std::byte>(
        kHexDigits[(static_cast<std::uint64_t>(attempt_id) >> shift) & 0x0fU]);
  }
  message[kEncodedIdBytes] = static_cast<std::byte>(':');
  return message;
}

[[nodiscard]] constexpr int HexValue(std::byte value) noexcept {
  const auto character = static_cast<unsigned char>(value);
  if (character >= static_cast<unsigned char>('0') &&
      character <= static_cast<unsigned char>('9')) {
    return character - static_cast<unsigned char>('0');
  }
  if (character >= static_cast<unsigned char>('a') &&
      character <= static_cast<unsigned char>('f')) {
    return character - static_cast<unsigned char>('a') + 10;
  }
  return -1;
}

[[nodiscard]] constexpr bool DecodeAttemptId(const Message& message,
                                             std::size_t& attempt_id) noexcept {
  if (message[kEncodedIdBytes] != static_cast<std::byte>(':')) {
    return false;
  }
  std::uint64_t decoded = 0;
  for (std::size_t index = 0; index < kEncodedIdBytes; ++index) {
    const int digit = HexValue(message[index]);
    if (digit < 0) {
      return false;
    }
    decoded = (decoded << 4U) | static_cast<std::uint64_t>(digit);
  }
  if (decoded > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  attempt_id = static_cast<std::size_t>(decoded);
  return true;
}

[[nodiscard]] bool HasBenchmarkContent(const storage::RecordView& record) noexcept {
  if (!record.source_path().Equals(storage::kBenchmarkSourcePath) ||
      !record.source_function().Equals(storage::kBenchmarkSourceFunction) ||
      record.source_line() != storage::kBenchmarkSourceLine ||
      record.event_timestamp() != storage::kBenchmarkEventTimestamp ||
      record.field_count() != storage::kBenchmarkFieldCount) {
    return false;
  }

  const auto string_field = record.FieldAt(0);
  const auto signed_field = record.FieldAt(1);
  const auto unsigned_field = record.FieldAt(2);
  const auto double_field = record.FieldAt(3);
  const auto bool_field = record.FieldAt(4);
  const auto null_field = record.FieldAt(5);
  return string_field && string_field->key().Equals(storage::kBenchmarkStringFieldKey) &&
         string_field->AsString() &&
         string_field->AsString()->Equals(storage::kBenchmarkStringFieldValue) && signed_field &&
         signed_field->key().Equals(storage::kBenchmarkInt64FieldKey) &&
         signed_field->AsInt64() == std::int64_t{-7} && unsigned_field &&
         unsigned_field->key().Equals(storage::kBenchmarkUInt64FieldKey) &&
         unsigned_field->AsUInt64() == std::uint64_t{42} && double_field &&
         double_field->key().Equals(storage::kBenchmarkDoubleFieldKey) &&
         double_field->AsDouble() == 1.25 && bool_field &&
         bool_field->key().Equals(storage::kBenchmarkBoolFieldKey) &&
         bool_field->AsBool() == true && null_field &&
         null_field->key().Equals(storage::kBenchmarkNullFieldKey) && null_field->IsNull();
}

[[nodiscard]] constexpr std::uint64_t NextRandom(std::uint64_t& state) noexcept {
  state ^= state >> 12U;
  state ^= state << 25U;
  state ^= state >> 27U;
  return state * 0x2545f4914f6cdd1dULL;
}

bool Check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << "composed producer stress: " << message << '\n';
  }
  return condition;
}

template <typename Actual, typename Expected>
bool CheckEqual(Actual actual, Expected expected, std::string_view field) {
  if (actual == expected) {
    return true;
  }
  std::cerr << "composed producer stress: " << field << " differs (actual=" << actual
            << ", expected=" << expected << ")\n";
  return false;
}

bool RunStress() {
  static_assert(kIngressCapacity / kProducerCount == kInitialAcceptedPerProducer);
  const composed::RecordPlan plan = composed::RecordPlan::Benchmark(kMessageBytes);
  const std::size_t serialized_bytes =
      static_cast<std::size_t>(plan.maximum_footprint().SerializedBytes());
  const std::size_t charge_bytes =
      static_cast<std::size_t>(plan.maximum_footprint().accounting_charge_bytes);
  const std::size_t retained_limit_bytes = kIngressCapacity * charge_bytes;
  const std::size_t logical_retained_ceiling = kIngressCapacity * serialized_bytes;
  composed::ComposedProducerPath<kIngressCapacity> path{retained_limit_bytes, 0, kProducerCount};
  auto bookkeeping = std::make_unique<StressBookkeeping>();
  StressCounters counters;
  std::array<std::thread, kProducerCount> producers;
  std::barrier initial_start{kProducerPhaseParticipants};
  std::barrier initial_wave_finished{kProducerPhaseParticipants};
  std::barrier randomized_wave_start{kProducerPhaseParticipants};
  std::atomic<Phase> phase{Phase::kStarting};
  std::atomic<bool> consumer_enabled{false};
  std::atomic<bool> initial_drain_done{false};
  std::atomic<std::uint64_t> initial_drain_target{0};
  std::atomic<bool> watchdog_done{false};
  const auto deadline = std::chrono::steady_clock::now() + kWatchdogDeadline;

  std::thread watchdog{[&] {
    while (!watchdog_done.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::cerr << "composed producer stress watchdog timeout: phase="
                  << PhaseName(phase.load(std::memory_order_acquire))
                  << " attempts=" << counters.attempts_completed.load(std::memory_order_relaxed)
                  << " accepted=" << counters.accepted.load(std::memory_order_relaxed)
                  << " dequeued=" << counters.records_dequeued.load(std::memory_order_relaxed)
                  << " observed=" << counters.observer_callbacks.load(std::memory_order_relaxed)
                  << " producers_done=" << counters.producers_done.load(std::memory_order_relaxed)
                  << '\n';
        std::cerr.flush();
        std::abort();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }};

  std::thread consumer{[&] {
    consumer_enabled.wait(false, std::memory_order_acquire);
    std::uint64_t expected_sequence = 0;
    bool reported_initial_drain = false;
    for (;;) {
      bool observer_called = false;
      const auto status =
          path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
            if (observer_called) {
              counters.result_errors.fetch_add(1, std::memory_order_relaxed);
            }
            observer_called = true;
            counters.observer_callbacks.fetch_add(1, std::memory_order_relaxed);
            if (sequence != expected_sequence) {
              counters.fifo_errors.fetch_add(1, std::memory_order_relaxed);
            }

            Message actual_message{};
            const std::size_t copied =
                record.message().CopyTo(std::span<std::byte>{actual_message});
            std::size_t attempt_id = 0;
            const bool valid_id = copied == actual_message.size() &&
                                  DecodeAttemptId(actual_message, attempt_id) &&
                                  attempt_id < kAttemptCount;
            if (!valid_id || (valid_id && actual_message != MakeMessage(attempt_id)) ||
                !HasBenchmarkContent(record)) {
              counters.content_errors.fetch_add(1, std::memory_order_relaxed);
            }
            if (valid_id) {
              if (bookkeeping->consumed[attempt_id].exchange(1, std::memory_order_relaxed) != 0U) {
                counters.consumption_duplicate_errors.fetch_add(1, std::memory_order_relaxed);
              }
              bookkeeping->consumed_sequences[attempt_id].store(sequence + 1U,
                                                                std::memory_order_relaxed);
            }
            if (sequence >= kAttemptCount) {
              counters.sequence_range_errors.fetch_add(1, std::memory_order_relaxed);
            } else {
              const std::uint64_t owner = valid_id ? static_cast<std::uint64_t>(attempt_id) + 1U
                                                   : std::numeric_limits<std::uint64_t>::max();
              if (bookkeeping->consumed_sequence_owners[sequence].exchange(
                      owner, std::memory_order_relaxed) != 0U) {
                counters.sequence_duplicate_errors.fetch_add(1, std::memory_order_relaxed);
              }
            }
          });

      if (status == ingress::ConsumeStatus::kRecord) {
        if (!observer_called) {
          counters.result_errors.fetch_add(1, std::memory_order_relaxed);
        }
        ++expected_sequence;
        const std::uint64_t dequeued =
            counters.records_dequeued.fetch_add(1, std::memory_order_relaxed) + 1U;
        if (!reported_initial_drain &&
            dequeued >= initial_drain_target.load(std::memory_order_acquire)) {
          reported_initial_drain = true;
          initial_drain_done.store(true, std::memory_order_release);
          initial_drain_done.notify_one();
        }
      } else {
        if (observer_called) {
          counters.result_errors.fetch_add(1, std::memory_order_relaxed);
        }
        if (!reported_initial_drain && counters.records_dequeued.load(std::memory_order_relaxed) >=
                                           initial_drain_target.load(std::memory_order_acquire)) {
          reported_initial_drain = true;
          initial_drain_done.store(true, std::memory_order_release);
          initial_drain_done.notify_one();
        }
        std::this_thread::yield();
      }

      if (counters.producers_done.load(std::memory_order_acquire) == kProducerCount &&
          counters.records_dequeued.load(std::memory_order_relaxed) ==
              counters.accepted.load(std::memory_order_relaxed)) {
        break;
      }
    }
  }};

  const auto produce = [&](std::size_t producer_index, std::size_t local_attempt) noexcept {
    const std::size_t attempt_id = producer_index * kAttemptsPerProducer + local_attempt;
    if (bookkeeping->attempted[attempt_id].exchange(1, std::memory_order_relaxed) != 0U) {
      counters.attempt_duplicate_errors.fetch_add(1, std::memory_order_relaxed);
    }
    const Message message = MakeMessage(attempt_id);
    bool message_called = false;
    bool context_called = false;
    bool writer_succeeded = true;
    const auto result = path.TryProduce(
        producer_index, plan,
        [&](Writer& writer) noexcept {
          message_called = true;
          counters.message_callbacks.fetch_add(1, std::memory_order_relaxed);
          const auto written = writer.Append(std::span<const std::byte>{message});
          writer_succeeded = written.requested_bytes == message.size() &&
                             written.stored_bytes == message.size() && !written.truncated;
          return writer_succeeded;
        },
        [&](Writer& writer) noexcept {
          context_called = true;
          counters.context_callbacks.fetch_add(1, std::memory_order_relaxed);
          const bool stored = composed::AddBenchmarkContext(writer);
          writer_succeeded = writer_succeeded && stored;
          return stored;
        });

    if (!writer_succeeded) {
      counters.writer_errors.fetch_add(1, std::memory_order_relaxed);
    }
    if (result.status == composed::ProduceStatus::kAccepted) {
      counters.accepted.fetch_add(1, std::memory_order_relaxed);
      if (!message_called || !context_called || !result.admission_sequence) {
        counters.result_errors.fetch_add(1, std::memory_order_relaxed);
      }
      if (result.admission_sequence) {
        const std::uint64_t sequence = *result.admission_sequence;
        bookkeeping->accepted_sequences[attempt_id].store(sequence + 1U, std::memory_order_relaxed);
        if (sequence >= kAttemptCount) {
          counters.sequence_range_errors.fetch_add(1, std::memory_order_relaxed);
        } else if (bookkeeping->accepted_sequence_owners[sequence].exchange(
                       static_cast<std::uint64_t>(attempt_id) + 1U, std::memory_order_relaxed) !=
                   0U) {
          counters.sequence_duplicate_errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
      if (bookkeeping->accepted[attempt_id].exchange(1, std::memory_order_release) != 0U) {
        counters.attempt_duplicate_errors.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      if (message_called || context_called) {
        counters.rejected_callback_errors.fetch_add(1, std::memory_order_relaxed);
      }
      if (result.admission_sequence) {
        counters.result_errors.fetch_add(1, std::memory_order_relaxed);
      }
      switch (result.status) {
        case composed::ProduceStatus::kIngressRejected:
          counters.ingress_rejected.fetch_add(1, std::memory_order_relaxed);
          break;
        case composed::ProduceStatus::kBudgetRejected:
          counters.budget_rejected.fetch_add(1, std::memory_order_relaxed);
          break;
        case composed::ProduceStatus::kInvalid:
          counters.invalid.fetch_add(1, std::memory_order_relaxed);
          break;
        case composed::ProduceStatus::kAccepted:
          counters.result_errors.fetch_add(1, std::memory_order_relaxed);
          break;
      }
    }
    counters.attempts_completed.fetch_add(1, std::memory_order_relaxed);
  };

  phase.store(Phase::kInitialWave, std::memory_order_release);
  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    producers[producer_index] = std::thread{[&, producer_index] {
      initial_start.arrive_and_wait();
      for (std::size_t local_attempt = 0; local_attempt < kInitialAttemptsPerProducer;
           ++local_attempt) {
        produce(producer_index, local_attempt);
      }
      initial_wave_finished.arrive_and_wait();
      randomized_wave_start.arrive_and_wait();

      std::uint64_t random_state =
          0x9e3779b97f4a7c15ULL ^ (static_cast<std::uint64_t>(producer_index) + 1U);
      const std::size_t permutation_multiplier =
          static_cast<std::size_t>(NextRandom(random_state) | 1U);
      const std::size_t permutation_offset =
          static_cast<std::size_t>(NextRandom(random_state)) & (kRandomAttemptsPerProducer - 1U);
      for (std::size_t attempt = 0; attempt < kRandomAttemptsPerProducer; ++attempt) {
        const std::size_t randomized_attempt =
            (attempt * permutation_multiplier + permutation_offset) &
            (kRandomAttemptsPerProducer - 1U);
        if ((NextRandom(random_state) & 0x0fU) == 0U) {
          std::this_thread::yield();
        }
        produce(producer_index, kInitialAttemptsPerProducer + randomized_attempt);
      }
      counters.producers_done.fetch_add(1, std::memory_order_acq_rel);
    }};
  }

  initial_start.arrive_and_wait();
  initial_wave_finished.arrive_and_wait();

  bool success = true;
  const auto initial = path.Snapshot();
  success &= CheckEqual(counters.attempts_completed.load(std::memory_order_relaxed),
                        std::uint64_t{kInitialAttemptCount}, "initial attempts");
  success &= CheckEqual(counters.accepted.load(std::memory_order_relaxed),
                        std::uint64_t{kInitialAcceptedCount}, "initial accepted Records");
  success &= CheckEqual(counters.ingress_rejected.load(std::memory_order_relaxed),
                        std::uint64_t{kInitialRejectedCount}, "initial ingress rejections");
  success &= CheckEqual(counters.budget_rejected.load(std::memory_order_relaxed), std::uint64_t{0},
                        "initial budget rejections");
  success &= CheckEqual(counters.invalid.load(std::memory_order_relaxed), std::uint64_t{0},
                        "initial invalid results");
  success &= CheckEqual(counters.message_callbacks.load(std::memory_order_relaxed),
                        std::uint64_t{kInitialAcceptedCount}, "initial message callbacks");
  success &= CheckEqual(counters.context_callbacks.load(std::memory_order_relaxed),
                        std::uint64_t{kInitialAcceptedCount}, "initial context callbacks");
  success &= CheckEqual(counters.rejected_callback_errors.load(std::memory_order_relaxed),
                        std::uint64_t{0}, "callbacks on rejected initial attempts");
  for (std::size_t producer = 0; producer < kProducerCount; ++producer) {
    for (std::size_t local_attempt = 0; local_attempt < kInitialAttemptsPerProducer;
         ++local_attempt) {
      const std::size_t attempt_id = producer * kAttemptsPerProducer + local_attempt;
      const bool accepted = bookkeeping->accepted[attempt_id].load(std::memory_order_relaxed) != 0U;
      success &= Check(accepted == (local_attempt < kInitialAcceptedPerProducer),
                       "a producer lane did not accept exactly its first two Records");
    }
  }
  success &= CheckEqual(initial.attempted_records, std::uint64_t{kInitialAttemptCount},
                        "initial path attempts");
  success &= CheckEqual(initial.accepted_records, std::uint64_t{kInitialAcceptedCount},
                        "initial path accepted Records");
  success &= CheckEqual(initial.rejected_records, std::uint64_t{kInitialRejectedCount},
                        "initial path rejected Records");
  success &= CheckEqual(initial.message_callback_count, std::uint64_t{kInitialAcceptedCount},
                        "initial path message callbacks");
  success &= CheckEqual(initial.context_callback_count, std::uint64_t{kInitialAcceptedCount},
                        "initial path context callbacks");
  success &= CheckEqual(initial.published_records, std::uint64_t{kInitialAcceptedCount},
                        "initial published Records");
  success &=
      CheckEqual(initial.consumed_records, std::uint64_t{0}, "consumer ran before its gate opened");
  success &= CheckEqual(initial.logical_retained_bytes, std::uint64_t{logical_retained_ceiling},
                        "deterministic logical retained ceiling");
  success &= CheckEqual(initial.physical_retained_bytes,
                        std::uint64_t{kInitialAcceptedCount * charge_bytes},
                        "initial physical retained bytes");
  success &= CheckEqual(initial.retained_limit_bytes, std::uint64_t{retained_limit_bytes},
                        "retained byte limit");
  success &= Check(initial.logical_retained_bytes <= initial.retained_limit_bytes &&
                       initial.physical_retained_bytes <= initial.retained_limit_bytes,
                   "initial retained accounting exceeded its bound");
  success &= CheckEqual(initial.topology.attempted_records, std::uint64_t{kInitialAttemptCount},
                        "initial topology attempts");
  success &= CheckEqual(initial.topology.enqueued_records, std::uint64_t{kInitialAcceptedCount},
                        "initial topology enqueues");
  success &= CheckEqual(initial.topology.rejected_records, std::uint64_t{kInitialRejectedCount},
                        "initial topology rejections");
  success &= CheckEqual(initial.topology.full_rejections, std::uint64_t{kInitialRejectedCount},
                        "initial lane-full rejections");
  success &= CheckEqual(initial.topology.contention_rejections, std::uint64_t{0},
                        "initial contention rejections");
  success &= CheckEqual(initial.topology.invalid_rejections, std::uint64_t{0},
                        "initial topology invalid rejections");
  success &=
      CheckEqual(initial.topology.dequeued_records, std::uint64_t{0}, "initial topology dequeues");
  success &= CheckEqual(initial.topology.retained_records, std::uint64_t{kInitialAcceptedCount},
                        "initial retained Records");
  success &= CheckEqual(initial.topology.retained_serialized_bytes,
                        std::uint64_t{kInitialAcceptedCount * serialized_bytes},
                        "initial retained serialized bytes");
  success &= CheckEqual(initial.topology.retained_charge_bytes,
                        std::uint64_t{kInitialAcceptedCount * charge_bytes},
                        "initial retained charge bytes");

  phase.store(Phase::kInitialDrain, std::memory_order_release);
  initial_drain_target.store(counters.accepted.load(std::memory_order_relaxed),
                             std::memory_order_release);
  consumer_enabled.store(true, std::memory_order_release);
  consumer_enabled.notify_one();
  initial_drain_done.wait(false, std::memory_order_acquire);

  const auto initially_drained = path.Snapshot();
  success &= CheckEqual(initially_drained.consumed_records, std::uint64_t{kInitialAcceptedCount},
                        "initial drain consumed Records");
  success &= CheckEqual(initially_drained.topology.dequeued_records,
                        std::uint64_t{kInitialAcceptedCount}, "initial drain topology dequeues");
  success &= CheckEqual(initially_drained.topology.retained_records, std::uint64_t{0},
                        "initial drain retained Records");
  success &= CheckEqual(initially_drained.topology.retained_serialized_bytes, std::uint64_t{0},
                        "initial drain retained serialized bytes");
  success &= CheckEqual(initially_drained.topology.retained_charge_bytes, std::uint64_t{0},
                        "initial drain retained charge bytes");
  success &= CheckEqual(initially_drained.logical_retained_bytes, std::uint64_t{0},
                        "initial drain logical retained baseline");
  success &=
      CheckEqual(initially_drained.physical_retained_bytes, std::uint64_t{retained_limit_bytes},
                 "initial drain physical cached credit");

  phase.store(Phase::kRandomizedWave, std::memory_order_release);
  randomized_wave_start.arrive_and_wait();
  for (auto& producer : producers) {
    producer.join();
  }
  phase.store(Phase::kFinalDrain, std::memory_order_release);
  consumer.join();

  phase.store(Phase::kValidation, std::memory_order_release);
  std::uint64_t attempted_in_bookkeeping = 0;
  std::uint64_t accepted_in_bookkeeping = 0;
  std::uint64_t consumed_in_bookkeeping = 0;
  std::uint64_t acceptance_mismatches = 0;
  std::uint64_t attempt_sequence_mismatches = 0;
  for (std::size_t attempt_id = 0; attempt_id < kAttemptCount; ++attempt_id) {
    const bool attempted = bookkeeping->attempted[attempt_id].load(std::memory_order_relaxed) != 0U;
    const bool accepted = bookkeeping->accepted[attempt_id].load(std::memory_order_relaxed) != 0U;
    const bool consumed = bookkeeping->consumed[attempt_id].load(std::memory_order_relaxed) != 0U;
    attempted_in_bookkeeping += attempted ? 1U : 0U;
    accepted_in_bookkeeping += accepted ? 1U : 0U;
    consumed_in_bookkeeping += consumed ? 1U : 0U;
    acceptance_mismatches += accepted != consumed ? 1U : 0U;
    if (accepted && consumed &&
        bookkeeping->accepted_sequences[attempt_id].load(std::memory_order_relaxed) !=
            bookkeeping->consumed_sequences[attempt_id].load(std::memory_order_relaxed)) {
      ++attempt_sequence_mismatches;
    }
  }

  const std::uint64_t accepted = counters.accepted.load(std::memory_order_relaxed);
  std::uint64_t missing_or_mismatched_sequences = 0;
  for (std::size_t sequence = 0; sequence < kAttemptCount; ++sequence) {
    const std::uint64_t accepted_owner =
        bookkeeping->accepted_sequence_owners[sequence].load(std::memory_order_relaxed);
    const std::uint64_t consumed_owner =
        bookkeeping->consumed_sequence_owners[sequence].load(std::memory_order_relaxed);
    const bool sequence_expected = sequence < accepted;
    if ((accepted_owner != 0U) != sequence_expected ||
        (consumed_owner != 0U) != sequence_expected || accepted_owner != consumed_owner) {
      ++missing_or_mismatched_sequences;
    }
  }

  const auto final = path.Snapshot();
  success &= CheckEqual(counters.attempts_completed.load(std::memory_order_relaxed),
                        std::uint64_t{kAttemptCount}, "completed attempts");
  success &=
      CheckEqual(attempted_in_bookkeeping, std::uint64_t{kAttemptCount}, "bookkeeping attempts");
  success &= CheckEqual(counters.producers_done.load(std::memory_order_relaxed),
                        std::uint64_t{kProducerCount}, "completed producers");
  success &= Check(accepted >= kInitialAcceptedCount && accepted <= kAttemptCount,
                   "accepted count is outside the possible range");
  success &= CheckEqual(counters.ingress_rejected.load(std::memory_order_relaxed),
                        std::uint64_t{kAttemptCount} - accepted, "total ingress rejections");
  success &= CheckEqual(counters.budget_rejected.load(std::memory_order_relaxed), std::uint64_t{0},
                        "total budget rejections");
  success &= CheckEqual(counters.invalid.load(std::memory_order_relaxed), std::uint64_t{0},
                        "total invalid results");
  success &= CheckEqual(counters.message_callbacks.load(std::memory_order_relaxed), accepted,
                        "message callbacks versus accepted Records");
  success &= CheckEqual(counters.context_callbacks.load(std::memory_order_relaxed), accepted,
                        "context callbacks versus accepted Records");
  success &= CheckEqual(counters.observer_callbacks.load(std::memory_order_relaxed), accepted,
                        "consumer callbacks versus accepted Records");
  success &= CheckEqual(counters.records_dequeued.load(std::memory_order_relaxed), accepted,
                        "dequeued versus accepted Records");
  success &= CheckEqual(accepted_in_bookkeeping, accepted, "bookkeeping accepted Records");
  success &= CheckEqual(consumed_in_bookkeeping, accepted, "bookkeeping consumed Records");
  success &=
      CheckEqual(acceptance_mismatches, std::uint64_t{0}, "accepted/consumed attempt mismatches");
  success &= CheckEqual(attempt_sequence_mismatches, std::uint64_t{0},
                        "attempt admission-sequence mismatches");
  success &= CheckEqual(missing_or_mismatched_sequences, std::uint64_t{0},
                        "missing, duplicate, or mismatched admission sequences");
  success &= CheckEqual(counters.result_errors.load(std::memory_order_relaxed), std::uint64_t{0},
                        "inconsistent produce/consume results");
  success &= CheckEqual(counters.writer_errors.load(std::memory_order_relaxed), std::uint64_t{0},
                        "message writer errors");
  success &= CheckEqual(counters.rejected_callback_errors.load(std::memory_order_relaxed),
                        std::uint64_t{0}, "callbacks on rejected attempts");
  success &= CheckEqual(counters.attempt_duplicate_errors.load(std::memory_order_relaxed),
                        std::uint64_t{0}, "duplicate attempt IDs");
  success &= CheckEqual(counters.sequence_range_errors.load(std::memory_order_relaxed),
                        std::uint64_t{0}, "out-of-range admission sequences");
  success &= CheckEqual(counters.sequence_duplicate_errors.load(std::memory_order_relaxed),
                        std::uint64_t{0}, "duplicate admission sequences");
  success &= CheckEqual(counters.fifo_errors.load(std::memory_order_relaxed), std::uint64_t{0},
                        "consumer FIFO errors");
  success &= CheckEqual(counters.content_errors.load(std::memory_order_relaxed), std::uint64_t{0},
                        "consumed Record content errors");
  success &= CheckEqual(counters.consumption_duplicate_errors.load(std::memory_order_relaxed),
                        std::uint64_t{0}, "duplicate consumed attempt IDs");
  success &=
      CheckEqual(final.attempted_records, std::uint64_t{kAttemptCount}, "final path attempts");
  success &= CheckEqual(final.accepted_records, accepted, "final path accepted Records");
  success &= CheckEqual(final.rejected_records, std::uint64_t{kAttemptCount} - accepted,
                        "final path rejected Records");
  success &=
      CheckEqual(final.message_callback_count, accepted, "final path message callback accounting");
  success &=
      CheckEqual(final.context_callback_count, accepted, "final path context callback accounting");
  success &= CheckEqual(final.published_records, accepted, "final published Records");
  success &= CheckEqual(final.consumed_records, accepted, "final consumed Records");
  success &=
      CheckEqual(final.logical_retained_bytes, std::uint64_t{0}, "final logical retained baseline");
  success &= CheckEqual(final.physical_retained_bytes, std::uint64_t{retained_limit_bytes},
                        "final physical cached credit");
  success &= Check(logical_retained_ceiling <= final.retained_limit_bytes,
                   "lane-derived logical retained ceiling exceeded the byte limit");
  success &= Check(final.logical_retained_bytes <= final.retained_limit_bytes &&
                       final.physical_retained_bytes <= final.retained_limit_bytes,
                   "final retained accounting exceeded its bound");
  success &= CheckEqual(final.fifo_error_count, std::uint64_t{0}, "path FIFO errors");
  success &= CheckEqual(final.record_validation_error_count, std::uint64_t{0},
                        "path Record validation errors");
  success &= CheckEqual(final.publication_error_count, std::uint64_t{0}, "path publication errors");
  success &= CheckEqual(final.topology.attempted_records, std::uint64_t{kAttemptCount},
                        "final topology attempts");
  success &= CheckEqual(final.topology.enqueued_records, accepted, "final topology enqueues");
  success &= CheckEqual(final.topology.dequeued_records, accepted, "final topology dequeues");
  success &= CheckEqual(final.topology.rejected_records, std::uint64_t{kAttemptCount} - accepted,
                        "final topology rejections");
  success &= CheckEqual(final.topology.full_rejections, std::uint64_t{kAttemptCount} - accepted,
                        "final topology lane-full rejections");
  success &= CheckEqual(final.topology.contention_rejections, std::uint64_t{0},
                        "final topology contention rejections");
  success &= CheckEqual(final.topology.invalid_rejections, std::uint64_t{0},
                        "final topology invalid rejections");
  success &= CheckEqual(final.topology.retained_records, std::uint64_t{0},
                        "final topology retained Records");
  success &= CheckEqual(final.topology.retained_serialized_bytes, std::uint64_t{0},
                        "final topology retained serialized bytes");
  success &= CheckEqual(final.topology.retained_charge_bytes, std::uint64_t{0},
                        "final topology retained charge bytes");

  watchdog_done.store(true, std::memory_order_release);
  watchdog.join();

  path.ReturnAllCredits();
  const auto cleaned = path.Snapshot();
  success &= CheckEqual(cleaned.logical_retained_bytes, std::uint64_t{0},
                        "cleanup logical retained baseline");
  success &= CheckEqual(cleaned.physical_retained_bytes, std::uint64_t{0},
                        "cleanup physical retained bytes");
  success &= CheckEqual(cleaned.topology.retained_records, std::uint64_t{0},
                        "cleanup topology retained Records");
  phase.store(Phase::kComplete, std::memory_order_release);
  return success;
}

}  // namespace

int main() {
  try {
    return RunStress() ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "composed producer stress failed with exception: " << error.what() << '\n';
    return 1;
  }
}
