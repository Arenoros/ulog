#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <latch>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>

#include "producer/producer_kernel.hpp"
#include "producer/record_storage.hpp"

namespace {

namespace producer = ulog::detail::producer;

constexpr std::size_t kProducerCount = 32;
constexpr std::size_t kIngressCellCount = 64;
constexpr std::size_t kInitialAcceptedPerProducer = 2;
constexpr std::size_t kRandomAttemptsPerProducer = 2'048;
constexpr std::uint64_t kTotalAttempts =
    kProducerCount * (kInitialAcceptedPerProducer + 1U + kRandomAttemptsPerProducer);
constexpr std::size_t kIdentityBytes = 12;
constexpr std::size_t kMaximumStressMessageBytes = 320;
constexpr std::size_t kMaximumRecordBytes = 256;
constexpr auto kDeadline = std::chrono::seconds{5};

struct Clock final {
  static producer::EventTimestamp Now(void* context) noexcept {
    return static_cast<producer::EventTimestamp>(
        static_cast<std::atomic<std::uint64_t>*>(context)->fetch_add(1, std::memory_order_relaxed));
  }
};

struct Counters final {
  std::atomic<std::uint64_t> message_evaluations{0};
  std::atomic<std::uint64_t> consumed_records{0};
  std::atomic<std::uint64_t> content_errors{0};
  std::atomic<std::uint64_t> sequence_errors{0};
  std::atomic<std::uint64_t> registration_errors{0};
  std::atomic<std::uint64_t> snapshot_errors{0};
  std::atomic<std::uint64_t> expected_abandoned_builds{0};
  std::atomic<std::uint64_t> expected_truncated_records{0};
  std::uint64_t random_records_consumed{0};
  std::uint64_t next_sequence{0};
  std::array<std::uint8_t, kProducerCount * kRandomAttemptsPerProducer> seen_random_records{};
};

enum class RecordMode : std::uint8_t { kNormal, kStructured, kTruncated, kAbandoned };

struct RecordIdentity final {
  RecordMode mode{RecordMode::kNormal};
  std::size_t producer_index{0};
  std::size_t attempt_index{0};
};

[[nodiscard]] constexpr char SerializeRecordMode(RecordMode mode) noexcept {
  switch (mode) {
    case RecordMode::kNormal:
      return 'N';
    case RecordMode::kStructured:
      return 'S';
    case RecordMode::kTruncated:
      return 'T';
    case RecordMode::kAbandoned:
      return 'A';
  }
  return '\0';
}

[[nodiscard]] constexpr std::optional<RecordMode> ParseRecordMode(char value) noexcept {
  switch (value) {
    case 'N':
      return RecordMode::kNormal;
    case 'S':
      return RecordMode::kStructured;
    case 'T':
      return RecordMode::kTruncated;
    case 'A':
      return RecordMode::kAbandoned;
    default:
      return std::nullopt;
  }
}

[[nodiscard]] constexpr char HexDigit(std::uint8_t value) noexcept {
  return value < 10U ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10U);
}

[[nodiscard]] constexpr std::optional<std::uint8_t> ParseHexDigit(char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<std::uint8_t>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<std::uint8_t>(value - 'a' + 10);
  }
  return std::nullopt;
}

void WriteIdentity(std::span<char> output, RecordIdentity identity) noexcept {
  output[0] = SerializeRecordMode(identity.mode);
  output[1] = HexDigit(static_cast<std::uint8_t>((identity.producer_index >> 4U) & 0x0fU));
  output[2] = HexDigit(static_cast<std::uint8_t>(identity.producer_index & 0x0fU));
  output[3] = ':';
  for (std::size_t digit = 0; digit < 8; ++digit) {
    const std::size_t shift = (7U - digit) * 4U;
    output[4U + digit] =
        HexDigit(static_cast<std::uint8_t>((identity.attempt_index >> shift) & 0x0fU));
  }
}

[[nodiscard]] std::optional<RecordIdentity> ParseIdentity(std::string_view message) noexcept {
  if (message.size() < kIdentityBytes || message[3] != ':') {
    return std::nullopt;
  }
  const auto mode = ParseRecordMode(message[0]);
  const auto producer_high = ParseHexDigit(message[1]);
  const auto producer_low = ParseHexDigit(message[2]);
  if (!mode || *mode == RecordMode::kAbandoned || !producer_high || !producer_low) {
    return std::nullopt;
  }
  std::size_t attempt_index = 0;
  for (std::size_t digit = 0; digit < 8; ++digit) {
    const auto parsed = ParseHexDigit(message[4U + digit]);
    if (!parsed) {
      return std::nullopt;
    }
    attempt_index = (attempt_index << 4U) | *parsed;
  }
  return RecordIdentity{.mode = *mode,
                        .producer_index = static_cast<std::size_t>(*producer_high) * 16U +
                                          static_cast<std::size_t>(*producer_low),
                        .attempt_index = attempt_index};
}

void Consume(void* context, std::uint64_t sequence, const producer::RecordView& record) noexcept {
  auto& counters = *static_cast<Counters*>(context);
  if (sequence != counters.next_sequence) {
    counters.sequence_errors.fetch_add(1, std::memory_order_relaxed);
  }
  ++counters.next_sequence;
  bool valid = record.level() == ulog::Level::kInfo;
  if (record.message() == "stress") {
    valid = valid && !record.truncated();
  } else {
    const auto identity = ParseIdentity(record.message());
    valid = valid && identity && identity->producer_index < kProducerCount &&
            identity->attempt_index < kRandomAttemptsPerProducer &&
            record.truncated() == (identity && identity->mode == RecordMode::kTruncated);
    if (identity && identity->producer_index < kProducerCount &&
        identity->attempt_index < kRandomAttemptsPerProducer) {
      const std::size_t identity_index =
          identity->producer_index * kRandomAttemptsPerProducer + identity->attempt_index;
      valid = valid && counters.seen_random_records[identity_index] == 0U;
      counters.seen_random_records[identity_index] = 1U;
      ++counters.random_records_consumed;

      bool expected_field = identity->mode != RecordMode::kNormal;
      bool found_field = false;
      for (std::size_t index = 0; index < record.field_count(); ++index) {
        const auto field = record.FieldAt(index);
        found_field =
            found_field ||
            (field && identity->mode == RecordMode::kStructured && field->key() == "producer" &&
             field->AsUInt64() == identity->producer_index) ||
            (field && identity->mode == RecordMode::kTruncated &&
             field->key() == "ulog.truncated" && field->AsBool().value_or(false));
      }
      valid = valid && expected_field == found_field;
    }
  }
  if (!valid) {
    counters.content_errors.fetch_add(1, std::memory_order_relaxed);
  }
  counters.consumed_records.fetch_add(1, std::memory_order_relaxed);
}

struct NativeBuildContext final {
  Counters* counters{nullptr};
  RecordIdentity identity{};
};

struct NativeBuild final {
  static producer::BuildStatus Invoke(void* context, producer::RecordAppender& writer) {
    auto& build = *static_cast<NativeBuildContext*>(context);
    build.counters->message_evaluations.fetch_add(1, std::memory_order_relaxed);
    std::array<char, kIdentityBytes> identity{};
    WriteIdentity(identity, build.identity);
    const auto text = writer.Append(std::string_view{identity.data(), identity.size()});
    if (build.identity.mode == RecordMode::kAbandoned) {
      build.counters->expected_abandoned_builds.fetch_add(1, std::memory_order_relaxed);
      return producer::BuildStatus::kInvalid;
    }
    const auto number = writer.Append(static_cast<std::uint64_t>(build.identity.attempt_index));
    auto output = writer.FormatOutput();
    *output++ = ':';
    *output++ = 'o';
    *output++ = 'k';
    const bool field =
        writer.AddField("producer", static_cast<std::uint64_t>(build.identity.producer_index));
    return !text.truncated && !number.truncated && field ? producer::BuildStatus::kComplete
                                                         : producer::BuildStatus::kInvalid;
  }
};

[[nodiscard]] std::uint64_t NextRandom(std::uint64_t& state) noexcept {
  state ^= state << 13U;
  state ^= state >> 7U;
  state ^= state << 17U;
  return state;
}

[[nodiscard]] bool RunStress() {
  std::atomic<std::uint64_t> clock_calls{0};
  producer::ProducerKernel kernel{
      producer::KernelConfig{.threshold = ulog::Level::kTrace,
                             .payload_capacity_bytes = 16'384,
                             .maximum_record_bytes = kMaximumRecordBytes,
                             .producer_slots = kProducerCount,
                             .ingress_cells = kIngressCellCount},
      producer::EventClock{&clock_calls, &Clock::Now}};
  const ulog::Logger logger = kernel.GetLogger();
  Counters counters;
  std::latch start{kProducerCount + 1};
  std::latch initial_complete{kProducerCount + 1};
  std::latch randomized_start{kProducerCount + 1};
  std::atomic<std::size_t> producers_done{0};
  std::atomic<bool> sampling_done{false};
  std::array<std::thread, kProducerCount> producers;

  std::mutex watchdog_mutex;
  std::condition_variable watchdog_cv;
  bool watchdog_done = false;
  std::thread watchdog{[&] {
    std::unique_lock lock{watchdog_mutex};
    if (!watchdog_cv.wait_for(lock, kDeadline, [&] { return watchdog_done; })) {
      std::cerr << "producer kernel stress exceeded its five-second watchdog\n";
      std::abort();
    }
  }};

  for (std::size_t producer_index = 0; producer_index < kProducerCount; ++producer_index) {
    producers[producer_index] = std::thread{[&, producer_index] {
      auto registration = kernel.TryRegisterProducer();
      if (!registration) {
        counters.registration_errors.fetch_add(1, std::memory_order_relaxed);
        start.arrive_and_wait();
        initial_complete.arrive_and_wait();
        randomized_start.arrive_and_wait();
        producers_done.fetch_add(1, std::memory_order_release);
        return;
      }

      start.arrive_and_wait();
      for (std::size_t attempt = 0; attempt < kInitialAcceptedPerProducer + 1U; ++attempt) {
        logger.Log<ulog::Level::kInfo>(
            ulog::SourceLocation::Custom("stress.cpp", "Producer", 91), [&]() -> std::string_view {
              counters.message_evaluations.fetch_add(1, std::memory_order_relaxed);
              return "stress";
            });
      }
      initial_complete.arrive_and_wait();
      randomized_start.arrive_and_wait();

      std::uint64_t random_state =
          0x9e3779b97f4a7c15ULL ^ (static_cast<std::uint64_t>(producer_index) + 1U);
      for (std::size_t attempt = 0; attempt < kRandomAttemptsPerProducer; ++attempt) {
        const std::uint64_t random = NextRandom(random_state);
        if ((random & 0x1fU) == 0U) {
          std::this_thread::yield();
        }
        const auto source = ulog::SourceLocation::Custom("stress.cpp", "Producer", 91);
        const std::uint64_t mode = (random >> 5U) & 0x0fU;
        if (mode <= 1U) {
          NativeBuildContext build{
              .counters = &counters,
              .identity = {.mode = mode == 0U ? RecordMode::kAbandoned : RecordMode::kStructured,
                           .producer_index = producer_index,
                           .attempt_index = attempt}};
          static_cast<void>(
              kernel.TryPublish(registration, ulog::Level::kInfo, source,
                                producer::BuildOperation{&build, &NativeBuild::Invoke}));
          continue;
        }

        std::array<char, kMaximumStressMessageBytes> message{};
        logger.Log<ulog::Level::kInfo>(source, [&]() -> std::string_view {
          counters.message_evaluations.fetch_add(1, std::memory_order_relaxed);
          message.fill('x');
          const bool oversized = mode <= 3U;
          WriteIdentity(message, {.mode = oversized ? RecordMode::kTruncated : RecordMode::kNormal,
                                  .producer_index = producer_index,
                                  .attempt_index = attempt});
          if (oversized) {
            counters.expected_truncated_records.fetch_add(1, std::memory_order_relaxed);
            return {message.data(), message.size()};
          }
          const std::size_t message_size =
              kIdentityBytes + static_cast<std::size_t>((random >> 9U) % 140U);
          return {message.data(), message_size};
        });
      }
      producers_done.fetch_add(1, std::memory_order_release);
    }};
  }

  start.arrive_and_wait();
  initial_complete.arrive_and_wait();
  const auto initial = kernel.GetSnapshot();
  constexpr std::size_t kInitialSerializedBytes =
      producer::record::kSerializedRecordMetadataBytes + std::string_view{"stress.cpp"}.size() +
      std::string_view{"Producer"}.size() + std::string_view{"stress"}.size();
  constexpr std::size_t kInitialChargeBytes =
      ((kInitialSerializedBytes + producer::kAccountingQuantumBytes - 1U) /
       producer::kAccountingQuantumBytes) *
      producer::kAccountingQuantumBytes;
  constexpr std::size_t kExpectedInitialLogicalBytes =
      kProducerCount * kInitialAcceptedPerProducer * kInitialSerializedBytes;
  constexpr std::size_t kExpectedInitialPhysicalBytes =
      kProducerCount *
      (kMaximumRecordBytes + (kInitialAcceptedPerProducer - 1U) * kInitialChargeBytes);
  bool success =
      initial.attempted_records == kProducerCount * 3U &&
      initial.accepted_records == kProducerCount * kInitialAcceptedPerProducer &&
      initial.rejected_lane_full == kProducerCount && initial.rejected_budget == 0U &&
      initial.retained_records == kProducerCount * kInitialAcceptedPerProducer &&
      initial.logical_retained_bytes == kExpectedInitialLogicalBytes &&
      initial.physical_retained_bytes == kExpectedInitialPhysicalBytes &&
      initial.fixed_backing_bytes == kIngressCellCount * sizeof(producer::record::RecordSlot) &&
      initial.accounting_sample_consistent;

  std::thread consumer{[&] {
    for (;;) {
      const auto status = kernel.TryConsume(&counters, &Consume);
      if (status == producer::ConsumeStatus::kRecord) {
        continue;
      }
      if (producers_done.load(std::memory_order_acquire) == kProducerCount &&
          status == producer::ConsumeStatus::kEmpty) {
        return;
      }
      std::this_thread::yield();
    }
  }};
  std::thread sampler{[&] {
    while (!sampling_done.load(std::memory_order_acquire)) {
      const auto snapshot = kernel.GetSnapshot();
      if (snapshot.accepted_records > snapshot.attempted_records ||
          snapshot.logical_retained_bytes > snapshot.physical_retained_bytes ||
          snapshot.physical_retained_bytes > snapshot.payload_capacity_bytes) {
        counters.snapshot_errors.fetch_add(1, std::memory_order_relaxed);
      }
      std::this_thread::yield();
    }
  }};
  randomized_start.arrive_and_wait();

  for (auto& thread : producers) {
    thread.join();
  }
  consumer.join();
  sampling_done.store(true, std::memory_order_release);
  sampler.join();

  const auto final = kernel.GetSnapshot();
  const std::uint64_t rejected = final.rejected_no_producer + final.rejected_lane_full +
                                 final.rejected_budget + final.abandoned_builds +
                                 final.invalid_records;
  success = success && counters.registration_errors.load(std::memory_order_relaxed) == 0U &&
            final.attempted_records == kTotalAttempts &&
            final.attempted_records == final.accepted_records + rejected &&
            final.accepted_records == final.consumed_records &&
            final.accepted_records + final.abandoned_builds ==
                counters.message_evaluations.load(std::memory_order_relaxed) &&
            final.consumed_records == counters.consumed_records.load(std::memory_order_relaxed) &&
            counters.random_records_consumed ==
                final.accepted_records - kProducerCount * kInitialAcceptedPerProducer &&
            counters.expected_abandoned_builds.load(std::memory_order_relaxed) ==
                final.abandoned_builds &&
            counters.expected_truncated_records.load(std::memory_order_relaxed) ==
                final.truncated_records &&
            clock_calls.load(std::memory_order_relaxed) ==
                final.accepted_records + final.abandoned_builds &&
            counters.content_errors.load(std::memory_order_relaxed) == 0U &&
            counters.sequence_errors.load(std::memory_order_relaxed) == 0U &&
            counters.snapshot_errors.load(std::memory_order_relaxed) == 0U &&
            final.rejected_no_producer == 0U && final.invalid_records == 0U &&
            final.retained_records == 0U && final.logical_retained_bytes == 0U &&
            final.physical_retained_bytes == 0U &&
            final.logical_retained_bytes <= final.physical_retained_bytes &&
            final.physical_retained_bytes <= final.payload_capacity_bytes &&
            final.accounting_sample_consistent && final.active_producer_slots == 0U &&
            final.retiring_producer_slots == 0U;

  {
    std::lock_guard lock{watchdog_mutex};
    watchdog_done = true;
  }
  watchdog_cv.notify_one();
  watchdog.join();

  if (!success) {
    std::cerr << "producer kernel stress failed: attempted=" << final.attempted_records
              << " accepted=" << final.accepted_records << " consumed=" << final.consumed_records
              << " rejected=" << rejected << " lane=" << final.rejected_lane_full
              << " budget=" << final.rejected_budget
              << " physical=" << final.physical_retained_bytes
              << " logical=" << final.logical_retained_bytes << '\n';
  }
  return success;
}

}  // namespace

int main() noexcept {
  try {
    return RunStress() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "producer kernel stress failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("producer kernel stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
