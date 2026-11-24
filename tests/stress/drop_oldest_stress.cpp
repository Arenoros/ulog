// Concurrent drop-oldest: producers evict each other's unclaimed Records while one consumer claims
// and skips them. Checks that every admitted Record is delivered or shed exactly once, that the
// survivors of each producer keep their admission order, and that byte accounting returns to zero.

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string_view>
#include <thread>
#include <ulog/level.hpp>
#include <ulog/source_location.hpp>
#include <vector>

#include "producer/producer_kernel.hpp"

namespace {

namespace producer = ulog::detail::producer;

constexpr std::size_t kProducers = 4;
constexpr std::size_t kAttemptsPerProducer = 50'000;
constexpr ulog::SourceLocation kSource = ulog::SourceLocation::Custom("stress.cpp", "Evict", 1);
constexpr std::array kLevels{ulog::Level::kInfo, ulog::Level::kWarning, ulog::Level::kError,
                             ulog::Level::kCritical};

/// Writes the producer and its attempt number as the eight-byte message.
struct TaggedBuild final {
  std::uint32_t producer;
  std::uint32_t attempt;

  static producer::BuildStatus Invoke(void* context, producer::RecordAppender& appender) {
    const auto& build = *static_cast<const TaggedBuild*>(context);
    std::array<char, 8> text{};
    for (std::size_t index = 0; index < 4; ++index) {
      text[index] = static_cast<char>('a' + ((build.producer >> (4U * index)) & 0xFU));
      text[4 + index] = static_cast<char>('a' + ((build.attempt >> (4U * index)) & 0xFU));
    }
    static_cast<void>(appender.Append(std::string_view{text.data(), text.size()}));
    return producer::BuildStatus::kComplete;
  }
};

struct Delivered final {
  std::uint64_t sequence;
  std::uint32_t producer;
  std::uint32_t attempt;
};

void Observe(void* context, std::uint64_t sequence, const producer::RecordView& record) noexcept {
  auto& delivered = *static_cast<Delivered*>(context);
  delivered.sequence = sequence;
  const std::string_view text = record.message();
  delivered.producer = 0;
  delivered.attempt = 0;
  for (std::size_t index = 0; index < 4 && text.size() == 8U; ++index) {
    delivered.producer |= static_cast<std::uint32_t>(text[index] - 'a') << (4U * index);
    delivered.attempt |= static_cast<std::uint32_t>(text[4 + index] - 'a') << (4U * index);
  }
}

[[nodiscard]] bool Run() {
  // Sixteen cells but a budget of four worst-case Records, so the budget binds and evicts.
  producer::ProducerKernel kernel{producer::KernelConfig{
      .threshold = ulog::Level::kTrace,
      .payload_capacity_bytes = 2'048,
      .maximum_record_bytes = 512,
      .producer_slots = kProducers,
      .ingress_cells = 16,
      .drop_oldest = true,
  }};
  std::atomic<std::size_t> producers_done{0};
  std::array<std::uint64_t, kProducers> accepted{};
  std::vector<std::thread> producers;
  for (std::size_t index = 0; index < kProducers; ++index) {
    producers.emplace_back([&, index] {
      auto registration = kernel.TryRegisterProducer();
      for (std::uint32_t attempt = 0; registration && attempt < kAttemptsPerProducer; ++attempt) {
        TaggedBuild build{static_cast<std::uint32_t>(index), attempt};
        const auto result =
            kernel.TryPublish(registration, kLevels[(index + attempt) % kLevels.size()], kSource,
                              producer::BuildOperation{&build, &TaggedBuild::Invoke});
        accepted[index] += result.outcome == producer::PublishOutcome::kAccepted ? 1U : 0U;
      }
      producers_done.fetch_add(1, std::memory_order_release);
    });
  }

  std::uint64_t delivered_count = 0;
  std::uint64_t shed_count = 0;
  std::uint64_t next_sequence = 0;
  bool ordered = true;
  std::array<std::int64_t, kProducers> last_attempt{};
  last_attempt.fill(-1);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{20};
  while (std::chrono::steady_clock::now() < deadline) {
    Delivered delivered{};
    std::uint64_t shed_sequence = 0;
    const auto status = kernel.TryConsume(&delivered, &Observe, &shed_sequence);
    if (status == producer::ConsumeStatus::kShed) {
      ordered = ordered && shed_sequence == next_sequence;
      ++next_sequence;
      ++shed_count;
      continue;
    }
    if (status == producer::ConsumeStatus::kRecord) {
      ordered = ordered && delivered.sequence == next_sequence &&
                delivered.producer<kProducers&& static_cast<std::int64_t>(delivered.attempt)>
                    last_attempt[delivered.producer];
      if (delivered.producer < kProducers) {
        last_attempt[delivered.producer] = delivered.attempt;
      }
      ++next_sequence;
      ++delivered_count;
      // A slow consumer keeps the budget saturated so producers keep evicting.
      if (delivered_count % 8U == 0U) {
        std::this_thread::yield();
      }
      continue;
    }
    if (producers_done.load(std::memory_order_acquire) == kProducers &&
        status == producer::ConsumeStatus::kEmpty) {
      break;
    }
    std::this_thread::yield();
  }
  for (auto& thread : producers) {
    thread.join();
  }

  std::uint64_t total_accepted = 0;
  for (const std::uint64_t count : accepted) {
    total_accepted += count;
  }
  const auto snapshot = kernel.GetSnapshot();
  const bool valid = ordered && total_accepted == snapshot.accepted_records &&
                     delivered_count + shed_count == total_accepted &&
                     shed_count == snapshot.evicted_records && shed_count != 0U &&
                     snapshot.retained_records == 0U && snapshot.logical_retained_bytes == 0U &&
                     snapshot.invalid_records == 0U && snapshot.accounting_sample_consistent;
  std::printf("drop-oldest stress: accepted=%llu delivered=%llu shed=%llu\n",
              static_cast<unsigned long long>(total_accepted),
              static_cast<unsigned long long>(delivered_count),
              static_cast<unsigned long long>(shed_count));
  if (!valid) {
    std::fprintf(stderr,
                 "drop-oldest stress: ordered=%d accepted=%llu snapshot_accepted=%llu "
                 "delivered=%llu shed=%llu evicted=%llu retained=%llu logical=%zu invalid=%llu\n",
                 ordered ? 1 : 0, static_cast<unsigned long long>(total_accepted),
                 static_cast<unsigned long long>(snapshot.accepted_records),
                 static_cast<unsigned long long>(delivered_count),
                 static_cast<unsigned long long>(shed_count),
                 static_cast<unsigned long long>(snapshot.evicted_records),
                 static_cast<unsigned long long>(snapshot.retained_records),
                 snapshot.logical_retained_bytes,
                 static_cast<unsigned long long>(snapshot.invalid_records));
  }
  return valid;
}

}  // namespace

int main() noexcept {
  try {
    return Run() ? 0 : 1;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "drop-oldest stress failed with exception: %s\n", error.what());
    return 1;
  } catch (...) {
    std::fputs("drop-oldest stress failed with an unknown exception\n", stderr);
    return 1;
  }
}
