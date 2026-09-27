#include "io/raw_file_sink.hpp"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "control/thread_role.hpp"
#include "encoding/raw_encoder.hpp"

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <uv.h>

namespace ulog::detail::io {
namespace {

/// Ownership of one Runtime-owned frame buffer.
enum class FileSlotState : std::uint8_t { kFree, kReserved, kQueued, kWriting };

/// Worker-visible delivery state. A buffer is reusable only after the worker retires it.
enum class FileDeliveryState : std::uint8_t {
  kRetired,
  kInFlight,
  kDelivered,
  kFailed,
  kCancelled,
};

struct FileSlot final {
  FileSlotState state{FileSlotState::kFree};
  FileDeliveryState delivery{FileDeliveryState::kRetired};
  std::uint64_t generation{0};
  std::uint64_t admission_sequence{0};
  std::size_t encoded_bytes{0};
  std::size_t written_bytes{0};
};

// Owner read/write plus group and other read; Windows applies only the owner write bit.
constexpr int kCreatedFileMode = 0644;
// POSIX file-type bits, which libuv also reports on Windows.
constexpr std::uint64_t kFileTypeMask = 0170000;
constexpr std::uint64_t kDirectoryType = 0040000;

[[nodiscard]] std::uint64_t NextGeneration(std::uint64_t generation) noexcept {
  ++generation;
  return generation == 0U ? 1U : generation;
}

}  // namespace

struct RawFileSinkState final {
  RawFileSinkState(std::string path_utf8, std::size_t write_buffers,
                   std::size_t maximum_record_bytes, FileFaultPlan fault_plan)
      : path(std::move(path_utf8)),
        buffer_count(write_buffers),
        buffer_bytes(encoding::MaximumRawEncodedBytes(maximum_record_bytes)),
        faults(fault_plan),
        buffers(std::make_unique<char[]>(buffer_count * buffer_bytes)),
        slots(std::make_unique<FileSlot[]>(buffer_count)) {}

  [[nodiscard]] char* Buffer(std::size_t index) noexcept {
    return buffers.get() + index * buffer_bytes;
  }

  const std::string path;
  const std::size_t buffer_count;
  const std::size_t buffer_bytes;
  const FileFaultPlan faults;
  std::unique_ptr<char[]> buffers;
  std::unique_ptr<FileSlot[]> slots;

  // libuv structures are used only on the loop thread, except uv_async_send(), which other threads
  // call under `mutex` while `async_open` is true.
  uv_loop_t loop{};
  uv_async_t wake_async{};
  uv_fs_t write_request{};
  uv_fs_t close_request{};
  uv_file file{-1};

  mutable std::mutex mutex;
  std::condition_variable lifecycle;
  route::RouteWake wake{};
  FileSinkStartResult start_result{};
  std::uint64_t write_calls{0};
  std::size_t writing_slot{0};
  std::int32_t failure_error{0};
  std::int32_t close_result{0};
  bool start_reported{false};
  bool loop_exited{false};
  bool async_open{false};
  bool stopping{false};
  bool close_requested{false};
  bool closing{false};
  bool closed{false};
  bool writing{false};
};

namespace {

void ObserveOperation(const FileFaultPlan& faults) noexcept {
  const bool on_loop_thread = control::GetUlogThreadRole() == control::UlogThreadRole::kIo;
  auto* counter = on_loop_thread ? faults.loop_thread_operations : faults.other_thread_operations;
  if (counter != nullptr) {
    counter->fetch_add(1, std::memory_order_relaxed);
  }
}

void SendWakeLocked(RawFileSinkState& state) noexcept {
  if (state.async_open) {
    static_cast<void>(uv_async_send(&state.wake_async));
  }
}

void CancelQueuedLocked(RawFileSinkState& state) noexcept {
  for (std::size_t index = 0; index < state.buffer_count; ++index) {
    auto& slot = state.slots[index];
    if (slot.state == FileSlotState::kQueued) {
      slot.state = FileSlotState::kFree;
      slot.delivery = FileDeliveryState::kCancelled;
    }
  }
}

void PumpLocked(RawFileSinkState& state) noexcept;
void FinishWriteLocked(RawFileSinkState& state, std::int64_t result) noexcept;

void OnWriteComplete(uv_fs_t* request) noexcept {
  auto& state = *static_cast<RawFileSinkState*>(request->data);
  const auto result = static_cast<std::int64_t>(request->result);
  uv_fs_req_cleanup(request);
  std::lock_guard lock{state.mutex};
  FinishWriteLocked(state, result);
}

void StartWriteLocked(RawFileSinkState& state, std::size_t index) noexcept {
  auto& slot = state.slots[index];
  slot.state = FileSlotState::kWriting;
  state.writing = true;
  state.writing_slot = index;

  std::size_t length = slot.encoded_bytes - slot.written_bytes;
  if (state.faults.maximum_write_bytes != 0U && state.faults.maximum_write_bytes < length) {
    length = state.faults.maximum_write_bytes;
  }
  const std::uint64_t call = state.write_calls++;
  ObserveOperation(state.faults);
  if (call == state.faults.failing_write_call) {
    FinishWriteLocked(state, state.faults.write_error);
    return;
  }

  const uv_buf_t buffer =
      uv_buf_init(state.Buffer(index) + slot.written_bytes, static_cast<unsigned int>(length));
  state.write_request.data = &state;
  // Offset -1 appends at the current end; only one append is active, so frames never interleave.
  const int error =
      uv_fs_write(&state.loop, &state.write_request, state.file, &buffer, 1, -1, &OnWriteComplete);
  if (error < 0) {
    FinishWriteLocked(state, error);
  }
}

void FinishWriteLocked(RawFileSinkState& state, std::int64_t result) noexcept {
  auto& slot = state.slots[state.writing_slot];
  state.writing = false;
  const std::size_t remaining = slot.encoded_bytes - slot.written_bytes;
  if (result < 0 || (result == 0 && remaining != 0U)) {
    // The file may hold a partial frame. It is reported and never replayed.
    slot.state = FileSlotState::kFree;
    slot.delivery = FileDeliveryState::kFailed;
    state.failure_error = result < 0 ? static_cast<std::int32_t>(result) : UV_EIO;
    CancelQueuedLocked(state);
    state.wake.Notify();
    PumpLocked(state);
    return;
  }

  const auto written = static_cast<std::size_t>(result);
  slot.written_bytes += written < remaining ? written : remaining;
  if (slot.written_bytes < slot.encoded_bytes) {
    StartWriteLocked(state, state.writing_slot);
    return;
  }
  slot.state = FileSlotState::kFree;
  slot.delivery = FileDeliveryState::kDelivered;
  state.wake.Notify();
  PumpLocked(state);
}

void FinishCloseLocked(RawFileSinkState& state, std::int64_t result) noexcept {
  state.file = -1;
  state.closing = false;
  state.closed = true;
  state.close_result = result < 0 ? static_cast<std::int32_t>(result) : 0;
  state.async_open = false;
  uv_close(reinterpret_cast<uv_handle_t*>(&state.wake_async), nullptr);
  state.wake.Notify();
}

void OnCloseComplete(uv_fs_t* request) noexcept {
  auto& state = *static_cast<RawFileSinkState*>(request->data);
  const auto result = static_cast<std::int64_t>(request->result);
  uv_fs_req_cleanup(request);
  std::lock_guard lock{state.mutex};
  FinishCloseLocked(state, result);
}

void StartCloseLocked(RawFileSinkState& state) noexcept {
  state.closing = true;
  ObserveOperation(state.faults);
  if (state.faults.close_error != 0) {
    uv_fs_t request{};
    static_cast<void>(uv_fs_close(&state.loop, &request, state.file, nullptr));
    uv_fs_req_cleanup(&request);
    FinishCloseLocked(state, state.faults.close_error);
    return;
  }
  state.close_request.data = &state;
  const int error = uv_fs_close(&state.loop, &state.close_request, state.file, &OnCloseComplete);
  if (error < 0) {
    FinishCloseLocked(state, error);
  }
}

void PumpLocked(RawFileSinkState& state) noexcept {
  if (state.writing || state.closing || state.closed) {
    return;
  }
  if (state.failure_error == 0 && !state.stopping) {
    std::optional<std::size_t> selected;
    for (std::size_t index = 0; index < state.buffer_count; ++index) {
      const auto& slot = state.slots[index];
      if (slot.state == FileSlotState::kQueued &&
          (!selected || slot.admission_sequence < state.slots[*selected].admission_sequence)) {
        selected = index;
      }
    }
    if (selected) {
      StartWriteLocked(state, *selected);
      return;
    }
  }
  if (state.close_requested) {
    StartCloseLocked(state);
  }
}

void OnWake(uv_async_t* handle) noexcept {
  auto& state = *static_cast<RawFileSinkState*>(handle->data);
  std::lock_guard lock{state.mutex};
  PumpLocked(state);
}

void ReportStart(RawFileSinkState& state, FileSinkStartResult result) noexcept {
  {
    std::lock_guard lock{state.mutex};
    state.start_result = result;
    state.start_reported = true;
  }
  state.lifecycle.notify_all();
}

void MarkLoopExited(RawFileSinkState& state) noexcept {
  {
    std::lock_guard lock{state.mutex};
    state.loop_exited = true;
  }
  state.lifecycle.notify_all();
}

void RunLoop(RawFileSinkState& state) noexcept {
  const control::ScopedUlogThreadRole io_role{control::UlogThreadRole::kIo};
  const int loop_error = uv_loop_init(&state.loop);
  if (loop_error != 0) {
    ReportStart(state, {.status = FileSinkStartStatus::kLoopFailed, .io_error = loop_error});
    MarkLoopExited(state);
    return;
  }
  state.wake_async.data = &state;
  const int async_error = uv_async_init(&state.loop, &state.wake_async, &OnWake);
  if (async_error != 0) {
    static_cast<void>(uv_loop_close(&state.loop));
    ReportStart(state, {.status = FileSinkStartStatus::kLoopFailed, .io_error = async_error});
    MarkLoopExited(state);
    return;
  }

  int opened = state.faults.open_error;
  ObserveOperation(state.faults);
  if (opened == 0) {
    uv_fs_t request{};
    opened = uv_fs_open(&state.loop, &request, state.path.c_str(),
                        UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_APPEND, kCreatedFileMode, nullptr);
    uv_fs_req_cleanup(&request);
  }
  if (opened >= 0) {
    // Windows can open a directory handle for append, so reject directories on every platform.
    uv_fs_t stat_request{};
    const int stat_error = uv_fs_fstat(&state.loop, &stat_request, opened, nullptr);
    const bool directory =
        stat_error == 0 && (stat_request.statbuf.st_mode & kFileTypeMask) == kDirectoryType;
    uv_fs_req_cleanup(&stat_request);
    if (stat_error < 0 || directory) {
      uv_fs_t close_request{};
      static_cast<void>(uv_fs_close(&state.loop, &close_request, opened, nullptr));
      uv_fs_req_cleanup(&close_request);
      opened = stat_error < 0 ? stat_error : UV_EISDIR;
    }
  }
  if (opened < 0) {
    uv_close(reinterpret_cast<uv_handle_t*>(&state.wake_async), nullptr);
    static_cast<void>(uv_run(&state.loop, UV_RUN_DEFAULT));
    static_cast<void>(uv_loop_close(&state.loop));
    ReportStart(state, {.status = FileSinkStartStatus::kOpenFailed, .io_error = opened});
    MarkLoopExited(state);
    return;
  }

  {
    std::lock_guard lock{state.mutex};
    state.file = opened;
    state.async_open = true;
    state.start_result = {.status = FileSinkStartStatus::kStarted};
    state.start_reported = true;
    if (state.close_requested) {
      // Stop arrived while startup was still opening the file.
      SendWakeLocked(state);
    }
  }
  state.lifecycle.notify_all();
  static_cast<void>(uv_run(&state.loop, UV_RUN_DEFAULT));
  static_cast<void>(uv_loop_close(&state.loop));
  MarkLoopExited(state);
}

}  // namespace

FileWriteClaim::FileWriteClaim(std::shared_ptr<RawFileSinkState> state, std::size_t index,
                               std::uint64_t generation) noexcept
    : state_(std::move(state)), index_(index), generation_(generation) {}

FileWriteClaim::FileWriteClaim(FileWriteClaim&& other) noexcept
    : state_(std::move(other.state_)),
      index_(std::exchange(other.index_, 0)),
      generation_(std::exchange(other.generation_, 0)) {}

FileWriteClaim& FileWriteClaim::operator=(FileWriteClaim&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    index_ = std::exchange(other.index_, 0);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}

FileWriteClaim::~FileWriteClaim() { Reset(); }

void FileWriteClaim::Reset() noexcept {
  if (state_ == nullptr) {
    return;
  }
  const auto state = std::move(state_);
  std::lock_guard lock{state->mutex};
  auto& slot = state->slots[index_];
  if (slot.state == FileSlotState::kReserved && slot.generation == generation_) {
    slot.state = FileSlotState::kFree;
  }
}

route::StoreResult FileWriteClaim::StoreRaw(std::uint64_t admission_sequence,
                                            const producer::RecordView& record) noexcept {
  if (state_ == nullptr) {
    return {};
  }
  auto& state = *state_;
  const auto encoded =
      encoding::EncodeRawRecord(record, std::span<char>{state.Buffer(index_), state.buffer_bytes});
  if (!encoded.complete) {
    Reset();
    return {};
  }

  {
    std::lock_guard lock{state.mutex};
    auto& slot = state.slots[index_];
    if (slot.state != FileSlotState::kReserved || slot.generation != generation_) {
      return {};
    }
    slot.admission_sequence = admission_sequence;
    slot.encoded_bytes = encoded.encoded_bytes;
    slot.written_bytes = 0;
    if (state.stopping || state.failure_error != 0 || !state.async_open) {
      slot.state = FileSlotState::kFree;
      slot.delivery = FileDeliveryState::kCancelled;
    } else {
      slot.state = FileSlotState::kQueued;
      slot.delivery = FileDeliveryState::kInFlight;
      SendWakeLocked(state);
    }
  }
  state_.reset();
  return {.encoded_bytes = encoded.encoded_bytes, .submitted = true};
}

RawFileSink::RawFileSink(std::string path_utf8, std::size_t write_buffers,
                         std::size_t maximum_record_bytes, FileFaultPlan faults)
    : state_(std::make_shared<RawFileSinkState>(std::move(path_utf8), write_buffers,
                                                maximum_record_bytes, faults)) {}

RawFileSink::~RawFileSink() {
  Stop();
  DetachWake();
  if (!thread_.joinable()) {
    return;
  }
  bool exited = false;
  {
    std::lock_guard lock{state_->mutex};
    exited = state_->loop_exited;
  }
  if (exited) {
    thread_.join();
  } else {
    // The loop thread owns a shared state reference and exits after its active append closes.
    thread_.detach();
  }
}

FileSinkStartResult RawFileSink::Start(route::RouteWake wake,
                                       std::chrono::steady_clock::time_point deadline) noexcept {
  {
    std::lock_guard lock{state_->mutex};
    state_->wake = wake;
  }
  try {
    thread_ = std::thread{[state = state_]() noexcept { RunLoop(*state); }};
  } catch (const std::system_error&) {
    return {.status = FileSinkStartStatus::kThreadFailed};
  }
  std::unique_lock lock{state_->mutex};
  if (!state_->lifecycle.wait_until(lock, deadline, [this] { return state_->start_reported; })) {
    return {.status = FileSinkStartStatus::kTimedOut};
  }
  return state_->start_result;
}

FileWriteClaim RawFileSink::TryClaimWrite() noexcept {
  std::lock_guard lock{state_->mutex};
  if (state_->stopping || state_->close_requested || state_->failure_error != 0 ||
      !state_->async_open) {
    return {};
  }
  for (std::size_t index = 0; index < state_->buffer_count; ++index) {
    auto& slot = state_->slots[index];
    if (slot.state == FileSlotState::kFree && slot.delivery == FileDeliveryState::kRetired) {
      slot.generation = NextGeneration(slot.generation);
      slot.state = FileSlotState::kReserved;
      return FileWriteClaim{state_, index, slot.generation};
    }
  }
  return {};
}

std::optional<route::DeliveryRetirement> RawFileSink::TryRetire(
    std::uint64_t admission_sequence) noexcept {
  std::lock_guard lock{state_->mutex};
  for (std::size_t index = 0; index < state_->buffer_count; ++index) {
    auto& slot = state_->slots[index];
    const bool completed = slot.delivery == FileDeliveryState::kDelivered ||
                           slot.delivery == FileDeliveryState::kFailed;
    if (!completed || slot.admission_sequence != admission_sequence) {
      continue;
    }
    const route::DeliveryRetirement retirement{
        .outcome = slot.delivery == FileDeliveryState::kDelivered
                       ? route::DeliveryOutcome::kDelivered
                       : route::DeliveryOutcome::kFailed,
        .encoded_bytes = slot.encoded_bytes,
    };
    slot.delivery = FileDeliveryState::kRetired;
    return retirement;
  }
  return std::nullopt;
}

route::InFlightSummary RawFileSink::SummarizeInFlight(std::uint64_t watermark) const noexcept {
  route::InFlightSummary summary;
  std::lock_guard lock{state_->mutex};
  for (std::size_t index = 0; index < state_->buffer_count; ++index) {
    const auto& slot = state_->slots[index];
    if (slot.delivery == FileDeliveryState::kRetired || slot.admission_sequence >= watermark) {
      continue;
    }
    ++summary.records;
    summary.bytes += slot.encoded_bytes;
    if (slot.delivery == FileDeliveryState::kDelivered) {
      ++summary.delivered_records;
      summary.delivered_bytes += slot.encoded_bytes;
    } else if (slot.delivery == FileDeliveryState::kFailed) {
      ++summary.failed_records;
      summary.failed_bytes += slot.encoded_bytes;
    }
  }
  return summary;
}

std::int32_t RawFileSink::FailureError() const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->failure_error;
}

void RawFileSink::Stop() noexcept {
  std::lock_guard lock{state_->mutex};
  state_->stopping = true;
  state_->close_requested = true;
  CancelQueuedLocked(*state_);
  SendWakeLocked(*state_);
}

void RawFileSink::RequestClose() noexcept {
  std::lock_guard lock{state_->mutex};
  state_->close_requested = true;
  SendWakeLocked(*state_);
}

std::optional<std::int32_t> RawFileSink::CloseResult() const noexcept {
  std::lock_guard lock{state_->mutex};
  if (!state_->closed) {
    return std::nullopt;
  }
  return state_->close_result;
}

void RawFileSink::DiscardInFlight() noexcept {
  std::lock_guard lock{state_->mutex};
  for (std::size_t index = 0; index < state_->buffer_count; ++index) {
    state_->slots[index].delivery = FileDeliveryState::kRetired;
  }
}

void RawFileSink::DetachWake() noexcept {
  std::lock_guard lock{state_->mutex};
  state_->wake = {};
}

bool RawFileSink::JoinUntil(std::chrono::steady_clock::time_point deadline) noexcept {
  if (!thread_.joinable()) {
    return true;
  }
  bool exited = false;
  {
    std::unique_lock lock{state_->mutex};
    exited = state_->lifecycle.wait_until(lock, deadline, [this] { return state_->loop_exited; });
  }
  if (exited) {
    thread_.join();
  } else {
    thread_.detach();
  }
  return exited;
}

std::size_t RawFileSink::FixedBackingBytes() const noexcept {
  return state_->buffer_count * (state_->buffer_bytes + sizeof(FileSlot)) +
         sizeof(RawFileSinkState);
}

}  // namespace ulog::detail::io
