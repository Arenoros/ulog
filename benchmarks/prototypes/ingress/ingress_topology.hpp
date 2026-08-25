#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

namespace ulog::benchmark_support::ingress {

inline constexpr std::size_t kMaximumConcurrentPublishers = 32;

struct RecordHandle final {
  std::uint32_t slot_index{0};
  std::uint64_t generation{0};
  std::uint64_t serialized_bytes{0};
  std::uint64_t accounting_charge_bytes{0};

  [[nodiscard]] friend bool operator==(const RecordHandle&, const RecordHandle&) = default;
};

enum class PublishStatus : std::uint8_t { kAccepted, kFull, kContended, kInvalid };

struct PublishResult final {
  PublishStatus status{PublishStatus::kInvalid};
  std::optional<std::uint64_t> admission_sequence;
  std::size_t publication_actions{0};
};

enum class ConsumeStatus : std::uint8_t { kEmpty, kPending, kRecord };

struct ConsumedRecord final {
  RecordHandle record;
  std::uint64_t admission_sequence{0};
};

struct ConsumeResult final {
  ConsumeStatus status{ConsumeStatus::kEmpty};
  std::optional<ConsumedRecord> record;
};

struct TopologySnapshot final {
  std::uint64_t attempted_records{0};
  std::uint64_t enqueued_records{0};
  std::uint64_t dequeued_records{0};
  std::uint64_t rejected_records{0};
  std::uint64_t full_rejections{0};
  std::uint64_t contention_rejections{0};
  std::uint64_t invalid_rejections{0};
  std::uint64_t retained_records{0};
  std::uint64_t retained_serialized_bytes{0};
  std::uint64_t retained_charge_bytes{0};
};

}  // namespace ulog::benchmark_support::ingress
