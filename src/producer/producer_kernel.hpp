#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <ulog/level.hpp>
#include <ulog/logger.hpp>
#include <ulog/source_location.hpp>

namespace ulog::detail::producer {

inline constexpr std::size_t kAccountingQuantumBytes = 64;
inline constexpr std::size_t kMaximumRecordBytes = 16'384;
inline constexpr std::size_t kMaximumProducerSlots = 32;
inline constexpr std::size_t kMaximumIngressCells = 64;

using EventTimestamp = std::int64_t;

struct EventClock final {
  void* context{nullptr};
  EventTimestamp (*now)(void*) noexcept {nullptr};
};

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

struct KernelConfig final {
  Level threshold{Level::kInfo};
  std::size_t payload_capacity_bytes{0};
  std::size_t maximum_record_bytes{kMaximumRecordBytes};
  std::size_t producer_slots{kMaximumProducerSlots};
  std::size_t ingress_cells{kMaximumIngressCells};
  ConsumerNotification consumer_notification{};
};

struct WriteResult final {
  std::size_t requested_bytes{0};
  std::size_t stored_bytes{0};
  bool truncated{false};
};

enum class FieldKind : std::uint8_t { kString, kInt64, kUInt64, kDouble, kBool, kNull };

struct NullValue final {};
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

enum class ConsumeStatus : std::uint8_t { kRecord, kEmpty, kPending };

using RecordConsumer = void (*)(void*, std::uint64_t admission_sequence,
                                const RecordView&) noexcept;

struct KernelSnapshot final {
  std::uint64_t attempted_records{0};
  std::uint64_t accepted_records{0};
  std::uint64_t consumed_records{0};
  std::uint64_t rejected_no_producer{0};
  std::uint64_t rejected_lane_full{0};
  std::uint64_t rejected_budget{0};
  std::uint64_t abandoned_builds{0};
  std::uint64_t invalid_records{0};
  std::uint64_t truncated_records{0};
  std::uint64_t retained_records{0};
  std::size_t logical_retained_bytes{0};
  std::size_t physical_retained_bytes{0};
  std::size_t payload_capacity_bytes{0};
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
  void CloseAdmission() noexcept;
  [[nodiscard]] bool IsAdmissionOpen() const noexcept;
  [[nodiscard]] bool IsQuiescent() noexcept;

  [[nodiscard]] PublishResult TryPublish(ProducerRegistration& producer, Level level,
                                         const SourceLocation& source, BuildOperation operation);
  [[nodiscard]] ConsumeStatus TryConsume(void* context, RecordConsumer consumer) noexcept;
  [[nodiscard]] KernelSnapshot GetSnapshot() const noexcept;

 private:
  struct Impl;

  static void LogMessage(void* context, Level level, const SourceLocation& source,
                         void* builder_context, MessageBuilder build_message);
  [[nodiscard]] PublishResult TryPublishSlot(std::size_t slot_index, std::uint64_t generation,
                                             Level level, const SourceLocation& source,
                                             BuildOperation operation);
  void RetireProducer(ProducerRegistration& producer) noexcept;

  std::uint64_t identity_{0};
  std::unique_ptr<Impl> impl_;
};

}  // namespace ulog::detail::producer
