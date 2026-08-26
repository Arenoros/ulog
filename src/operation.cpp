#include <string_view>
#include <ulog/operation.hpp>
#include <utility>

#include "control/operation_access.hpp"

namespace ulog {
namespace {

constexpr std::string_view kEmptyOperationMessage =
    "The Operation handle is empty and cannot be waited on.";
constexpr std::string_view kEmptyOperationHint =
    "Keep the Operation returned when the ordered action starts and do not reuse a moved-from "
    "handle.";
constexpr std::string_view kDeadlineMessage =
    "The Operation did not complete before the requested deadline.";
constexpr std::string_view kDeadlineHint =
    "The Operation is still active; poll it or wait again with a later deadline.";
constexpr std::string_view kForbiddenThreadMessage =
    "WaitUntil cannot block a Ulog-owned worker, I/O, or callback thread.";
constexpr std::string_view kForbiddenThreadHint =
    "Use Poll or register the completion callback from Ulog-owned threads.";
constexpr std::string_view kWaitFailedMessage =
    "The operating system wait primitive failed before the Operation completed.";
constexpr std::string_view kWaitFailedHint =
    "Poll the Operation and retry WaitUntil; report repeated failures with the platform and "
    "deadline.";
constexpr std::string_view kCompletedMessage = "The Operation completed.";
constexpr std::string_view kNoActionNeeded = "No corrective action is required.";
constexpr std::string_view kReserveExhaustedMessage =
    "The Operation could not start because every bounded control-reserve slot is in use.";
constexpr std::string_view kReserveExhaustedHint =
    "Release completed Operation handles, wait for in-flight control actions, or increase the "
    "Runtime control-operation capacity.";
constexpr std::string_view kCallbackRegisteredMessage =
    "The Operation completion callback was registered.";
constexpr std::string_view kCallbackAlreadyRegisteredMessage =
    "This Operation already has a completion callback.";
constexpr std::string_view kCallbackAlreadyRegisteredHint =
    "Register at most one callback and fan out from that callback if multiple consumers need "
    "notification.";
constexpr std::string_view kInvalidCallbackMessage = "The completion callback is empty.";
constexpr std::string_view kInvalidCallbackHint =
    "Pass a non-empty OperationCallback or a small noexcept callable.";

struct OperationDiagnostic final {
  std::string_view message;
  std::string_view how_to_fix;
};

[[nodiscard]] constexpr OperationDiagnostic GetWaitDiagnostic(
    const OperationWaitStatus status) noexcept {
  switch (status) {
    case OperationWaitStatus::kInvalidOperation:
      return {kEmptyOperationMessage, kEmptyOperationHint};
    case OperationWaitStatus::kDeadlineExceeded:
      return {kDeadlineMessage, kDeadlineHint};
    case OperationWaitStatus::kForbiddenThread:
      return {kForbiddenThreadMessage, kForbiddenThreadHint};
    case OperationWaitStatus::kWaitFailed:
      return {kWaitFailedMessage, kWaitFailedHint};
    case OperationWaitStatus::kCompleted:
      return {kCompletedMessage, kNoActionNeeded};
  }
  return {kWaitFailedMessage, kWaitFailedHint};
}

[[nodiscard]] constexpr OperationDiagnostic GetCallbackDiagnostic(
    const OperationCallbackStatus status) noexcept {
  switch (status) {
    case OperationCallbackStatus::kRegistered:
      return {kCallbackRegisteredMessage, kNoActionNeeded};
    case OperationCallbackStatus::kAlreadyRegistered:
      return {kCallbackAlreadyRegisteredMessage, kCallbackAlreadyRegisteredHint};
    case OperationCallbackStatus::kInvalidOperation:
      return {kEmptyOperationMessage, kEmptyOperationHint};
    case OperationCallbackStatus::kInvalidCallback:
      return {kInvalidCallbackMessage, kInvalidCallbackHint};
  }
  return {kInvalidCallbackMessage, kInvalidCallbackHint};
}

}  // namespace

std::string_view OperationWaitResult::Message() const noexcept {
  return GetWaitDiagnostic(status).message;
}

std::string_view OperationWaitResult::HowToFix() const noexcept {
  return GetWaitDiagnostic(status).how_to_fix;
}

std::string_view OperationStartFailure::Message() const noexcept {
  return kReserveExhaustedMessage;
}

std::string_view OperationStartFailure::HowToFix() const noexcept { return kReserveExhaustedHint; }

std::string_view OperationCallbackResult::Message() const noexcept {
  return GetCallbackDiagnostic(status).message;
}

std::string_view OperationCallbackResult::HowToFix() const noexcept {
  return GetCallbackDiagnostic(status).how_to_fix;
}

Operation::Operation(Operation&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)),
      generation_(std::exchange(other.generation_, 0)),
      operations_(std::exchange(other.operations_, nullptr)) {}

Operation& Operation::operator=(Operation&& other) noexcept {
  if (this != &other) {
    Reset();
    state_ = std::exchange(other.state_, nullptr);
    generation_ = std::exchange(other.generation_, 0);
    operations_ = std::exchange(other.operations_, nullptr);
  }
  return *this;
}

Operation::~Operation() { Reset(); }

OperationPollResult Operation::Poll() const noexcept {
  if (state_ == nullptr) {
    return {};
  }
  return operations_->poll(state_, generation_);
}

OperationWaitResult Operation::WaitUntil(
    const std::chrono::steady_clock::time_point deadline) const noexcept {
  if (state_ == nullptr) {
    return {};
  }
  return operations_->wait_until(state_, generation_, deadline);
}

OperationCallbackResult Operation::OnComplete(OperationCallback callback) const noexcept {
  if (state_ == nullptr) {
    return {};
  }
  return operations_->on_complete(state_, generation_, std::move(callback));
}

void Operation::Reset() noexcept {
  if (state_ != nullptr) {
    operations_->release(state_, generation_);
  }
  state_ = nullptr;
  generation_ = 0;
  operations_ = nullptr;
}

}  // namespace ulog
