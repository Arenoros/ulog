#include "prototypes/composed/composed_producer_kernel.hpp"

#include <atomic>
#include <barrier>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "prototypes/record_storage/record_storage.hpp"
#include "support/workload_harness.hpp"

namespace {

namespace composed = ulog::benchmark_support::composed;
namespace ingress = ulog::benchmark_support::ingress;
namespace storage = ulog::benchmark_support::record_storage;

using Writer = storage::ContiguousRecordSlot::Writer;

int failure_count = 0;

void Check(bool condition, std::string_view test, std::string_view message) {
  if (!condition) {
    ++failure_count;
    std::cerr << test << ": " << message << '\n';
  }
}

template <typename Value>
void CheckEqual(const Value& actual, const Value& expected, std::string_view test,
                std::string_view field) {
  if (actual != expected) {
    ++failure_count;
    std::cerr << test << ": " << field << " differs\n";
  }
}

[[nodiscard]] std::size_t BenchmarkCharge(std::size_t message_bytes) noexcept {
  return static_cast<std::size_t>(
      storage::DescribeRecord<storage::ContiguousPolicy>(message_bytes).accounting_charge_bytes);
}

[[nodiscard]] bool WriteBenchmarkFields(Writer& writer, std::string_view string_key = "kind",
                                        std::string_view string_value = "benchmark") noexcept {
  return storage::AddBenchmarkFields(writer, string_key, string_value);
}

template <std::size_t Capacity>
[[nodiscard]] auto ProduceBenchmark(composed::ComposedProducerPath<Capacity>& path,
                                    std::size_t producer_index, std::string_view message,
                                    std::uint64_t& message_calls, std::uint64_t& context_calls,
                                    bool& writer_success) {
  return path.TryProduce(
      producer_index, composed::RecordPlan::Benchmark(message.size()),
      [&](Writer& writer) noexcept {
        ++message_calls;
        const auto written = writer.Append(message);
        writer_success =
            writer_success && written.stored_bytes == message.size() && !written.truncated;
      },
      [&](Writer& writer) noexcept {
        ++context_calls;
        writer_success = WriteBenchmarkFields(writer) && writer_success;
      });
}

void CheckBenchmarkRecord(const storage::RecordView& record, std::string_view expected_message,
                          std::string_view test) {
  Check(record.message().Equals(expected_message), test, "consumed message differs");
  Check(record.source_path().Equals(storage::kBenchmarkSourcePath), test,
        "consumed source path differs");
  Check(record.source_function().Equals(storage::kBenchmarkSourceFunction), test,
        "consumed source function differs");
  CheckEqual(record.source_line(), storage::kBenchmarkSourceLine, test, "source line");
  CheckEqual(record.event_timestamp(), storage::kBenchmarkEventTimestamp, test, "event timestamp");
  CheckEqual(record.field_count(), storage::kBenchmarkFieldCount, test, "field count");

  const auto string_field = record.FieldAt(0);
  const auto signed_field = record.FieldAt(1);
  const auto unsigned_field = record.FieldAt(2);
  const auto double_field = record.FieldAt(3);
  const auto bool_field = record.FieldAt(4);
  const auto null_field = record.FieldAt(5);
  Check(string_field && string_field->key().Equals(storage::kBenchmarkStringFieldKey) &&
            string_field->AsString() &&
            string_field->AsString()->Equals(storage::kBenchmarkStringFieldValue),
        test, "owned string field differs");
  Check(signed_field && signed_field->key().Equals(storage::kBenchmarkInt64FieldKey) &&
            signed_field->AsInt64() == std::int64_t{-7},
        test, "signed field differs");
  Check(unsigned_field && unsigned_field->key().Equals(storage::kBenchmarkUInt64FieldKey) &&
            unsigned_field->AsUInt64() == std::uint64_t{42},
        test, "unsigned field differs");
  Check(double_field && double_field->key().Equals(storage::kBenchmarkDoubleFieldKey) &&
            double_field->AsDouble() == 1.25,
        test, "double field differs");
  Check(bool_field && bool_field->key().Equals(storage::kBenchmarkBoolFieldKey) &&
            bool_field->AsBool() == true,
        test, "bool field differs");
  Check(null_field && null_field->key().Equals(storage::kBenchmarkNullFieldKey) &&
            null_field->IsNull(),
        test, "null field differs");
}

void TestBudgetRejectionPrecedesCallerEvaluation() {
  constexpr std::string_view kTest = "composed budget rejection is lazy";
  const std::string message(64, 'b');
  const std::size_t charge = BenchmarkCharge(message.size());
  composed::ComposedProducerPath<> path{charge, 0, 1};

  std::uint64_t accepted_message_calls = 0;
  std::uint64_t accepted_context_calls = 0;
  bool writer_success = true;
  const auto accepted = ProduceBenchmark(path, 0, message, accepted_message_calls,
                                         accepted_context_calls, writer_success);
  Check(accepted.status == composed::ProduceStatus::kAccepted, kTest,
        "fixture Record was not accepted");
  Check(accepted.admission_sequence == 0, kTest,
        "first accepted Record did not receive sequence zero");
  Check(writer_success, kTest, "fixture writer did not finish within its plan");

  const auto before_rejection = path.Snapshot();
  std::uint64_t rejected_message_calls = 0;
  std::uint64_t rejected_context_calls = 0;
  bool rejected_writer_success = true;
  const auto rejected = ProduceBenchmark(path, 0, message, rejected_message_calls,
                                         rejected_context_calls, rejected_writer_success);
  const auto after_rejection = path.Snapshot();

  Check(rejected.status == composed::ProduceStatus::kBudgetRejected, kTest,
        "full byte budget did not return a budget rejection");
  Check(!rejected.admission_sequence, kTest, "budget rejection consumed an admission sequence");
  CheckEqual(rejected_message_calls, std::uint64_t{0}, kTest,
             "budget rejection evaluated the caller message callback");
  CheckEqual(rejected_context_calls, std::uint64_t{0}, kTest,
             "budget rejection evaluated the context callback");
  CheckEqual(after_rejection.logical_retained_bytes, before_rejection.logical_retained_bytes, kTest,
             "logical retained bytes after rejection");
  CheckEqual(after_rejection.physical_retained_bytes, before_rejection.physical_retained_bytes,
             kTest, "physical retained bytes after rejection");
  CheckEqual(after_rejection.topology.retained_records, before_rejection.topology.retained_records,
             kTest, "retained topology Records after rejection");
  CheckEqual(after_rejection.message_callback_count, std::uint64_t{1}, kTest,
             "kernel message callback accounting");
  CheckEqual(after_rejection.context_callback_count, std::uint64_t{1}, kTest,
             "kernel context callback accounting");

  bool observed = false;
  const auto consumed =
      path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
        observed = true;
        CheckEqual(sequence, std::uint64_t{0}, kTest, "consumed sequence");
        CheckBenchmarkRecord(record, message, kTest);
      });
  Check(consumed == ingress::ConsumeStatus::kRecord && observed, kTest,
        "accepted Record was not consumed");
  path.ReturnAllCredits();
  const auto cleaned = path.Snapshot();
  CheckEqual(cleaned.logical_retained_bytes, std::uint64_t{0}, kTest,
             "logical retained bytes after cleanup");
  CheckEqual(cleaned.physical_retained_bytes, std::uint64_t{0}, kTest,
             "physical retained bytes after cleanup");
}

void TestLaneFullRejectionPrecedesCallerEvaluation() {
  constexpr std::string_view kTest = "composed lane-full rejection is lazy";
  constexpr std::string_view kMessage = "lane!!";
  const std::size_t charge = BenchmarkCharge(kMessage.size());
  composed::ComposedProducerPath<2> path{4U * charge, 0, 1};
  bool writer_success = true;

  for (std::uint64_t sequence = 0; sequence < 2; ++sequence) {
    std::uint64_t message_calls = 0;
    std::uint64_t context_calls = 0;
    const auto result =
        ProduceBenchmark(path, 0, kMessage, message_calls, context_calls, writer_success);
    Check(result.status == composed::ProduceStatus::kAccepted, kTest,
          "lane fixture Record was not accepted");
    Check(result.admission_sequence == sequence, kTest, "lane fixture sequence differs");
  }
  Check(writer_success, kTest, "lane fixture writer did not finish within its plan");

  const auto before_rejection = path.Snapshot();
  std::uint64_t message_calls = 0;
  std::uint64_t context_calls = 0;
  bool rejected_writer_success = true;
  const auto rejected =
      ProduceBenchmark(path, 0, kMessage, message_calls, context_calls, rejected_writer_success);
  const auto after_rejection = path.Snapshot();
  Check(rejected.status == composed::ProduceStatus::kIngressRejected, kTest,
        "full producer lane did not return an ingress rejection");
  Check(!rejected.admission_sequence, kTest, "lane-full rejection consumed an admission sequence");
  CheckEqual(message_calls, std::uint64_t{0}, kTest,
             "lane-full rejection evaluated the caller message callback");
  CheckEqual(context_calls, std::uint64_t{0}, kTest,
             "lane-full rejection evaluated the context callback");
  CheckEqual(after_rejection.physical_retained_bytes, before_rejection.physical_retained_bytes,
             kTest, "lane-full rejection changed physical retained bytes");
  CheckEqual(after_rejection.topology.retained_records, std::uint64_t{2}, kTest,
             "lane-full rejection changed retained Record count");

  for (std::uint64_t sequence = 0; sequence < 2; ++sequence) {
    bool observed = false;
    const auto consumed = path.TryConsume(
        [&](std::uint64_t actual_sequence, const storage::RecordView& record) noexcept {
          observed = true;
          CheckEqual(actual_sequence, sequence, kTest, "lane drain sequence");
          CheckBenchmarkRecord(record, kMessage, kTest);
        });
    Check(consumed == ingress::ConsumeStatus::kRecord && observed, kTest,
          "lane fixture Record was not consumed");
  }
  path.ReturnAllCredits();
}

void TestAcceptedRecordOwnsInputsAndPreservesFifo() {
  constexpr std::string_view kTest = "composed accepted Record ownership and FIFO";
  std::string caller_message = "owned-message";
  std::string caller_key = "kind";
  std::string caller_value = "benchmark";
  const std::string expected_message = caller_message;
  const std::string expected_key = caller_key;
  const std::string expected_value = caller_value;
  const std::size_t charge = BenchmarkCharge(caller_message.size());
  composed::ComposedProducerPath<4> path{4U * charge, 0, 2};
  bool writer_success = true;
  std::uint64_t message_calls = 0;
  std::uint64_t context_calls = 0;

  const auto first = path.TryProduce(
      0, composed::RecordPlan::Benchmark(caller_message.size()),
      [&](Writer& writer) noexcept {
        ++message_calls;
        const auto written = writer.Append(std::string_view{caller_message});
        writer_success =
            writer_success && written.stored_bytes == caller_message.size() && !written.truncated;
      },
      [&](Writer& writer) noexcept {
        ++context_calls;
        writer_success = WriteBenchmarkFields(writer, caller_key, caller_value) && writer_success;
      });
  Check(first.status == composed::ProduceStatus::kAccepted && first.admission_sequence == 0, kTest,
        "first owned Record was not published as sequence zero");

  caller_message.assign(caller_message.size(), 'x');
  caller_key.assign(caller_key.size(), 'x');
  caller_value.assign(caller_value.size(), 'x');

  constexpr std::string_view kSecondMessage = "next-record!";
  std::uint64_t second_message_calls = 0;
  std::uint64_t second_context_calls = 0;
  const auto second = ProduceBenchmark(path, 1, kSecondMessage, second_message_calls,
                                       second_context_calls, writer_success);
  Check(second.status == composed::ProduceStatus::kAccepted && second.admission_sequence == 1,
        kTest, "second owned Record was not published as sequence one");
  Check(writer_success, kTest, "accepted writer did not finish within its reservation");

  bool first_observed = false;
  const auto first_consumed =
      path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
        first_observed = true;
        CheckEqual(sequence, std::uint64_t{0}, kTest, "first FIFO sequence");
        Check(record.message().Equals(expected_message), kTest,
              "accepted Record retained a caller-owned message view");
        const auto string_field = record.FieldAt(0);
        Check(string_field && string_field->key().Equals(expected_key) &&
                  string_field->AsString() && string_field->AsString()->Equals(expected_value),
              kTest, "accepted Record retained caller-owned field storage");
        const auto during_callback = path.Snapshot();
        Check(during_callback.logical_retained_bytes != 0 &&
                  during_callback.topology.retained_records == 2,
              kTest, "consumer released ownership before its callback returned");
      });
  Check(first_consumed == ingress::ConsumeStatus::kRecord && first_observed, kTest,
        "first FIFO Record was not consumed");

  bool second_observed = false;
  const auto second_consumed =
      path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
        second_observed = true;
        CheckEqual(sequence, std::uint64_t{1}, kTest, "second FIFO sequence");
        CheckBenchmarkRecord(record, kSecondMessage, kTest);
      });
  Check(second_consumed == ingress::ConsumeStatus::kRecord && second_observed, kTest,
        "second FIFO Record was not consumed");

  const auto drained = path.Snapshot();
  CheckEqual(drained.logical_retained_bytes, std::uint64_t{0}, kTest,
             "drained path retained logical bytes");
  CheckEqual(drained.consumed_records, std::uint64_t{2}, kTest, "consumed Record count");
  CheckEqual(drained.fifo_error_count, std::uint64_t{0}, kTest, "FIFO error count");
  CheckEqual(drained.record_validation_error_count, std::uint64_t{0}, kTest,
             "Record validation error count");
  CheckEqual(drained.publication_error_count, std::uint64_t{0}, kTest, "publication error count");
  path.ReturnAllCredits();
}

void TestAdmissionSequenceIsAssignedOnlyAtPublication() {
  constexpr std::string_view kTest = "composed sequence starts at publication";
  constexpr std::string_view kSlowMessage = "slow";
  constexpr std::string_view kFastMessage = "fast";
  const std::size_t charge = BenchmarkCharge(kSlowMessage.size());
  composed::ComposedProducerPath<4> path{4U * charge, 0, 2};
  std::barrier callback_entered{2};
  std::barrier release_callback{2};
  std::atomic<composed::ProduceStatus> slow_status{composed::ProduceStatus::kInvalid};
  std::atomic<std::uint64_t> slow_sequence{std::numeric_limits<std::uint64_t>::max()};
  std::atomic<bool> slow_writer_success{true};

  std::thread slow_producer{[&] {
    const auto result = path.TryProduce(
        0, composed::RecordPlan::Benchmark(kSlowMessage.size()),
        [&](Writer& writer) noexcept {
          callback_entered.arrive_and_wait();
          release_callback.arrive_and_wait();
          const auto written = writer.Append(kSlowMessage);
          slow_writer_success.store(
              written.stored_bytes == kSlowMessage.size() && !written.truncated,
              std::memory_order_relaxed);
        },
        [&](Writer& writer) noexcept {
          slow_writer_success.store(
              WriteBenchmarkFields(writer) && slow_writer_success.load(std::memory_order_relaxed),
              std::memory_order_relaxed);
        });
    slow_status.store(result.status, std::memory_order_relaxed);
    if (result.admission_sequence) {
      slow_sequence.store(*result.admission_sequence, std::memory_order_relaxed);
    }
  }};

  callback_entered.arrive_and_wait();
  bool premature_observer_call = false;
  const auto before_publication = path.TryConsume(
      [&](std::uint64_t, const storage::RecordView&) noexcept { premature_observer_call = true; });
  Check(before_publication == ingress::ConsumeStatus::kEmpty && !premature_observer_call, kTest,
        "reserved-but-unpublished writer became visible to the consumer");

  bool fast_writer_success = true;
  std::uint64_t fast_message_calls = 0;
  std::uint64_t fast_context_calls = 0;
  const auto fast = ProduceBenchmark(path, 1, kFastMessage, fast_message_calls, fast_context_calls,
                                     fast_writer_success);
  Check(fast.status == composed::ProduceStatus::kAccepted && fast.admission_sequence == 0, kTest,
        "later reservation did not publish first as sequence zero");
  bool fast_observed = false;
  const auto fast_consumed =
      path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
        fast_observed = true;
        CheckEqual(sequence, std::uint64_t{0}, kTest, "fast Record sequence");
        CheckBenchmarkRecord(record, kFastMessage, kTest);
      });
  Check(fast_consumed == ingress::ConsumeStatus::kRecord && fast_observed, kTest,
        "first published Record was not immediately consumable");

  release_callback.arrive_and_wait();
  slow_producer.join();
  Check(slow_status.load(std::memory_order_relaxed) == composed::ProduceStatus::kAccepted, kTest,
        "blocked producer was not accepted after its callback resumed");
  CheckEqual(slow_sequence.load(std::memory_order_relaxed), std::uint64_t{1}, kTest,
             "blocked producer sequence");
  Check(slow_writer_success.load(std::memory_order_relaxed) && fast_writer_success, kTest,
        "publication-order fixture writer failed");

  bool slow_observed = false;
  const auto slow_consumed =
      path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
        slow_observed = true;
        CheckEqual(sequence, std::uint64_t{1}, kTest, "slow Record sequence");
        CheckBenchmarkRecord(record, kSlowMessage, kTest);
      });
  Check(slow_consumed == ingress::ConsumeStatus::kRecord && slow_observed, kTest,
        "blocked producer Record was not consumed second");
  path.ReturnAllCredits();
}

void TestConsumerClaimPreventsSlotReuseAndOverwrite() {
  constexpr std::string_view kTest = "composed consumer claim holds slot ownership";
  constexpr std::string_view kFirstMessage = "first!";
  constexpr std::string_view kSecondMessage = "second";
  constexpr std::string_view kThirdMessage = "third!";
  const std::size_t charge = BenchmarkCharge(kFirstMessage.size());
  composed::ComposedProducerPath<2> path{4U * charge, 0, 1};
  bool writer_success = true;

  for (const std::string_view message : {kFirstMessage, kSecondMessage}) {
    std::uint64_t message_calls = 0;
    std::uint64_t context_calls = 0;
    const auto result =
        ProduceBenchmark(path, 0, message, message_calls, context_calls, writer_success);
    Check(result.status == composed::ProduceStatus::kAccepted, kTest,
          "consumer-claim fixture Record was not accepted");
  }

  std::barrier observer_entered{2};
  std::barrier release_observer{2};
  std::atomic<bool> observer_before_matches{false};
  std::atomic<bool> observer_after_matches{false};
  std::atomic<ingress::ConsumeStatus> consume_status{ingress::ConsumeStatus::kEmpty};
  std::thread consumer{[&] {
    const auto status =
        path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
          observer_before_matches.store(sequence == 0 && record.message().Equals(kFirstMessage),
                                        std::memory_order_relaxed);
          observer_entered.arrive_and_wait();
          release_observer.arrive_and_wait();
          observer_after_matches.store(sequence == 0 && record.message().Equals(kFirstMessage),
                                       std::memory_order_relaxed);
        });
    consume_status.store(status, std::memory_order_relaxed);
  }};

  observer_entered.arrive_and_wait();
  const auto before_reuse = path.Snapshot();
  std::uint64_t rejected_message_calls = 0;
  std::uint64_t rejected_context_calls = 0;
  bool rejected_writer_success = true;
  const auto rejected = ProduceBenchmark(path, 0, kThirdMessage, rejected_message_calls,
                                         rejected_context_calls, rejected_writer_success);
  const auto after_reuse = path.Snapshot();
  Check(rejected.status == composed::ProduceStatus::kIngressRejected, kTest,
        "producer reused a slot while its consumer callback was active");
  CheckEqual(rejected_message_calls, std::uint64_t{0}, kTest,
             "held consumer claim still evaluated the message callback");
  CheckEqual(rejected_context_calls, std::uint64_t{0}, kTest,
             "held consumer claim still evaluated the context callback");
  CheckEqual(after_reuse.physical_retained_bytes, before_reuse.physical_retained_bytes, kTest,
             "failed reuse changed physical retained bytes");
  CheckEqual(after_reuse.topology.retained_records, std::uint64_t{2}, kTest,
             "consumer claim released topology retention before callback completion");

  release_observer.arrive_and_wait();
  consumer.join();
  Check(consume_status.load(std::memory_order_relaxed) == ingress::ConsumeStatus::kRecord, kTest,
        "blocked consumer did not complete with a Record");
  Check(observer_before_matches.load(std::memory_order_relaxed), kTest,
        "consumer initially observed a corrupted Record");
  Check(observer_after_matches.load(std::memory_order_relaxed), kTest,
        "producer overwrote the Record during its consumer callback");

  std::uint64_t retry_message_calls = 0;
  std::uint64_t retry_context_calls = 0;
  const auto retry = ProduceBenchmark(path, 0, kThirdMessage, retry_message_calls,
                                      retry_context_calls, writer_success);
  Check(retry.status == composed::ProduceStatus::kAccepted && retry.admission_sequence == 2, kTest,
        "slot did not become reusable after the consumer callback returned");

  for (const auto expected :
       {std::pair{std::uint64_t{1}, kSecondMessage}, std::pair{std::uint64_t{2}, kThirdMessage}}) {
    bool observed = false;
    const auto status =
        path.TryConsume([&](std::uint64_t sequence, const storage::RecordView& record) noexcept {
          observed = true;
          CheckEqual(sequence, expected.first, kTest, "post-claim FIFO sequence");
          CheckBenchmarkRecord(record, expected.second, kTest);
        });
    Check(status == ingress::ConsumeStatus::kRecord && observed, kTest,
          "post-claim Record was not consumed");
  }
  Check(writer_success, kTest, "consumer-claim fixture writer failed");
  path.ReturnAllCredits();
}

void TestWriterFailurePreventsPublication() {
  constexpr std::string_view kTest = "composed writer failure";
  constexpr std::string_view kMessage = "writer-failure";
  const auto plan = composed::RecordPlan::Benchmark(kMessage.size());
  const auto capacity = static_cast<std::size_t>(plan.maximum_footprint().accounting_charge_bytes);
  composed::ComposedProducerPath<1> path{capacity, 0, 1};

  std::uint64_t message_calls = 0;
  std::uint64_t context_calls = 0;
  const auto failed = path.TryProduce(
      0, plan,
      [&](Writer& writer) noexcept {
        ++message_calls;
        static_cast<void>(writer.Append(kMessage));
        return false;
      },
      [&](Writer& writer) noexcept {
        ++context_calls;
        return storage::AddBenchmarkFields(writer);
      });
  const auto rejected = path.Snapshot();
  Check(failed.status == composed::ProduceStatus::kInvalid && !failed.admission_sequence, kTest,
        "failed writer callback published a Record");
  Check(message_calls == 1 && context_calls == 1, kTest,
        "accepted reservation did not evaluate both callbacks exactly once");
  Check(rejected.attempted_records == 1 && rejected.accepted_records == 0 &&
            rejected.rejected_records == 1 && rejected.published_records == 0 &&
            rejected.record_validation_error_count == 1,
        kTest, "writer failure accounting differs");
  Check(rejected.logical_retained_bytes == 0 && rejected.topology.attempted_records == 0, kTest,
        "writer failure retained logical ownership or reached publication");

  bool writer_success = true;
  std::uint64_t retry_message_calls = 0;
  std::uint64_t retry_context_calls = 0;
  const auto retry =
      ProduceBenchmark(path, 0, kMessage, retry_message_calls, retry_context_calls, writer_success);
  Check(retry.status == composed::ProduceStatus::kAccepted && retry.admission_sequence == 0, kTest,
        "writer failure did not restore lane and byte admission");
  const auto consumed = path.TryConsume([](std::uint64_t, const storage::RecordView&) noexcept {});
  Check(consumed == ingress::ConsumeStatus::kRecord && writer_success, kTest,
        "post-failure retry did not publish and consume normally");
  path.ReturnAllCredits();
}

void TestComposedKernelRunsTheCommonWorkload() {
  constexpr std::string_view kTest = "composed common workload";
  const ulog::benchmark_support::WorkloadCase workload{
      .producer_count = 4,
      .record_size_bytes = 64,
      .occupancy = ulog::benchmark_support::Occupancy::kEmpty,
      .capacity_bytes = ulog::benchmark_support::kPayloadCapacityBytes,
      .warmup_rounds = 1,
      .measured_rounds = 2,
      .repetition = 0,
  };
  composed::ComposedProducerKernel kernel;
  const auto result = ulog::benchmark_support::RunWorkload(workload, kernel);
  const auto topology = kernel.MeasurementTopologySnapshot();

  Check(composed::ComposedProducerKernel::Name() == "composed-producer", kTest,
        "common workload kernel name differs");
  Check(result.attempted_records == 8 && result.accepted_records == 8 &&
            result.rejected_records == 0 && result.maximum_accepted_per_round == 4,
        kTest, "common workload admission counts differ");
  Check(kernel.message_callback_count() == result.accepted_records &&
            kernel.context_callback_count() == result.accepted_records,
        kTest, "callback accounting does not equal accepted Records");
  Check(topology.attempted_records == result.accepted_records &&
            topology.enqueued_records == result.accepted_records &&
            topology.dequeued_records == result.accepted_records &&
            topology.rejected_records == 0 && topology.retained_records == 0 &&
            topology.retained_serialized_bytes == 0 && topology.retained_charge_bytes == 0,
        kTest, "common workload topology did not publish and consume every accepted Record");
  Check(result.allocation_count == 0 && result.allocation_failure_count == 0 &&
            result.accounting_error_count == 0 && result.retained_bound_error_count == 0,
        kTest, "common workload allocation, accounting, or retained-bound check failed");
  Check(result.logical_retained_final_bytes == result.logical_retained_initial_bytes &&
            result.physical_retained_final_bytes == result.physical_retained_initial_bytes,
        kTest, "common workload did not restore its measured retained baseline");
  Check(result.logical_retained_high_water_bytes <= result.logical_retained_limit_bytes &&
            result.physical_retained_high_water_bytes <= result.physical_retained_limit_bytes,
        kTest, "common workload retained high-water exceeded its declared limit");
  Check(kernel.fifo_error_count() == 0 && kernel.record_validation_error_count() == 0 &&
            kernel.publication_error_count() == 0 && kernel.lifecycle_error_count() == 0,
        kTest, "common workload FIFO, Record, publication, or lifecycle validation failed");
}

}  // namespace

int main() {
  try {
    TestBudgetRejectionPrecedesCallerEvaluation();
    TestLaneFullRejectionPrecedesCallerEvaluation();
    TestAcceptedRecordOwnsInputsAndPreservesFifo();
    TestAdmissionSequenceIsAssignedOnlyAtPublication();
    TestConsumerClaimPreventsSlotReuseAndOverwrite();
    TestWriterFailurePreventsPublication();
    TestComposedKernelRunsTheCommonWorkload();
    return failure_count == 0 ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << "composed producer kernel test failed with exception: " << error.what() << '\n';
    return 1;
  }
}
