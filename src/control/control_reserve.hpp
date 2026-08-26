#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ulog/operation.hpp>

namespace ulog::detail::control {

struct ControlNode;
struct ControlDomain;

struct CallbackTask final {
  void* context{nullptr};
  void (*run)(void*) noexcept {nullptr};
  CallbackTask* next{nullptr};
};

struct CallbackDispatcher final {
  std::shared_ptr<void> lifetime;
  void* context{nullptr};
  void (*enqueue)(void*, CallbackTask&) noexcept {nullptr};
  std::size_t fixed_state_bytes{0};

  [[nodiscard]] explicit operator bool() const noexcept {
    return lifetime != nullptr && context != nullptr && enqueue != nullptr;
  }
  void Enqueue(CallbackTask& task) const noexcept { enqueue(context, task); }
};

class ManualCallbackDispatcher final {
 public:
  ManualCallbackDispatcher();
  ~ManualCallbackDispatcher();

  ManualCallbackDispatcher(const ManualCallbackDispatcher&) = delete;
  ManualCallbackDispatcher& operator=(const ManualCallbackDispatcher&) = delete;

  [[nodiscard]] CallbackDispatcher GetDispatcher() const noexcept;
  [[nodiscard]] bool RunOne() noexcept;

 private:
  struct State;
  static void EnqueueTask(void* context, CallbackTask& task) noexcept;

  std::shared_ptr<State> state_;
};

class OperationCompletion final {
 public:
  OperationCompletion() noexcept = default;
  OperationCompletion(OperationCompletion&& other) noexcept;
  OperationCompletion& operator=(OperationCompletion&& other) noexcept;
  OperationCompletion(const OperationCompletion&) = delete;
  OperationCompletion& operator=(const OperationCompletion&) = delete;
  ~OperationCompletion();

  [[nodiscard]] explicit operator bool() const noexcept { return node_ != nullptr; }
  [[nodiscard]] bool TryComplete(OperationOutcome outcome) noexcept;

 private:
  friend class ControlReserve;
  OperationCompletion(ControlNode& node, std::uint64_t generation) noexcept
      : node_(&node), generation_(generation) {}

  ControlNode* node_{nullptr};
  std::uint64_t generation_{0};
};

struct ControlStartResult final {
  Operation operation{};
  OperationCompletion completion{};
  std::optional<OperationStartFailure> failure{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return !failure.has_value() && static_cast<bool>(operation) && static_cast<bool>(completion);
  }
};

struct ControlReserveSnapshot final {
  std::size_t capacity{0};
  std::size_t in_use{0};
  std::size_t node_backing_bytes{0};
  std::size_t dispatcher_state_bytes{0};
};

class ControlReserve final {
 public:
  explicit ControlReserve(std::size_t capacity);
  ControlReserve(std::size_t capacity, CallbackDispatcher dispatcher);
  ~ControlReserve();

  ControlReserve(const ControlReserve&) = delete;
  ControlReserve& operator=(const ControlReserve&) = delete;
  ControlReserve(ControlReserve&&) = delete;
  ControlReserve& operator=(ControlReserve&&) = delete;

  [[nodiscard]] ControlStartResult TryStart() noexcept;
  [[nodiscard]] ControlReserveSnapshot GetSnapshot() const noexcept;

 private:
  std::shared_ptr<ControlDomain> domain_;
};

}  // namespace ulog::detail::control
