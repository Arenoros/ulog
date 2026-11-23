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
// Owner all plus group and other read/search, reduced by the process umask; ignored on Windows.
constexpr int kCreatedDirectoryMode = 0755;
// POSIX file-type bits, which libuv also reports on Windows.
constexpr std::uint64_t kFileTypeMask = 0170000;
constexpr std::uint64_t kDirectoryType = 0040000;

[[nodiscard]] std::uint64_t NextGeneration(std::uint64_t generation) noexcept {
  ++generation;
  return generation == 0U ? 1U : generation;
}

}  // namespace

struct RawFileSinkState final {
  explicit RawFileSinkState(FileSinkConfig config)
      : path(std::move(config.path_utf8)),
        directory_scratch(path),
        create_parent_directories(config.create_parent_directories),
        truncate_on_start(config.truncate_on_start),
        buffer_count(config.write_buffers),
        buffer_bytes(encoding::MaximumRawEncodedBytes(config.maximum_record_bytes)),
        faults(config.faults),
        buffers(std::make_unique<char[]>(buffer_count * buffer_bytes)),
        slots(std::make_unique<FileSlot[]>(buffer_count)) {}

  [[nodiscard]] char* Buffer(std::size_t index) noexcept {
    return buffers.get() + index * buffer_bytes;
  }

  const std::string path;
  /// Copy of `path` that directory creation terminates at each ancestor, allocated before Start().
  std::string directory_scratch;
  const bool create_parent_directories;
  const bool truncate_on_start;
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
  uv_fs_t sync_request{};
  uv_file file{-1};

  mutable std::mutex mutex;
  std::condition_variable lifecycle;
  route::RouteWake wake{};
  FileSinkStartResult start_result{};
  std::uint64_t write_calls{0};
  /// Sync tickets: requested by the worker, covered by the active sync, and completed.
  std::uint64_t sync_requested{0};
  std::uint64_t sync_target{0};
  std::uint64_t sync_completed{0};
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
  bool syncing{false};
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

void FinishSyncLocked(RawFileSinkState& state, std::int64_t result) noexcept {
  state.syncing = false;
  if (result < 0) {
    // A failed fsync may have dropped dirty pages, so a later success cannot be trusted.
    state.failure_error = static_cast<std::int32_t>(result);
    CancelQueuedLocked(state);
  } else {
    state.sync_completed = state.sync_target;
  }
  state.wake.Notify();
  PumpLocked(state);
}

void OnSyncComplete(uv_fs_t* request) noexcept {
  auto& state = *static_cast<RawFileSinkState*>(request->data);
  const auto result = static_cast<std::int64_t>(request->result);
  uv_fs_req_cleanup(request);
  std::lock_guard lock{state.mutex};
  FinishSyncLocked(state, result);
}

/// Starts one fsync while no append is active, so it covers every completed append.
void StartSyncLocked(RawFileSinkState& state) noexcept {
  state.syncing = true;
  state.sync_target = state.sync_requested;
  ObserveOperation(state.faults);
  if (state.faults.sync_operations != nullptr) {
    state.faults.sync_operations->fetch_add(1, std::memory_order_relaxed);
  }
  if (state.faults.sync_error != 0) {
    FinishSyncLocked(state, state.faults.sync_error);
    return;
  }
  state.sync_request.data = &state;
  const int error = uv_fs_fsync(&state.loop, &state.sync_request, state.file, &OnSyncComplete);
  if (error < 0) {
    FinishSyncLocked(state, error);
  }
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
  if (state.writing || state.syncing || state.closing || state.closed) {
    return;
  }
  if (state.failure_error == 0 && !state.stopping) {
    if (state.sync_completed < state.sync_requested) {
      StartSyncLocked(state);
      return;
    }
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

[[nodiscard]] bool IsPathSeparator(char character) noexcept {
#if defined(_WIN32)
  return character == '/' || character == '\\';
#else
  return character == '/';
#endif
}

/// Returns whether `path[end]` ends an ancestor: a separator that follows a non-separator.
[[nodiscard]] bool EndsAncestor(const std::string& path, std::size_t end) noexcept {
  return end > 0U && IsPathSeparator(path[end]) && !IsPathSeparator(path[end - 1U]);
}

[[nodiscard]] std::size_t PreviousAncestorEnd(const std::string& path,
                                              std::size_t before) noexcept {
  for (std::size_t end = before; end-- > 0U;) {
    if (EndsAncestor(path, end)) {
      return end;
    }
  }
  return std::string::npos;
}

[[nodiscard]] std::size_t NextAncestorEnd(const std::string& path, std::size_t after) noexcept {
  for (std::size_t end = after + 1U; end < path.size(); ++end) {
    if (EndsAncestor(path, end)) {
      return end;
    }
  }
  return std::string::npos;
}

[[nodiscard]] int MakeAncestor(RawFileSinkState& state, std::size_t end) noexcept {
  ObserveOperation(state.faults);
  if (state.faults.directory_error != 0) {
    return state.faults.directory_error;
  }
  auto& scratch = state.directory_scratch;
  scratch[end] = '\0';
  uv_fs_t request{};
  const int error =
      uv_fs_mkdir(&state.loop, &request, scratch.c_str(), kCreatedDirectoryMode, nullptr);
  uv_fs_req_cleanup(&request);
  scratch[end] = state.path[end];
  return error;
}

/// Creates the missing ancestors of the file path. It walks up from the parent only while an
/// ancestor is missing, so existing directories are never modified.
[[nodiscard]] int CreateParentDirectories(RawFileSinkState& state) noexcept {
  const std::string& path = state.path;
  std::size_t end = PreviousAncestorEnd(path, path.size());
  int error = UV_ENOENT;
  while (end != std::string::npos) {
    error = MakeAncestor(state, end);
    if (error != UV_ENOENT) {
      break;
    }
    end = PreviousAncestorEnd(path, end);
  }
  if (error != 0 && error != UV_EEXIST) {
    return error;
  }
  for (end = NextAncestorEnd(path, end); end != std::string::npos;
       end = NextAncestorEnd(path, end)) {
    error = MakeAncestor(state, end);
    if (error != 0 && error != UV_EEXIST) {
      return error;
    }
  }
  return 0;
}

[[nodiscard]] int OpenForAppend(RawFileSinkState& state) noexcept {
  ObserveOperation(state.faults);
  if (state.faults.open_error != 0) {
    return state.faults.open_error;
  }
  const int truncate = state.truncate_on_start ? UV_FS_O_TRUNC : 0;
  uv_fs_t request{};
  const int opened = uv_fs_open(&state.loop, &request, state.path.c_str(),
                                UV_FS_O_WRONLY | UV_FS_O_CREAT | UV_FS_O_APPEND | truncate,
                                kCreatedFileMode, nullptr);
  uv_fs_req_cleanup(&request);
  return opened;
}

void CloseSynchronously(RawFileSinkState& state, uv_file file) noexcept {
  ObserveOperation(state.faults);
  uv_fs_t request{};
  static_cast<void>(uv_fs_close(&state.loop, &request, file, nullptr));
  uv_fs_req_cleanup(&request);
}

/// Reads the last existing byte through a read-only handle, because the append handle cannot
/// read. Content that cannot be read counts as not ending in a newline.
[[nodiscard]] bool EndsWithNewline(RawFileSinkState& state, std::uint64_t size) noexcept {
  ObserveOperation(state.faults);
  uv_fs_t request{};
  const int reader =
      uv_fs_open(&state.loop, &request, state.path.c_str(), UV_FS_O_RDONLY, 0, nullptr);
  uv_fs_req_cleanup(&request);
  if (reader < 0) {
    return false;
  }
  char last = '\0';
  const uv_buf_t buffer = uv_buf_init(&last, 1);
  ObserveOperation(state.faults);
  const int read = uv_fs_read(&state.loop, &request, reader, &buffer, 1,
                              static_cast<std::int64_t>(size - 1U), nullptr);
  uv_fs_req_cleanup(&request);
  CloseSynchronously(state, reader);
  return read == 1 && last == '\n';
}

/// Starts the first new frame on its own line after existing content that does not end with one.
[[nodiscard]] int WriteSeparator(RawFileSinkState& state, uv_file file) noexcept {
  char newline = '\n';
  const uv_buf_t buffer = uv_buf_init(&newline, 1);
  ObserveOperation(state.faults);
  uv_fs_t request{};
  const int written = uv_fs_write(&state.loop, &request, file, &buffer, 1, -1, nullptr);
  uv_fs_req_cleanup(&request);
  if (written < 0) {
    return written;
  }
  return written == 1 ? 0 : UV_EIO;
}

/// Opens the route file for append, creating ancestors and separating existing content.
[[nodiscard]] FileSinkStartResult OpenRouteFile(RawFileSinkState& state, uv_file& file) noexcept {
  int opened = OpenForAppend(state);
  if (opened == UV_ENOENT && state.create_parent_directories) {
    const int created = CreateParentDirectories(state);
    if (created != 0) {
      return {.status = FileSinkStartStatus::kDirectoryCreateFailed, .io_error = created};
    }
    opened = OpenForAppend(state);
  }
  if (opened < 0) {
    return {.status = FileSinkStartStatus::kOpenFailed, .io_error = opened};
  }

  // Windows can open a directory handle for append, so reject directories on every platform.
  uv_fs_t stat_request{};
  int error = uv_fs_fstat(&state.loop, &stat_request, opened, nullptr);
  const bool directory =
      error == 0 && (stat_request.statbuf.st_mode & kFileTypeMask) == kDirectoryType;
  const std::uint64_t size = stat_request.statbuf.st_size;
  uv_fs_req_cleanup(&stat_request);
  if (error == 0 && directory) {
    error = UV_EISDIR;
  } else if (error == 0 && size != 0U && !EndsWithNewline(state, size)) {
    error = WriteSeparator(state, opened);
  }
  if (error < 0) {
    CloseSynchronously(state, opened);
    return {.status = FileSinkStartStatus::kOpenFailed, .io_error = error};
  }
  file = opened;
  return {.status = FileSinkStartStatus::kStarted};
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

  uv_file opened = -1;
  const FileSinkStartResult open_result = OpenRouteFile(state, opened);
  if (open_result.status != FileSinkStartStatus::kStarted) {
    uv_close(reinterpret_cast<uv_handle_t*>(&state.wake_async), nullptr);
    static_cast<void>(uv_run(&state.loop, UV_RUN_DEFAULT));
    static_cast<void>(uv_loop_close(&state.loop));
    ReportStart(state, open_result);
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

FileWriteClaim::FileWriteClaim(std::shared_ptr<RawFileSinkState> state,
                               FileSlotIdentity identity) noexcept
    : state_(std::move(state)), identity_(identity) {}

FileWriteClaim::FileWriteClaim(FileWriteClaim&& other) noexcept
    : state_(std::move(other.state_)), identity_(std::exchange(other.identity_, {})) {}

FileWriteClaim& FileWriteClaim::operator=(FileWriteClaim&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::move(other.state_);
    identity_ = std::exchange(other.identity_, {});
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
  auto& slot = state->slots[identity_.index];
  if (slot.state == FileSlotState::kReserved && slot.generation == identity_.generation) {
    slot.state = FileSlotState::kFree;
  }
}

route::StoreResult FileWriteClaim::StoreRaw(std::uint64_t admission_sequence,
                                            const producer::RecordView& record) noexcept {
  if (state_ == nullptr) {
    return {};
  }
  auto& state = *state_;
  const auto encoded = encoding::EncodeRawRecord(
      record, std::span<char>{state.Buffer(identity_.index), state.buffer_bytes});
  if (!encoded.complete) {
    Reset();
    return {};
  }

  {
    std::lock_guard lock{state.mutex};
    auto& slot = state.slots[identity_.index];
    if (slot.state != FileSlotState::kReserved || slot.generation != identity_.generation) {
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

RawFileSink::RawFileSink(FileSinkConfig config)
    : state_(std::make_shared<RawFileSinkState>(std::move(config))) {}

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
      return FileWriteClaim{state_, {.index = index, .generation = slot.generation}};
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

std::uint64_t RawFileSink::RequestSync() noexcept {
  std::lock_guard lock{state_->mutex};
  ++state_->sync_requested;
  SendWakeLocked(*state_);
  return state_->sync_requested;
}

bool RawFileSink::SyncCompleted(std::uint64_t ticket) const noexcept {
  std::lock_guard lock{state_->mutex};
  return state_->sync_completed >= ticket;
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
