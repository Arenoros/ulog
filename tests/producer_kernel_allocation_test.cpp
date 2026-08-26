#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>

#include "producer/producer_kernel.hpp"
#include "support/allocation_interposer.hpp"

namespace {

namespace allocation_tracking = ulog::benchmark_support::allocation_tracking;
namespace producer = ulog::detail::producer;

constexpr std::string_view kMessage = "allocation-free";
constexpr std::uint64_t kCycles = 1'024;
constexpr std::size_t kNearLimitMessageBytes = 16'000;
constexpr std::size_t kOversizedMessageBytes = 17'000;
constexpr ulog::SourceLocation kSource = ulog::SourceLocation::Custom("allocation.cpp", "Run", 1);

struct Clock final {
  static producer::EventTimestamp Now(void* context) noexcept {
    return ++*static_cast<producer::EventTimestamp*>(context);
  }
};

struct Counters final {
  std::uint64_t builder_calls{0};
  std::uint64_t consumer_calls{0};
  bool valid{true};
};

void Consume(void* context, std::uint64_t, const producer::RecordView& record) noexcept {
  auto& counters = *static_cast<Counters*>(context);
  ++counters.consumer_calls;
  counters.valid = counters.valid && record.message() == kMessage;
}

struct NativeBuild final {
  static producer::BuildStatus Invoke(void* context, producer::RecordAppender& writer) {
    auto& counters = *static_cast<Counters*>(context);
    ++counters.builder_calls;
    const auto text = writer.Append("allocation-");
    const auto number = writer.Append(std::uint64_t{7});
    const auto ratio = writer.Append(1.25);
    auto output = writer.FormatOutput();
    *output++ = ':';
    *output++ = 'o';
    *output++ = 'k';
    const bool field = writer.AddField("kind", kMessage);
    counters.valid =
        counters.valid && !text.truncated && !number.truncated && !ratio.truncated && field;
    return producer::BuildStatus::kComplete;
  }
};

void ConsumeNative(void* context, std::uint64_t, const producer::RecordView& record) noexcept {
  auto& counters = *static_cast<Counters*>(context);
  ++counters.consumer_calls;
  const auto field = record.FieldAt(0);
  counters.valid = counters.valid && record.message() == "allocation-71.25:ok" && field &&
                   field->key() == "kind" && field->AsString() == kMessage;
}

void ConsumeNearLimit(void* context, std::uint64_t, const producer::RecordView& record) noexcept {
  auto& counters = *static_cast<Counters*>(context);
  ++counters.consumer_calls;
  counters.valid =
      counters.valid && record.message().size() == kNearLimitMessageBytes && !record.truncated();
}

void ConsumeTruncated(void* context, std::uint64_t, const producer::RecordView& record) noexcept {
  auto& counters = *static_cast<Counters*>(context);
  ++counters.consumer_calls;
  bool has_truncated_field = false;
  for (std::size_t index = 0; index < record.field_count(); ++index) {
    const auto field = record.FieldAt(index);
    has_truncated_field = has_truncated_field || (field && field->key() == "ulog.truncated" &&
                                                  field->AsBool().value_or(false));
  }
  counters.valid = counters.valid && record.truncated() && has_truncated_field &&
                   record.message().size() < kOversizedMessageBytes;
}

[[nodiscard]] bool Run() {
  producer::EventTimestamp timestamp = 0;
  producer::ProducerKernel kernel{producer::KernelConfig{.threshold = ulog::Level::kTrace,
                                                         .payload_capacity_bytes = 32'768,
                                                         .maximum_record_bytes = 16'384,
                                                         .producer_slots = 1,
                                                         .ingress_cells = 2},
                                  producer::EventClock{&timestamp, &Clock::Now}};
  auto registration = kernel.TryRegisterProducer();
  if (!registration) {
    std::cerr << "producer allocation test: registration failed\n";
    return false;
  }
  const ulog::Logger logger = kernel.GetLogger();
  Counters counters;
  std::array<char, kNearLimitMessageBytes> near_limit_message{};
  std::array<char, kOversizedMessageBytes> oversized_message{};
  near_limit_message.fill('n');
  oversized_message.fill('t');

  logger.Log<ulog::Level::kInfo>(kSource, [] { return kMessage; });
  if (kernel.TryConsume(&counters, &Consume) != producer::ConsumeStatus::kRecord) {
    std::cerr << "producer allocation test: warm cycle failed\n";
    return false;
  }

  const std::uint64_t allocations_before =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  for (std::uint64_t cycle = 0; cycle < kCycles; ++cycle) {
    logger.Log<ulog::Level::kInfo>(kSource, [&]() {
      ++counters.builder_calls;
      return kMessage;
    });
    counters.valid = counters.valid &&
                     kernel.TryConsume(&counters, &Consume) == producer::ConsumeStatus::kRecord;

    const auto native =
        kernel.TryPublish(registration, ulog::Level::kDebug, kSource,
                          producer::BuildOperation{&counters, &NativeBuild::Invoke});
    counters.valid = counters.valid && native.outcome == producer::PublishOutcome::kAccepted;
    counters.valid = counters.valid && kernel.TryConsume(&counters, &ConsumeNative) ==
                                           producer::ConsumeStatus::kRecord;
  }

  kernel.SetLevel(ulog::Level::kWarning);
  const std::uint64_t filtered_builder_calls = counters.builder_calls;
  const auto filtered =
      kernel.TryPublish(registration, ulog::Level::kDebug, kSource,
                        producer::BuildOperation{&counters, &NativeBuild::Invoke});
  counters.valid = counters.valid && filtered.outcome == producer::PublishOutcome::kFiltered &&
                   counters.builder_calls == filtered_builder_calls;
  kernel.SetLevel(ulog::Level::kTrace);

  logger.Log<ulog::Level::kInfo>(kSource, [&]() -> std::string_view {
    ++counters.builder_calls;
    return {near_limit_message.data(), near_limit_message.size()};
  });
  counters.valid = counters.valid && kernel.TryConsume(&counters, &ConsumeNearLimit) ==
                                         producer::ConsumeStatus::kRecord;
  logger.Log<ulog::Level::kInfo>(kSource, [&]() -> std::string_view {
    ++counters.builder_calls;
    return {oversized_message.data(), oversized_message.size()};
  });
  counters.valid = counters.valid && kernel.TryConsume(&counters, &ConsumeTruncated) ==
                                         producer::ConsumeStatus::kRecord;

  logger.Log<ulog::Level::kInfo>(kSource, [] { return kMessage; });
  logger.Log<ulog::Level::kInfo>(kSource, [] { return kMessage; });
  std::uint64_t rejected_factory_calls = 0;
  logger.Log<ulog::Level::kInfo>(kSource, [&]() {
    ++rejected_factory_calls;
    return kMessage;
  });
  counters.valid = counters.valid && rejected_factory_calls == 0;
  counters.valid =
      counters.valid && kernel.TryConsume(&counters, &Consume) == producer::ConsumeStatus::kRecord;
  counters.valid =
      counters.valid && kernel.TryConsume(&counters, &Consume) == producer::ConsumeStatus::kRecord;

  const std::uint64_t allocations_after =
      allocation_tracking::allocation_count.load(std::memory_order_relaxed);
  const auto snapshot = kernel.GetSnapshot();
  const std::uint64_t expected_records = 1U + 2U * kCycles + 4U;
  const bool success =
      counters.valid && allocations_after == allocations_before &&
      counters.builder_calls == 2U * kCycles + 2U && counters.consumer_calls == expected_records &&
      snapshot.accepted_records == expected_records &&
      snapshot.consumed_records == expected_records && snapshot.rejected_lane_full == 1U &&
      snapshot.truncated_records == 1U && snapshot.retained_records == 0U &&
      snapshot.logical_retained_bytes == 0U && snapshot.accounting_sample_consistent;
  if (!success) {
    std::cerr << "producer allocation test: allocations="
              << (allocations_after - allocations_before) << " builders=" << counters.builder_calls
              << " consumers=" << counters.consumer_calls
              << " accepted=" << snapshot.accepted_records
              << " lane_rejected=" << snapshot.rejected_lane_full << '\n';
  }
  return success;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "producer allocation test failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("producer allocation test failed with an unknown exception\n", stderr);
    return 1;
  }
}
