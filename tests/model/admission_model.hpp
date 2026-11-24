#pragma once

// Reference model of single-route admission, used as a test oracle for the real Runtime.
//
// The model is a sequential state machine. Each mutating call applies one externally visible
// step, then Settle() advances the modeled worker until it can make no further progress. The real
// Runtime must converge to the same observable state after the same step.
//
// Extension points for later slices:
// - Admit() decides one attempt; a new shedding policy (drop-oldest, thinning, blocking) adds an
//   Admission reason and a decision stage there, as the priority reserve did.
// - RouteModel owns destination slots and deliveries; another route type replaces it, and
//   multiple routes become several RouteModel instances fed from the same admission sequence.
// - ControlModel owns Operation reports; route-local watermarks extend its report builder.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <ulog/operation.hpp>
#include <utility>
#include <vector>

namespace ulog::test::admission {

inline constexpr std::size_t kQuantumBytes = 64;
/// Serialized Record header bytes; the oracle checks this against Record storage.
inline constexpr std::size_t kRecordMetadataBytes = 56;
/// A Raw frame without fields is "tskv\ttext=" + message + "\n".
inline constexpr std::size_t kRawFrameOverheadBytes = 11;
inline constexpr std::size_t kUnknownProducer = static_cast<std::size_t>(-1);

/// Byte charge of one retained Record: at least one quantum, rounded up to whole quanta.
[[nodiscard]] constexpr std::size_t ChargeBytes(std::size_t serialized_bytes) noexcept {
  const std::size_t at_least = serialized_bytes < kQuantumBytes ? kQuantumBytes : serialized_bytes;
  return (at_least + kQuantumBytes - 1U) / kQuantumBytes * kQuantumBytes;
}

struct Config final {
  std::size_t producer_slots{1};
  std::size_t ingress_cells{1};
  std::size_t payload_capacity_bytes{512};
  std::size_t maximum_record_bytes{512};
  std::size_t destination_slots{1};
  std::size_t control_operations{1};
  /// Source path plus function name bytes of the call site every producer uses.
  std::size_t source_bytes{0};
  /// Bytes that only priority (Error and Critical) Records use after the payload budget rejects
  /// them; zero disables the reserve.
  std::size_t priority_reserve_bytes{0};
};

enum class Admission : std::uint8_t {
  kAccepted,
  kFiltered,
  kNoProducer,
  kLaneFull,
  kBudget,
  kPriorityReserve,
};

enum class DeliveryMode : std::uint8_t { kCompleteInline, kHold, kFailInline };

enum class Stage : std::uint8_t {
  kIngress,   // admitted, not yet taken by the worker
  kHeld,      // submitted and waiting for a test completion
  kReady,     // delivered and waiting for TryTake()
  kObserved,  // taken and pinned by an observation
  kReleased,  // no longer pins its destination slot
};

struct Record final {
  std::uint64_t sequence{0};
  std::size_t producer{kUnknownProducer};
  /// Burst that admitted this Record, or zero. Burst Records have an unknown producer.
  std::size_t burst{0};
  /// Exact message, or empty for a burst Record whose message is only known by its length.
  std::string message;
  std::size_t message_bytes{0};
  std::size_t serialized_bytes{0};
  Stage stage{Stage::kIngress};
  bool processed{false};
  bool has_outcome{false};
  bool delivered{false};
  bool retired{false};
  bool handle_taken{false};
  /// Charged to the priority reserve, which takes its bytes back when the worker consumes it.
  bool priority{false};

  [[nodiscard]] std::size_t charge_bytes() const noexcept { return ChargeBytes(serialized_bytes); }
  [[nodiscard]] std::size_t frame_bytes() const noexcept {
    return message_bytes + kRawFrameOverheadBytes;
  }
};

enum class OperationKind : std::uint8_t { kDrain, kShutdown };

struct ModelOperation final {
  OperationKind kind{OperationKind::kDrain};
  std::uint64_t watermark{0};
  bool completed{false};
  OperationOutcome outcome{OperationOutcome::kSucceeded};
  OperationReport report{};
  bool handle_released{false};
};

struct Counters final {
  std::uint64_t accepted{0};
  std::uint64_t processed{0};
  std::uint64_t processed_bytes{0};
  std::uint64_t delivered{0};
  std::uint64_t delivered_bytes{0};
  std::uint64_t failed{0};
  std::uint64_t failed_bytes{0};
  std::uint64_t rejected_no_producer{0};
  std::uint64_t rejected_lane_full{0};
  std::uint64_t rejected_budget{0};
  std::uint64_t rejected_priority_reserve{0};
  std::uint64_t retained_records{0};
  std::size_t logical_retained_bytes{0};
  /// Empty while it depends on an unresolved burst order.
  std::optional<std::size_t> physical_retained_bytes{};
  bool admission_open{true};
};

class AdmissionModel final {
 public:
  explicit AdmissionModel(Config config)
      : config_(config),
        central_(config.payload_capacity_bytes),
        priority_available_(config.priority_reserve_bytes) {
    for (std::size_t slot = 0; slot < config.producer_slots; ++slot) {
      ProducerState producer;
      producer.lane_capacity = config.ingress_cells / config.producer_slots +
                               (slot < config.ingress_cells % config.producer_slots ? 1U : 0U);
      producers_.push_back(producer);
    }
  }

  [[nodiscard]] const Config& config() const noexcept { return config_; }
  [[nodiscard]] std::size_t MaximumMessageBytes() const noexcept {
    return config_.maximum_record_bytes - kRecordMetadataBytes - config_.source_bytes;
  }

  // ---- Producers ----------------------------------------------------------------------------

  /// Registers producer thread `thread`; threads register in index order and take the lowest free
  /// slot, so thread i owns slot i. Returns whether the thread received a slot.
  bool Register(std::size_t thread) {
    if (!admission_open_ || thread >= producers_.size()) {
      return false;
    }
    producers_[thread].registered = true;
    return true;
  }

  /// Whether a producer's lane and credit currently depend on an unresolved burst order.
  [[nodiscard]] bool IsBusy(std::size_t thread) const noexcept {
    return thread < producers_.size() && producers_[thread].busy_burst != 0;
  }

  /// One attempt by `thread` to log `message`; a `priority` (Error or Critical) attempt may use
  /// the priority reserve after the payload budget rejects it.
  Admission Log(std::size_t thread, std::string message, bool priority = false) {
    const std::size_t bytes = message.size();
    bool from_reserve = false;
    const Admission result = Admit(thread, SerializedBytes(bytes), priority, from_reserve);
    last_used_reserve_ = from_reserve;
    if (result == Admission::kAccepted) {
      Record record;
      record.sequence = records_.size();
      record.producer = thread;
      record.message = std::move(message);
      record.message_bytes = bytes;
      record.serialized_bytes = SerializedBytes(bytes);
      record.priority = from_reserve;
      records_.push_back(std::move(record));
    }
    Settle();
    return result;
  }

  /// Whether the last Log() was admitted through the priority reserve.
  [[nodiscard]] bool last_used_reserve() const noexcept { return last_used_reserve_; }

  /// Whether `count` attempts by each of `threads` with `message_bytes` messages have one outcome
  /// per thread for every interleaving: the worker cannot take a Record during the burst, and the
  /// shared payload pool covers every refill the attempts can request.
  [[nodiscard]] bool IsBurstDeterministic(const std::vector<std::size_t>& threads,
                                          std::size_t count,
                                          std::size_t message_bytes) const noexcept {
    if (!admission_open_) {
      return true;
    }
    if (!worker_stopped_ && route_.FreeSlots(records_, config_.destination_slots) != 0U) {
      return false;
    }
    std::size_t refills = 0;
    for (const std::size_t thread : threads) {
      if (thread >= producers_.size() || !producers_[thread].registered ||
          producers_[thread].retiring) {
        continue;
      }
      ProducerState copy = producers_[thread];
      for (std::size_t attempt = 0; attempt < count; ++attempt) {
        if (copy.lane_used == copy.lane_capacity) {
          break;
        }
        const std::size_t reserve = config_.maximum_record_bytes;
        if (copy.local_credit < reserve) {
          refills += reserve - copy.local_credit;
          copy.local_credit = reserve;
        }
        copy.local_credit -= ChargeBytes(SerializedBytes(message_bytes));
        ++copy.lane_used;
      }
    }
    return refills <= central_;
  }

  /// A deterministic burst: each thread makes `count` attempts concurrently. Returns the accepted
  /// count per thread. Accepted Records take consecutive sequences whose producer order is unknown.
  std::vector<std::size_t> Burst(const std::vector<std::size_t>& threads, std::size_t count,
                                 std::size_t message_bytes) {
    const std::size_t burst = ++last_burst_;
    BurstState state;
    state.charge_bytes = ChargeBytes(SerializedBytes(message_bytes));
    std::vector<std::size_t> accepted(threads.size(), 0);
    for (std::size_t index = 0; index < threads.size(); ++index) {
      for (std::size_t attempt = 0; attempt < count; ++attempt) {
        bool from_reserve = false;
        if (Admit(threads[index], SerializedBytes(message_bytes), false, from_reserve) !=
            Admission::kAccepted) {
          continue;
        }
        ++accepted[index];
        Record record;
        record.sequence = records_.size();
        record.burst = burst;
        record.message_bytes = message_bytes;
        record.serialized_bytes = SerializedBytes(message_bytes);
        records_.push_back(std::move(record));
      }
      if (accepted[index] != 0U) {
        state.accepted.emplace_back(threads[index], accepted[index]);
        state.remaining += accepted[index];
        producers_[threads[index]].busy_burst = burst;
      }
    }
    if (state.remaining != 0U) {
      bursts_.emplace_back(burst, std::move(state));
    }
    Settle();
    return accepted;
  }

  // ---- Route --------------------------------------------------------------------------------

  void SetMode(DeliveryMode mode) noexcept { route_.mode = mode; }
  [[nodiscard]] DeliveryMode mode() const noexcept { return route_.mode; }

  /// Held Records in admission order.
  [[nodiscard]] std::vector<std::uint64_t> Held() const {
    std::vector<std::uint64_t> held;
    for (const Record& record : records_) {
      if (record.stage == Stage::kHeld) {
        held.push_back(record.sequence);
      }
    }
    return held;
  }

  /// The held Record TryTakePendingDelivery() returns next, if any.
  [[nodiscard]] std::optional<std::uint64_t> NextUntakenHeld() const noexcept {
    for (const Record& record : records_) {
      if (record.stage == Stage::kHeld && !record.handle_taken) {
        return record.sequence;
      }
    }
    return std::nullopt;
  }

  void TakeHandle(std::uint64_t sequence) { records_.at(sequence).handle_taken = true; }

  /// Completes a held delivery. A failed or abandoned delivery discards its frame.
  void Finish(std::uint64_t sequence, bool delivered) {
    Record& record = records_.at(sequence);
    record.has_outcome = true;
    record.delivered = delivered;
    record.stage = delivered ? Stage::kReady : Stage::kReleased;
    Settle();
  }

  /// The Record TryTake() returns next: the lowest delivered frame not yet taken.
  [[nodiscard]] std::optional<std::uint64_t> NextReady() const noexcept {
    for (const Record& record : records_) {
      if (record.stage == Stage::kReady) {
        return record.sequence;
      }
    }
    return std::nullopt;
  }

  void Take(std::uint64_t sequence) { records_.at(sequence).stage = Stage::kObserved; }

  [[nodiscard]] std::vector<std::uint64_t> Observed() const {
    std::vector<std::uint64_t> observed;
    for (const Record& record : records_) {
      if (record.stage == Stage::kObserved) {
        observed.push_back(record.sequence);
      }
    }
    return observed;
  }

  void Release(std::uint64_t sequence) {
    records_.at(sequence).stage = Stage::kReleased;
    Settle();
  }

  // ---- Control ------------------------------------------------------------------------------

  /// Starts a Drain or Shutdown. Returns the Operation index, or empty when the control reserve is
  /// exhausted and nothing starts.
  std::optional<std::size_t> Start(OperationKind kind) {
    if (ControlsInUse() == config_.control_operations) {
      return std::nullopt;
    }
    ModelOperation operation;
    operation.kind = kind;
    if (worker_stopped_) {
      operation.watermark = records_.size();
      Complete(operation);
    } else {
      if (kind == OperationKind::kShutdown && !shutdown_requested_) {
        shutdown_requested_ = true;
        admission_open_ = false;
        for (ProducerState& producer : producers_) {
          producer.retiring = producer.registered;
        }
      }
      operation.watermark = records_.size();
      if (kind == OperationKind::kDrain && operation.watermark == retired_) {
        Complete(operation);
      }
    }
    operations_.push_back(operation);
    Settle();
    return operations_.size() - 1U;
  }

  void ReleaseOperation(std::size_t index) { operations_.at(index).handle_released = true; }

  [[nodiscard]] const std::vector<ModelOperation>& operations() const noexcept {
    return operations_;
  }
  [[nodiscard]] std::size_t ControlsInUse() const noexcept {
    std::size_t in_use = 0;
    for (const ModelOperation& operation : operations_) {
      in_use += operation.completed && operation.handle_released ? 0U : 1U;
    }
    return in_use;
  }

  // ---- Observation --------------------------------------------------------------------------

  [[nodiscard]] const std::vector<Record>& records() const noexcept { return records_; }
  [[nodiscard]] bool worker_stopped() const noexcept { return worker_stopped_; }

  [[nodiscard]] Counters counters() const {
    Counters counters = rejections_;
    counters.accepted = records_.size();
    counters.admission_open = admission_open_;
    for (const Record& record : records_) {
      if (!record.processed) {
        ++counters.retained_records;
        counters.logical_retained_bytes += record.serialized_bytes;
        continue;
      }
      ++counters.processed;
      counters.processed_bytes += record.frame_bytes();
      if (record.retired) {
        (record.delivered ? counters.delivered : counters.failed) += 1U;
        (record.delivered ? counters.delivered_bytes : counters.failed_bytes) +=
            record.frame_bytes();
      }
    }
    const bool physical_known = !shutdown_requested_ || bursts_.empty();
    if (physical_known) {
      counters.physical_retained_bytes = config_.payload_capacity_bytes - central_ +
                                         config_.priority_reserve_bytes - priority_available_;
    }
    return counters;
  }

 private:
  struct ProducerState final {
    bool registered{false};
    /// Admission closed; the slot returns its credit once its lane is drained.
    bool retiring{false};
    std::size_t lane_capacity{0};
    std::size_t lane_used{0};
    std::size_t local_credit{0};
    std::size_t busy_burst{0};
  };

  struct BurstState final {
    std::vector<std::pair<std::size_t, std::size_t>> accepted;  // thread, Records
    std::size_t remaining{0};
    std::size_t charge_bytes{0};
  };

  struct RouteModel final {
    DeliveryMode mode{DeliveryMode::kCompleteInline};

    /// A slot is reusable once its outcome retired and no delivered frame pins it.
    [[nodiscard]] static std::size_t FreeSlots(const std::vector<Record>& records,
                                               std::size_t capacity) noexcept {
      std::size_t used = 0;
      for (const Record& record : records) {
        if (record.processed && (!record.retired || record.stage == Stage::kReady ||
                                 record.stage == Stage::kObserved)) {
          ++used;
        }
      }
      return used < capacity ? capacity - used : 0U;
    }

    void Submit(Record& record) const noexcept {
      switch (mode) {
        case DeliveryMode::kCompleteInline:
          record.has_outcome = true;
          record.delivered = true;
          record.stage = Stage::kReady;
          break;
        case DeliveryMode::kHold:
          record.stage = Stage::kHeld;
          break;
        case DeliveryMode::kFailInline:
          record.has_outcome = true;
          record.delivered = false;
          record.stage = Stage::kReleased;
          break;
      }
    }
  };

  [[nodiscard]] std::size_t SerializedBytes(std::size_t message_bytes) const noexcept {
    return kRecordMetadataBytes + config_.source_bytes + message_bytes;
  }

  /// Drop-newest admission, checked in the producer kernel's order. A rejected attempt consumes
  /// no sequence and evaluates no message.
  Admission Admit(std::size_t thread, std::size_t serialized_bytes, bool priority,
                  bool& from_reserve) {
    from_reserve = false;
    if (!admission_open_) {
      // Closing admission also sets the threshold to None, so the call is filtered uncounted.
      return Admission::kFiltered;
    }
    if (thread >= producers_.size() || !producers_[thread].registered) {
      ++rejections_.rejected_no_producer;
      return Admission::kNoProducer;
    }
    ProducerState& producer = producers_[thread];
    if (producer.lane_used == producer.lane_capacity) {
      ++rejections_.rejected_lane_full;
      return Admission::kLaneFull;
    }
    // The worst-case reservation refills only the producer's deficit from the shared pool; credit
    // returned by a commit or a consumed Record stays with the producer until it retires.
    const std::size_t reserve = config_.maximum_record_bytes;
    if (producer.local_credit < reserve) {
      const std::size_t deficit = reserve - producer.local_credit;
      if (central_ < deficit) {
        if (!priority || config_.priority_reserve_bytes == 0U) {
          ++rejections_.rejected_budget;
          return Admission::kBudget;
        }
        // The reserve charges the worst case, and the commit returns the unused part at once.
        const std::size_t worst_case = ChargeBytes(reserve);
        if (priority_available_ < worst_case) {
          ++rejections_.rejected_priority_reserve;
          return Admission::kPriorityReserve;
        }
        priority_available_ -= ChargeBytes(serialized_bytes);
        ++producer.lane_used;
        from_reserve = true;
        return Admission::kAccepted;
      }
      central_ -= deficit;
      producer.local_credit = reserve;
    }
    producer.local_credit -= ChargeBytes(serialized_bytes);
    ++producer.lane_used;
    return Admission::kAccepted;
  }

  void Consume(Record& record) {
    record.processed = true;
    route_.Submit(record);
    if (record.burst == 0U) {
      ProducerState& producer = producers_[record.producer];
      --producer.lane_used;
      (record.priority ? priority_available_ : producer.local_credit) += record.charge_bytes();
      return;
    }
    for (auto it = bursts_.begin(); it != bursts_.end(); ++it) {
      if (it->first != record.burst || --it->second.remaining != 0U) {
        continue;
      }
      // The whole burst left ingress, so every producer's share is known again.
      for (const auto& [thread, count] : it->second.accepted) {
        ProducerState& producer = producers_[thread];
        producer.lane_used -= count;
        producer.local_credit += count * it->second.charge_bytes;
        if (producer.busy_burst == record.burst) {
          producer.busy_burst = 0;
        }
      }
      bursts_.erase(it);
      return;
    }
  }

  void ReconcileRetiringProducers() noexcept {
    for (ProducerState& producer : producers_) {
      if (producer.retiring && producer.busy_burst == 0U && producer.lane_used == 0U) {
        central_ += producer.local_credit;
        producer.local_credit = 0;
        producer.retiring = false;
        producer.registered = false;
      }
    }
  }

  [[nodiscard]] OperationReport Report(std::uint64_t watermark) const noexcept {
    OperationReport report{.watermark_records = watermark};
    for (std::uint64_t sequence = 0; sequence < watermark; ++sequence) {
      const Record& record = records_[sequence];
      ++report.processed_records;
      report.processed_bytes += record.frame_bytes();
      if (record.delivered) {
        ++report.delivered_records;
        report.delivered_bytes += record.frame_bytes();
      } else {
        ++report.failed_records;
        report.failed_bytes += record.frame_bytes();
      }
    }
    return report;
  }

  void Complete(ModelOperation& operation) const noexcept {
    operation.completed = true;
    operation.outcome = OperationOutcome::kSucceeded;
    operation.report = Report(operation.watermark);
  }

  /// Advances the modeled worker until it cannot progress.
  void Settle() {
    bool progressed = true;
    while (progressed) {
      progressed = false;
      if (shutdown_requested_) {
        ReconcileRetiringProducers();
      }
      // Retire outcomes strictly in admission order; each retirement completes the Drains whose
      // watermark it reaches.
      while (retired_ < records_.size() && records_[retired_].processed &&
             records_[retired_].has_outcome) {
        records_[retired_].retired = true;
        ++retired_;
        progressed = true;
        for (ModelOperation& operation : operations_) {
          if (!operation.completed && operation.kind == OperationKind::kDrain &&
              operation.watermark <= retired_) {
            Complete(operation);
          }
        }
      }
      if (shutdown_requested_ && !worker_stopped_ && retired_ == records_.size()) {
        ReconcileRetiringProducers();
        worker_stopped_ = true;
        progressed = true;
        for (ModelOperation& operation : operations_) {
          if (!operation.completed) {
            if (operation.kind == OperationKind::kShutdown) {
              operation.watermark = records_.size();
            }
            Complete(operation);
          }
        }
      }
      if (!worker_stopped_ && processed_ < records_.size() &&
          RouteModel::FreeSlots(records_, config_.destination_slots) != 0U) {
        Consume(records_[processed_]);
        ++processed_;
        progressed = true;
      }
    }
  }

  Config config_;
  std::vector<ProducerState> producers_;
  std::vector<Record> records_;
  std::vector<ModelOperation> operations_;
  std::vector<std::pair<std::size_t, BurstState>> bursts_;
  RouteModel route_;
  Counters rejections_;
  std::size_t central_{0};
  std::size_t priority_available_{0};
  bool last_used_reserve_{false};
  std::size_t processed_{0};
  std::size_t retired_{0};
  std::size_t last_burst_{0};
  bool admission_open_{true};
  bool shutdown_requested_{false};
  bool worker_stopped_{false};
};

}  // namespace ulog::test::admission
