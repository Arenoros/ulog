#include "control/admission_wait.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>

#include "control/thread_role.hpp"
#include "producer/producer_kernel.hpp"

namespace ulog::detail::control {
namespace {

/// Bounds each condition-variable wait, so a far deadline such as time_point::max() never reaches
/// a platform conversion that could overflow into an immediate timeout.
constexpr std::chrono::hours kLongestSingleWait{1};

}  // namespace

producer::CapacityWaiter AdmissionWait::Hooks() noexcept {
  return {
      .context = this,
      .may_block = &AdmissionWait::MayBlock,
      .enter = &AdmissionWait::Enter,
      .epoch = &AdmissionWait::Epoch,
      .wait = &AdmissionWait::Wait,
      .leave = &AdmissionWait::Leave,
      .notify = &AdmissionWait::Notify,
      .close = &AdmissionWait::Close,
  };
}

void AdmissionWait::CloseAndAwaitIdle() noexcept {
  std::unique_lock lock{mutex_};
  closed_ = true;
  condition_.notify_all();
  condition_.wait(lock, [&] { return active_ == 0U; });
}

bool AdmissionWait::MayBlock(void*) noexcept {
  // The worker, the I/O loop, and Operation callbacks must keep making progress for every waiter.
  return GetUlogThreadRole() == UlogThreadRole::kExternal;
}

bool AdmissionWait::Enter(void* context) noexcept {
  auto& self = *static_cast<AdmissionWait*>(context);
  const std::lock_guard lock{self.mutex_};
  if (self.closed_) {
    return false;
  }
  ++self.active_;
  return true;
}

std::uint64_t AdmissionWait::Epoch(void* context) noexcept {
  auto& self = *static_cast<AdmissionWait*>(context);
  const std::lock_guard lock{self.mutex_};
  return self.epoch_;
}

bool AdmissionWait::Wait(void* context, std::uint64_t observed,
                         std::chrono::steady_clock::time_point deadline) noexcept {
  auto& self = *static_cast<AdmissionWait*>(context);
  std::unique_lock lock{self.mutex_};
  while (!self.closed_ && self.epoch_ == observed) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    self.condition_.wait_until(
        lock, deadline - now > kLongestSingleWait ? now + kLongestSingleWait : deadline);
  }
  return !self.closed_;
}

void AdmissionWait::Leave(void* context) noexcept {
  auto& self = *static_cast<AdmissionWait*>(context);
  const std::lock_guard lock{self.mutex_};
  --self.active_;
  if (self.active_ == 0U && self.closed_) {
    self.condition_.notify_all();
  }
}

void AdmissionWait::Notify(void* context) noexcept {
  auto& self = *static_cast<AdmissionWait*>(context);
  {
    const std::lock_guard lock{self.mutex_};
    ++self.epoch_;
  }
  self.condition_.notify_all();
}

void AdmissionWait::Close(void* context) noexcept {
  auto& self = *static_cast<AdmissionWait*>(context);
  {
    const std::lock_guard lock{self.mutex_};
    self.closed_ = true;
  }
  self.condition_.notify_all();
}

}  // namespace ulog::detail::control
