#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <ulog/fields.hpp>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>
#include <ulog/timestamp.hpp>

namespace ulog::detail::producer {

inline constexpr std::size_t kAccountingQuantumBytes = 64;
inline constexpr std::size_t kMaximumRecordBytes = 16'384;
inline constexpr std::size_t kMaximumProducerSlots = 32;
inline constexpr std::size_t kMaximumIngressCells = 64;

using EventTimestamp = std::int64_t;

using EventClock = ::ulog::EventClock;

[[nodiscard]] EventClock SystemEventClock() noexcept;

struct ConsumerNotification final {
  void* context{nullptr};
  void (*notify)(void*) noexcept {nullptr};

  void Notify() const noexcept {
    if (notify != nullptr) {
      notify(context);
    }
  }
};

/// Waits for admission capacity on behalf of blocking calls (Logger::LogUntil). The producer kernel
/// owns no lock or wait primitive: the Runtime supplies these hooks, and the kernel calls `notify`
/// only while a blocking call is waiting. Every successful `enter` is paired with one `leave`.
struct CapacityWaiter final {
  void* context{nullptr};
  /// False on a thread that must never wait, such as a Ulog-owned thread.
  bool (*may_block)(void*) noexcept {nullptr};
  /// Registers a waiting call; false once the waiter is closed.
  bool (*enter)(void*) noexcept {nullptr};
  /// The capacity epoch, read before each attempt so a later notify is never lost.
  std::uint64_t (*epoch)(void*) noexcept {nullptr};
  /// Waits until the epoch moves past `observed`; false once closed or past `deadline`.
  bool (*wait)(void*, std::uint64_t observed,
               std::chrono::steady_clock::time_point deadline) noexcept {nullptr};
  void (*leave)(void*) noexcept {nullptr};
  /// Advances the epoch and wakes every waiting call.
  void (*notify)(void*) noexcept {nullptr};
  /// Releases every waiting call; later `enter` calls fail.
  void (*close)(void*) noexcept {nullptr};
};

struct KernelConfig final {
  Level threshold{Level::kInfo};
  std::size_t payload_capacity_bytes{0};
  std::size_t maximum_record_bytes{kMaximumRecordBytes};
  std::size_t producer_slots{kMaximumProducerSlots};
  std::size_t ingress_cells{kMaximumIngressCells};
  ConsumerNotification consumer_notification{};
  /// Bytes that only Error and Critical Records may use after the payload budget rejects them;
  /// zero disables the reserve.
  std::size_t priority_reserve_bytes{0};
  /// Drop-oldest: a Record the payload budget rejects may evict the oldest unclaimed Records of
  /// equal or lower level instead of being rejected.
  bool drop_oldest{false};
  /// Adaptive thinning: above `thinning_low_watermark_bytes` of shared-budget occupancy, calls are
  /// kept with a deterministic, falling probability before anything is reserved.
  bool adaptive_thinning{false};
  std::size_t thinning_low_watermark_bytes{0};
  /// Blocking calls wait through these hooks; without them every blocking call is refused.
  CapacityWaiter capacity_waiter{};
};

struct WriteResult final {
  std::size_t requested_bytes{0};
  std::size_t stored_bytes{0};
  bool truncated{false};
};

using FieldKind = ::ulog::FieldKind;
using NullValue = ::ulog::Null;
inline constexpr NullValue kNull{};

class RecordAppender final {
 public:
  class FormatOutputIterator final {
   public:
    using value_type = void;
    using difference_type = std::ptrdiff_t;
    using pointer = void;
    using reference = void;
    using iterator_category = std::output_iterator_tag;

    FormatOutputIterator& operator=(char value) noexcept;
    [[nodiscard]] FormatOutputIterator& operator*() noexcept { return *this; }
    FormatOutputIterator& operator++() noexcept { return *this; }
    FormatOutputIterator operator++(int) noexcept { return *this; }

   private:
    friend class RecordAppender;
    explicit FormatOutputIterator(RecordAppender& appender) noexcept : appender_(&appender) {}

    RecordAppender* appender_{nullptr};
  };

  [[nodiscard]] WriteResult Append(std::string_view value) noexcept;
  template <std::size_t Size>
  [[nodiscard]] WriteResult Append(const char (&value)[Size]) noexcept {
    static_assert(Size != 0);
    return Append(std::string_view{value, Size - 1U});
  }
  [[nodiscard]] WriteResult Append(std::int64_t value) noexcept;
  [[nodiscard]] WriteResult Append(std::uint64_t value) noexcept;
  [[nodiscard]] WriteResult Append(double value) noexcept;
  [[nodiscard]] WriteResult Append(bool value) noexcept;

  [[nodiscard]] bool AddField(std::string_view key, std::string_view value) noexcept;
  template <std::size_t Size>
  [[nodiscard]] bool AddField(std::string_view key, const char (&value)[Size]) noexcept {
    static_assert(Size != 0);
    return AddField(key, std::string_view{value, Size - 1U});
  }
  [[nodiscard]] bool AddField(std::string_view key, std::int64_t value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, std::uint64_t value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, double value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, bool value) noexcept;
  [[nodiscard]] bool AddField(std::string_view key, NullValue) noexcept;
  /// Adds one field under the Record collision and reserved-key policy.
  [[nodiscard]] FieldStatus PutField(std::string_view key, const FieldValue& value,
                                     FieldMode mode) noexcept;

  [[nodiscard]] FormatOutputIterator FormatOutput() noexcept { return FormatOutputIterator{*this}; }

 private:
  friend class ProducerKernel;
  explicit RecordAppender(void* writer) noexcept : writer_(writer) {}

  void* writer_{nullptr};
};

enum class BuildStatus : std::uint8_t { kComplete, kInvalid };

struct BuildOperation final {
  void* context{nullptr};
  BuildStatus (*invoke)(void*, RecordAppender&){nullptr};
};

enum class PublishOutcome : std::uint8_t {
  kAccepted,
  kFiltered,
  kNoProducerSlot,
  kLaneFull,
  kBudgetExhausted,
  /// An Error or Critical Record found both the payload budget and the priority reserve full.
  kPriorityReserveExhausted,
  /// Adaptive thinning dropped the call before it reserved anything.
  kThinned,
  /// A blocking call was refused without waiting because its thread must not block.
  kBlockingRefused,
  kInvalidRecord,
};

struct PublishResult final {
  PublishOutcome outcome{PublishOutcome::kInvalidRecord};
  std::optional<std::uint64_t> admission_sequence{};
  bool truncated{false};
};

class FieldView final {
 public:
  [[nodiscard]] std::string_view key() const noexcept;
  [[nodiscard]] FieldKind kind() const noexcept;
  [[nodiscard]] std::optional<std::string_view> AsString() const noexcept;
  [[nodiscard]] std::optional<std::int64_t> AsInt64() const noexcept;
  [[nodiscard]] std::optional<std::uint64_t> AsUInt64() const noexcept;
  [[nodiscard]] std::optional<double> AsDouble() const noexcept;
  [[nodiscard]] std::optional<bool> AsBool() const noexcept;
  [[nodiscard]] bool IsNull() const noexcept;
  [[nodiscard]] std::optional<std::string_view> AsJson() const noexcept;
  [[nodiscard]] bool IsFrozen() const noexcept;
  [[nodiscard]] FieldValue Value() const noexcept;

 private:
  friend class FieldCursor;
  friend class RecordView;
  FieldView(const void* storage, std::uint32_t offset) noexcept
      : storage_(storage), offset_(offset) {}

  const void* storage_{nullptr};
  std::uint32_t offset_{0};
};

class FieldCursor final {
 public:
  [[nodiscard]] std::optional<FieldView> Next() noexcept;

 private:
  friend class RecordView;
  FieldCursor(const void* storage, std::uint32_t offset, std::uint32_t remaining) noexcept
      : storage_(storage), offset_(offset), remaining_(remaining) {}

  const void* storage_{nullptr};
  std::uint32_t offset_{0};
  std::uint32_t remaining_{0};
};

class RecordView final {
 public:
  [[nodiscard]] explicit operator bool() const noexcept { return storage_ != nullptr; }
  [[nodiscard]] Level level() const noexcept;
  [[nodiscard]] EventTimestamp event_timestamp() const noexcept;
  [[nodiscard]] std::string_view source_path() const noexcept;
  [[nodiscard]] std::string_view source_function() const noexcept;
  [[nodiscard]] std::uint32_t source_line() const noexcept;
  [[nodiscard]] std::uint32_t source_column() const noexcept;
  [[nodiscard]] std::string_view message() const noexcept;
  [[nodiscard]] bool truncated() const noexcept;
  [[nodiscard]] std::size_t field_count() const noexcept;
  [[nodiscard]] FieldCursor Fields() const noexcept;
  [[nodiscard]] std::optional<FieldView> FieldAt(std::size_t index) const noexcept;
  [[nodiscard]] std::size_t serialized_bytes() const noexcept { return serialized_bytes_; }
  [[nodiscard]] std::size_t accounting_charge_bytes() const noexcept {
    return accounting_charge_bytes_;
  }

 private:
  friend class ProducerKernel;
  RecordView(const void* storage, std::size_t serialized_bytes,
             std::size_t accounting_charge_bytes) noexcept
      : storage_(storage),
        serialized_bytes_(serialized_bytes),
        accounting_charge_bytes_(accounting_charge_bytes) {}

  const void* storage_{nullptr};
  std::size_t serialized_bytes_{0};
  std::size_t accounting_charge_bytes_{0};
};

/// kShed: the next Record was evicted; the consumer skipped it without observing it.
enum class ConsumeStatus : std::uint8_t { kRecord, kEmpty, kPending, kShed };

using RecordConsumer = void (*)(void*, std::uint64_t admission_sequence,
                                const RecordView&) noexcept;

struct KernelSnapshot final {
  std::uint64_t attempted_records{0};
  std::uint64_t accepted_records{0};
  std::uint64_t consumed_records{0};
  std::uint64_t rejected_no_producer{0};
  std::uint64_t rejected_lane_full{0};
  std::uint64_t rejected_budget{0};
  std::uint64_t rejected_priority_reserve{0};
  std::uint64_t rejected_thinned{0};
  /// Blocking calls refused on a thread that must not wait.
  std::uint64_t rejected_blocking_refused{0};
  std::uint64_t abandoned_builds{0};
  std::uint64_t invalid_records{0};
  std::uint64_t truncated_records{0};
  /// Records evicted by drop-oldest; they hold no bytes but keep their cells until skipped.
  std::uint64_t evicted_records{0};
  std::uint64_t retained_records{0};
  std::size_t logical_retained_bytes{0};
  std::size_t physical_retained_bytes{0};
  std::size_t payload_capacity_bytes{0};
  std::size_t priority_reserve_bytes{0};
  std::size_t fixed_backing_bytes{0};
  std::size_t active_producer_slots{0};
  std::size_t retiring_producer_slots{0};
  bool accounting_sample_consistent{true};
};

class ProducerKernel final {
 public:
  class ProducerRegistration final {
   public:
    ProducerRegistration() noexcept = default;
    ProducerRegistration(ProducerRegistration&& other) noexcept;
    ProducerRegistration& operator=(ProducerRegistration&& other) noexcept;
    ProducerRegistration(const ProducerRegistration&) = delete;
    ProducerRegistration& operator=(const ProducerRegistration&) = delete;
    ~ProducerRegistration();

    [[nodiscard]] explicit operator bool() const noexcept { return owner_ != nullptr; }

   private:
    friend class ProducerKernel;
    ProducerRegistration(ProducerKernel& owner, std::size_t slot_index,
                         std::uint64_t generation) noexcept
        : owner_(&owner), slot_index_(slot_index), generation_(generation) {}

    void Reset() noexcept;

    ProducerKernel* owner_{nullptr};
    std::size_t slot_index_{0};
    std::uint64_t generation_{0};
  };

  explicit ProducerKernel(KernelConfig config, EventClock clock = SystemEventClock());
  ~ProducerKernel();

  ProducerKernel(const ProducerKernel&) = delete;
  ProducerKernel& operator=(const ProducerKernel&) = delete;
  ProducerKernel(ProducerKernel&&) = delete;
  ProducerKernel& operator=(ProducerKernel&&) = delete;

  [[nodiscard]] Logger GetLogger() noexcept;
  [[nodiscard]] ProducerRegistration TryRegisterProducer() noexcept;
  void SetLevel(Level threshold) noexcept;
  /// Logger-facing threshold change; rejected once admission is closed.
  [[nodiscard]] LevelChangeStatus ChangeLevel(Level threshold) noexcept;
  void CloseAdmission() noexcept;
  [[nodiscard]] bool IsAdmissionOpen() const noexcept;
  [[nodiscard]] bool IsQuiescent() noexcept;

  [[nodiscard]] PublishResult TryPublish(ProducerRegistration& producer, Level level,
                                         const SourceLocation& source, BuildOperation operation);
  /// Like TryPublish, but a call that finds no lane cell or credit waits for capacity until
  /// `deadline` without shedding, then applies the configured policy once more. An expired
  /// deadline behaves exactly like TryPublish.
  [[nodiscard]] PublishResult TryPublishUntil(ProducerRegistration& producer, Level level,
                                              std::chrono::steady_clock::time_point deadline,
                                              const SourceLocation& source,
                                              BuildOperation operation);
  /// Consumes the next Record. An evicted Record is skipped instead: the result is kShed and its
  /// admission sequence is stored in `shed_sequence` when that is not null.
  [[nodiscard]] ConsumeStatus TryConsume(void* context, RecordConsumer consumer,
                                         std::uint64_t* shed_sequence = nullptr) noexcept;
  /// Skips the next Record only if it was evicted, so the consumer frees evicted cells without
  /// waiting for a destination slot. Returns kShed with its sequence, or kEmpty/kPending.
  [[nodiscard]] ConsumeStatus TrySkipShed(std::uint64_t& shed_sequence) noexcept;
  [[nodiscard]] KernelSnapshot GetSnapshot() const noexcept;

 private:
  struct Impl;

  static void LogMessage(void* context, Level level, const SourceLocation& source,
                         void* builder_context, MessageBuilder build_message);
  static void LogMessageUntil(void* context, Level level,
                              std::chrono::steady_clock::time_point deadline,
                              const SourceLocation& source, void* builder_context,
                              MessageBuilder build_message);
  static bool BeginRecord(void* context, Level level, const SourceLocation& source,
                          OpenRecord& record) noexcept;
  static bool AppendOpenText(void* context, std::string_view text) noexcept;
  static FieldStatus PutOpenField(void* context, std::string_view key, const FieldValue& value,
                                  FieldMode mode) noexcept;
  static void FinishOpenRecord(void* context, bool publish) noexcept;
  [[nodiscard]] PublishResult TryPublishSlot(std::size_t slot_index, std::uint64_t generation,
                                             Level level, const SourceLocation& source,
                                             BuildOperation operation);
  [[nodiscard]] PublishResult TryPublishSlotUntil(std::size_t slot_index, std::uint64_t generation,
                                                  Level level,
                                                  std::chrono::steady_clock::time_point deadline,
                                                  const SourceLocation& source,
                                                  BuildOperation operation);
  /// Builds and finishes the Record BeginSlot opened for `slot_index`.
  [[nodiscard]] PublishResult BuildOpenSlot(std::size_t slot_index, BuildOperation operation);
  /// Claims, reserves, and starts the slot's Record; it stays open until FinishSlot. While
  /// `defer_shedding` is set, a blocking call is waiting: thinning and eviction are skipped and a
  /// lane or credit rejection is returned uncounted, since the call will try again.
  [[nodiscard]] PublishOutcome BeginSlot(std::size_t slot_index, std::uint64_t generation,
                                         Level level, const SourceLocation& source,
                                         bool defer_shedding = false) noexcept;
  /// Publishes the slot's open Record after a complete build, or abandons it.
  PublishResult FinishSlot(std::size_t slot_index, BuildStatus build_status) noexcept;
  void RetireProducer(ProducerRegistration& producer) noexcept;

  std::uint64_t identity_{0};
  std::unique_ptr<Impl> impl_;
};

}  // namespace ulog::detail::producer
