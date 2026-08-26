#include "control/control_reserve.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

#include "control/operation_access.hpp"
#include "control/thread_role.hpp"

namespace ulog::detail::control {
namespace {

constexpr std::size_t kNoFreeNode = std::numeric_limits<std::size_t>::max();
enum class CallbackState : std::uint8_t {
  kEmpty,
  kInstalling,
  kArmed,
  kQueued,
  kRunning,
  kDone,
};

class CallbackTaskQueue final {
 public:
  void Push(CallbackTask& task) noexcept {
    task.next = nullptr;
    if (tail_ == nullptr) {
      head_ = &task;
    } else {
      tail_->next = &task;
    }
    tail_ = &task;
  }

  [[nodiscard]] CallbackTask* Pop() noexcept {
    if (head_ == nullptr) {
      return nullptr;
    }
    CallbackTask* task = head_;
    head_ = task->next;
    if (head_ == nullptr) {
      tail_ = nullptr;
    }
    task->next = nullptr;
    return task;
  }

  [[nodiscard]] bool Empty() const noexcept { return head_ == nullptr; }

 private:
  CallbackTask* head_{nullptr};
  CallbackTask* tail_{nullptr};
};

}  // namespace

struct ThreadedDispatcherState final {
  std::mutex mutex;
  std::condition_variable ready_condition;
  CallbackTaskQueue tasks;
  bool stopping{false};
};

class ThreadedCallbackDispatcher final {
 public:
  ThreadedCallbackDispatcher();
  ~ThreadedCallbackDispatcher();

  ThreadedCallbackDispatcher(const ThreadedCallbackDispatcher&) = delete;
  ThreadedCallbackDispatcher& operator=(const ThreadedCallbackDispatcher&) = delete;

  void Enqueue(CallbackTask& task) noexcept;

 private:
  static void Run(std::shared_ptr<ThreadedDispatcherState> state) noexcept;

  std::shared_ptr<ThreadedDispatcherState> state_;
  std::thread thread_;
};

struct ControlNode final {
  std::mutex mutex;
  std::condition_variable completed_condition;
  std::atomic<bool> ready{false};
  OperationResult result{};
  std::shared_ptr<ControlDomain> owner;
  std::uint64_t generation{0};
  std::size_t owners{0};
  std::size_t next_free{kNoFreeNode};
  CallbackTask callback_task{};
  OperationCallback callback{};
  CallbackState callback_state{CallbackState::kEmpty};
  bool completed{false};
};

struct ControlDomain final {
  ControlDomain(const std::size_t requested_capacity, CallbackDispatcher dispatcher)
      : nodes(std::make_unique<ControlNode[]>(requested_capacity)),
        capacity(requested_capacity),
        callback_dispatcher(std::move(dispatcher)) {
    for (std::size_t index = 0; index < capacity; ++index) {
      nodes[index].next_free = index + 1U < capacity ? index + 1U : kNoFreeNode;
    }
  }

  void Recycle(ControlNode& node) noexcept {
    {
      std::lock_guard node_lock{node.mutex};
      node.completed = false;
      node.ready.store(false, std::memory_order_relaxed);
      node.result = {};
      node.owners = 0;
      node.callback_task.next = nullptr;
      node.callback = {};
      node.callback_state = CallbackState::kEmpty;
      node.owner.reset();
    }

    const std::size_t index = static_cast<std::size_t>(&node - nodes.get());
    std::lock_guard domain_lock{mutex};
    node.next_free = free_head;
    free_head = index;
    --in_use;
  }

  std::unique_ptr<ControlNode[]> nodes;
  const std::size_t capacity;
  mutable std::mutex mutex;
  std::size_t free_head{0};
  std::size_t in_use{0};
  CallbackDispatcher callback_dispatcher;
};

namespace {

void ReleaseOwner(ControlNode& node, const std::uint64_t generation) noexcept {
  std::shared_ptr<ControlDomain> owner;
  {
    std::lock_guard lock{node.mutex};
    if (node.generation != generation || node.owners == 0) {
      return;
    }
    --node.owners;
    if (node.owners != 0) {
      return;
    }
    owner = node.owner;
  }
  owner->Recycle(node);
}

void RunCallback(ControlNode& node) noexcept {
  const ScopedUlogThreadRole callback_role{UlogThreadRole::kCallback};
  OperationResult result;
  std::uint64_t generation = 0;
  {
    std::lock_guard lock{node.mutex};
    if (node.callback_state != CallbackState::kQueued) {
      return;
    }
    node.callback_state = CallbackState::kRunning;
    result = node.result;
    generation = node.generation;
  }
  OperationCallbackAccess::Invoke(node.callback, result);
  OperationCallbackAccess::Reset(node.callback);
  {
    std::lock_guard lock{node.mutex};
    node.callback_state = CallbackState::kDone;
  }
  ReleaseOwner(node, generation);
}

void RunCallbackTask(void* const context) noexcept {
  RunCallback(*static_cast<ControlNode*>(context));
}

}  // namespace

ThreadedCallbackDispatcher::ThreadedCallbackDispatcher()
    : state_(std::make_shared<ThreadedDispatcherState>()),
      thread_{[state = state_]() mutable { Run(std::move(state)); }} {}

ThreadedCallbackDispatcher::~ThreadedCallbackDispatcher() {
  {
    std::lock_guard lock{state_->mutex};
    state_->stopping = true;
  }
  state_->ready_condition.notify_one();
  if (thread_.get_id() == std::this_thread::get_id()) {
    thread_.detach();
  } else {
    thread_.join();
  }
}

void ThreadedCallbackDispatcher::Enqueue(CallbackTask& task) noexcept {
  {
    std::lock_guard lock{state_->mutex};
    state_->tasks.Push(task);
  }
  state_->ready_condition.notify_one();
}

void ThreadedCallbackDispatcher::Run(std::shared_ptr<ThreadedDispatcherState> state) noexcept {
  while (true) {
    CallbackTask* task = nullptr;
    {
      std::unique_lock lock{state->mutex};
      state->ready_condition.wait(lock,
                                  [&state] { return state->stopping || !state->tasks.Empty(); });
      if (state->tasks.Empty()) {
        return;
      }
      task = state->tasks.Pop();
    }
    task->run(task->context);
  }
}

namespace {

[[nodiscard]] CallbackDispatcher MakeThreadedCallbackDispatcher() {
  auto dispatcher = std::make_shared<ThreadedCallbackDispatcher>();
  return {
      .lifetime = dispatcher,
      .context = dispatcher.get(),
      .enqueue =
          [](void* context, CallbackTask& task) noexcept {
            static_cast<ThreadedCallbackDispatcher*>(context)->Enqueue(task);
          },
      .fixed_state_bytes = sizeof(ThreadedCallbackDispatcher) + sizeof(ThreadedDispatcherState)};
}

OperationPollResult PollNode(const void* const state, const std::uint64_t generation) noexcept {
  const auto& node = *static_cast<const ControlNode*>(state);
  if (node.generation != generation) {
    return {};
  }
  if (!node.ready.load(std::memory_order_acquire)) {
    return {OperationPollStatus::kPending, std::nullopt};
  }
  return {OperationPollStatus::kCompleted, node.result};
}

OperationWaitResult WaitForNode(void* const state, const std::uint64_t generation,
                                const std::chrono::steady_clock::time_point deadline) noexcept {
  auto& node = *static_cast<ControlNode*>(state);
  if (node.ready.load(std::memory_order_acquire)) {
    return {OperationWaitStatus::kCompleted, node.result};
  }
  if (GetUlogThreadRole() != UlogThreadRole::kExternal) {
    return {OperationWaitStatus::kForbiddenThread, std::nullopt};
  }

  try {
    std::unique_lock lock{node.mutex};
    if (node.generation != generation) {
      return {};
    }
    if (!node.completed_condition.wait_until(lock, deadline, [&node] { return node.completed; })) {
      return {OperationWaitStatus::kDeadlineExceeded, std::nullopt};
    }
    return {OperationWaitStatus::kCompleted, node.result};
  } catch (const std::system_error&) {
    return {OperationWaitStatus::kWaitFailed, std::nullopt};
  }
}

void ReleaseHandle(void* const state, const std::uint64_t generation) noexcept {
  ReleaseOwner(*static_cast<ControlNode*>(state), generation);
}

OperationCallbackResult RegisterCallback(void* const state, const std::uint64_t generation,
                                         OperationCallback&& callback) noexcept {
  if (!callback) {
    return {OperationCallbackStatus::kInvalidCallback};
  }

  auto& node = *static_cast<ControlNode*>(state);
  std::shared_ptr<ControlDomain> owner;
  bool enqueue = false;
  {
    std::lock_guard lock{node.mutex};
    if (node.generation != generation) {
      return {OperationCallbackStatus::kInvalidOperation};
    }
    if (node.callback_state != CallbackState::kEmpty) {
      return {OperationCallbackStatus::kAlreadyRegistered};
    }
    node.callback_state = CallbackState::kInstalling;
    // Pin the node while user-defined move behavior runs without the state lock.
    ++node.owners;
  }

  node.callback = std::move(callback);
  {
    std::lock_guard lock{node.mutex};
    node.callback_state = CallbackState::kArmed;
    if (node.completed) {
      node.callback_state = CallbackState::kQueued;
      ++node.owners;
      owner = node.owner;
      enqueue = true;
    }
  }
  if (enqueue) {
    owner->callback_dispatcher.Enqueue(node.callback_task);
  }
  ReleaseOwner(node, generation);
  return {OperationCallbackStatus::kRegistered};
}

constexpr OperationVTable kOperationVTable{&PollNode, &WaitForNode, &RegisterCallback,
                                           &ReleaseHandle};

}  // namespace

OperationCompletion::OperationCompletion(OperationCompletion&& other) noexcept
    : node_(std::exchange(other.node_, nullptr)),
      generation_(std::exchange(other.generation_, 0)) {}

OperationCompletion& OperationCompletion::operator=(OperationCompletion&& other) noexcept {
  if (this != &other) {
    if (node_ != nullptr) {
      static_cast<void>(TryComplete(OperationOutcome::kCancelled));
    }
    node_ = std::exchange(other.node_, nullptr);
    generation_ = std::exchange(other.generation_, 0);
  }
  return *this;
}

OperationCompletion::~OperationCompletion() {
  if (node_ != nullptr) {
    static_cast<void>(TryComplete(OperationOutcome::kCancelled));
  }
}

bool OperationCompletion::TryComplete(const OperationOutcome outcome) noexcept {
  if (node_ == nullptr) {
    return false;
  }

  ControlNode& node = *std::exchange(node_, nullptr);
  const std::uint64_t generation = std::exchange(generation_, 0);
  bool completed = false;
  bool enqueue_callback = false;
  std::shared_ptr<ControlDomain> owner;
  {
    std::lock_guard lock{node.mutex};
    if (node.generation == generation && !node.completed) {
      node.result = OperationResultAccess::Make(outcome);
      node.completed = true;
      node.ready.store(true, std::memory_order_release);
      if (node.callback_state == CallbackState::kArmed) {
        node.callback_state = CallbackState::kQueued;
        ++node.owners;
        owner = node.owner;
        enqueue_callback = true;
      }
      completed = true;
    }
  }
  if (completed) {
    node.completed_condition.notify_all();
  }
  if (enqueue_callback) {
    owner->callback_dispatcher.Enqueue(node.callback_task);
  }
  ReleaseOwner(node, generation);
  return completed;
}

struct ManualCallbackDispatcher::State final {
  std::mutex mutex;
  CallbackTaskQueue tasks;
};

ManualCallbackDispatcher::ManualCallbackDispatcher() : state_(std::make_shared<State>()) {}

ManualCallbackDispatcher::~ManualCallbackDispatcher() = default;

CallbackDispatcher ManualCallbackDispatcher::GetDispatcher() const noexcept {
  return {.lifetime = state_,
          .context = state_.get(),
          .enqueue = &ManualCallbackDispatcher::EnqueueTask,
          .fixed_state_bytes = sizeof(State)};
}

bool ManualCallbackDispatcher::RunOne() noexcept {
  CallbackTask* task = nullptr;
  {
    std::lock_guard lock{state_->mutex};
    if (state_->tasks.Empty()) {
      return false;
    }
    task = state_->tasks.Pop();
  }
  task->run(task->context);
  return true;
}

void ManualCallbackDispatcher::EnqueueTask(void* const context, CallbackTask& task) noexcept {
  auto& state = *static_cast<State*>(context);
  std::lock_guard lock{state.mutex};
  state.tasks.Push(task);
}

ControlReserve::ControlReserve(const std::size_t capacity)
    : ControlReserve(capacity, MakeThreadedCallbackDispatcher()) {}

ControlReserve::ControlReserve(const std::size_t capacity, CallbackDispatcher dispatcher) {
  if (capacity == 0) {
    throw std::invalid_argument{
        "control reserve capacity must be at least 1; configure a positive control-operation "
        "capacity"};
  }
  if (!dispatcher) {
    throw std::invalid_argument{
        "control reserve callback dispatcher is empty; provide an owned non-blocking dispatcher"};
  }
  domain_ = std::make_shared<ControlDomain>(capacity, std::move(dispatcher));
}

ControlReserve::~ControlReserve() = default;

ControlStartResult ControlReserve::TryStart() noexcept {
  std::lock_guard lock{domain_->mutex};
  if (domain_->free_head == kNoFreeNode) {
    return {.failure = OperationStartFailure{OperationStartErrorCode::kControlReserveExhausted,
                                             domain_->capacity, domain_->in_use}};
  }

  ControlNode& node = domain_->nodes[domain_->free_head];
  domain_->free_head = node.next_free;
  ++domain_->in_use;
  ++node.generation;
  node.owners = 2;
  node.completed = false;
  node.ready.store(false, std::memory_order_relaxed);
  node.result = {};
  node.callback = {};
  node.callback_state = CallbackState::kEmpty;
  node.callback_task = {&node, &RunCallbackTask, nullptr};
  node.owner = domain_;
  node.next_free = kNoFreeNode;

  return {.operation = OperationAccess::Make(&node, node.generation, kOperationVTable),
          .completion = OperationCompletion{node, node.generation}};
}

ControlReserveSnapshot ControlReserve::GetSnapshot() const noexcept {
  std::lock_guard lock{domain_->mutex};
  return {.capacity = domain_->capacity,
          .in_use = domain_->in_use,
          .node_backing_bytes = domain_->capacity * sizeof(ControlNode),
          .dispatcher_state_bytes = domain_->callback_dispatcher.fixed_state_bytes};
}

}  // namespace ulog::detail::control
