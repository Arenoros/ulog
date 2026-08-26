#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <type_traits>
#include <ulog/export.hpp>
#include <utility>

namespace ulog {

enum class OperationOutcome : std::uint8_t { kSucceeded, kCancelled, kFailed };

namespace detail {
struct OperationAccess;
struct OperationCallbackAccess;
struct OperationResultAccess;
struct OperationVTable;
}  // namespace detail

class OperationResult final {
 public:
  constexpr OperationResult() noexcept = default;

  /// Returns the immutable terminal outcome published for this Operation.
  [[nodiscard]] constexpr OperationOutcome Outcome() const noexcept {
    return static_cast<OperationOutcome>(storage_);
  }

  friend constexpr bool operator==(const OperationResult&,
                                   const OperationResult&) noexcept = default;

 private:
  friend struct detail::OperationResultAccess;
  explicit constexpr OperationResult(const OperationOutcome outcome) noexcept
      : storage_{static_cast<std::uintptr_t>(outcome)} {}

  std::uintptr_t storage_{static_cast<std::uintptr_t>(OperationOutcome::kFailed)};
};

inline constexpr std::size_t kOperationCallbackInlineBytes = 64;

class OperationCallback final {
 public:
  OperationCallback() noexcept = default;

  /// Owns one small noexcept callable without allocating general-purpose memory.
  template <typename Callback>
  explicit OperationCallback(Callback&& callback) noexcept {
    using CallbackType = std::remove_cvref_t<Callback>;
    static_assert(sizeof(CallbackType) <= kOperationCallbackInlineBytes,
                  "Operation callback exceeds kOperationCallbackInlineBytes; capture a pointer or "
                  "a small state handle instead.");
    static_assert(alignof(CallbackType) <= alignof(std::max_align_t),
                  "Operation callback has unsupported over-alignment; capture a pointer to the "
                  "aligned state instead.");
    static_assert(std::is_nothrow_constructible_v<CallbackType, Callback&&>,
                  "Operation callback construction must be noexcept.");
    static_assert(std::is_nothrow_move_constructible_v<CallbackType>,
                  "Operation callback must be nothrow move constructible.");
    static_assert(std::is_nothrow_destructible_v<CallbackType>,
                  "Operation callback must be nothrow destructible.");
    static_assert(std::is_nothrow_invocable_r_v<void, CallbackType&, const OperationResult&>,
                  "Operation callback must be callable as void(const OperationResult&) noexcept.");

    if constexpr (std::is_pointer_v<CallbackType>) {
      if (callback == nullptr) {
        return;
      }
    }

    std::construct_at(static_cast<CallbackType*>(Storage()), std::forward<Callback>(callback));
    operations_ = &OperationsFor<CallbackType>();
  }

  OperationCallback(OperationCallback&& other) noexcept { MoveFrom(other); }
  OperationCallback& operator=(OperationCallback&& other) noexcept {
    if (this != &other) {
      Reset();
      MoveFrom(other);
    }
    return *this;
  }
  OperationCallback(const OperationCallback&) = delete;
  OperationCallback& operator=(const OperationCallback&) = delete;
  ~OperationCallback() { Reset(); }

  [[nodiscard]] explicit operator bool() const noexcept { return operations_ != nullptr; }

 private:
  struct CallbackVTable final {
    void (*invoke)(void*, const OperationResult&) noexcept;
    void (*move)(void*, void*) noexcept;
    void (*destroy)(void*) noexcept;
  };

  template <typename Callback>
  [[nodiscard]] static const CallbackVTable& OperationsFor() noexcept {
    static constexpr CallbackVTable operations{
        [](void* storage, const OperationResult& result) noexcept {
          std::invoke(*static_cast<Callback*>(storage), result);
        },
        [](void* source, void* destination) noexcept {
          auto* callback = static_cast<Callback*>(source);
          std::construct_at(static_cast<Callback*>(destination), std::move(*callback));
          std::destroy_at(callback);
        },
        [](void* storage) noexcept { std::destroy_at(static_cast<Callback*>(storage)); }};
    return operations;
  }

  [[nodiscard]] void* Storage() noexcept { return storage_; }
  void Invoke(const OperationResult& result) noexcept { operations_->invoke(Storage(), result); }
  void MoveFrom(OperationCallback& other) noexcept {
    if (other.operations_ == nullptr) {
      return;
    }
    other.operations_->move(other.Storage(), Storage());
    operations_ = std::exchange(other.operations_, nullptr);
  }
  void Reset() noexcept {
    if (operations_ != nullptr) {
      operations_->destroy(Storage());
      operations_ = nullptr;
    }
  }

  friend struct detail::OperationCallbackAccess;

  alignas(std::max_align_t) std::byte storage_[kOperationCallbackInlineBytes]{};
  const CallbackVTable* operations_{nullptr};
};

enum class OperationPollStatus : std::uint8_t { kInvalidOperation, kPending, kCompleted };

struct OperationPollResult final {
  OperationPollStatus status{OperationPollStatus::kInvalidOperation};
  std::optional<OperationResult> completion{};
};

enum class OperationWaitStatus : std::uint8_t {
  kInvalidOperation,
  kDeadlineExceeded,
  kForbiddenThread,
  kWaitFailed,
  kCompleted,
};

struct OperationWaitResult final {
  OperationWaitStatus status{OperationWaitStatus::kInvalidOperation};
  std::optional<OperationResult> completion{};

  [[nodiscard]] ULOG_API std::string_view Message() const noexcept;
  [[nodiscard]] ULOG_API std::string_view HowToFix() const noexcept;
};

enum class OperationCallbackStatus : std::uint8_t {
  kRegistered,
  kAlreadyRegistered,
  kInvalidOperation,
  kInvalidCallback,
};

struct OperationCallbackResult final {
  OperationCallbackStatus status{OperationCallbackStatus::kInvalidOperation};

  [[nodiscard]] ULOG_API std::string_view Message() const noexcept;
  [[nodiscard]] ULOG_API std::string_view HowToFix() const noexcept;
};

enum class OperationStartErrorCode : std::uint8_t { kControlReserveExhausted };

struct OperationStartFailure final {
  OperationStartErrorCode code{OperationStartErrorCode::kControlReserveExhausted};
  std::size_t control_capacity{0};
  std::size_t controls_in_use{0};

  [[nodiscard]] ULOG_API std::string_view Message() const noexcept;
  [[nodiscard]] ULOG_API std::string_view HowToFix() const noexcept;
};

class Operation final {
 public:
  Operation() noexcept = default;
  ULOG_API Operation(Operation&& other) noexcept;
  ULOG_API Operation& operator=(Operation&& other) noexcept;
  Operation(const Operation&) = delete;
  Operation& operator=(const Operation&) = delete;
  ULOG_API ~Operation();

  [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }
  /// Observes pending or terminal state without waiting or consuming the result.
  [[nodiscard]] ULOG_API OperationPollResult Poll() const noexcept;
  /// Waits only on an external caller thread; timeout does not cancel the Operation.
  [[nodiscard]] ULOG_API OperationWaitResult
  WaitUntil(std::chrono::steady_clock::time_point deadline) const noexcept;
  /// Registers the sole callback. Even late registration is dispatched asynchronously.
  [[nodiscard]] ULOG_API OperationCallbackResult
  OnComplete(OperationCallback callback) const noexcept;

  template <typename Callback>
  [[nodiscard]] OperationCallbackResult OnComplete(Callback&& callback) const noexcept {
    return OnComplete(OperationCallback{std::forward<Callback>(callback)});
  }

 private:
  friend struct detail::OperationAccess;
  Operation(void* state, std::uint64_t generation,
            const detail::OperationVTable& operations) noexcept
      : state_(state), generation_(generation), operations_(&operations) {}

  void Reset() noexcept;

  void* state_{nullptr};
  std::uint64_t generation_{0};
  const detail::OperationVTable* operations_{nullptr};
};

struct OperationStartResult final {
  Operation operation{};
  std::optional<OperationStartFailure> failure{};

  [[nodiscard]] explicit operator bool() const noexcept {
    return !failure.has_value() && static_cast<bool>(operation);
  }
};

}  // namespace ulog
