#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::control {

/// Parks blocking calls (Logger::LogUntil) until the producer kernel frees capacity, their
/// deadline passes, or admission closes. The kernel reaches it only through CapacityWaiter hooks,
/// so its lock and condition variable stay off the ordinary producer path.
class AdmissionWait final {
 public:
  AdmissionWait() = default;
  AdmissionWait(const AdmissionWait&) = delete;
  AdmissionWait& operator=(const AdmissionWait&) = delete;

  [[nodiscard]] producer::CapacityWaiter Hooks() noexcept;

  /// Releases every waiting call and returns once each has left. Released calls only record their
  /// rejection before leaving, so the wait is bounded.
  void CloseAndAwaitIdle() noexcept;

 private:
  static bool MayBlock(void* context) noexcept;
  static bool Enter(void* context) noexcept;
  static std::uint64_t Epoch(void* context) noexcept;
  static bool Wait(void* context, std::uint64_t observed,
                   std::chrono::steady_clock::time_point deadline) noexcept;
  static void Leave(void* context) noexcept;
  static void Notify(void* context) noexcept;
  static void Close(void* context) noexcept;

  std::mutex mutex_;
  std::condition_variable condition_;
  std::uint64_t epoch_{0};
  std::size_t active_{0};
  bool closed_{false};
};

}  // namespace ulog::detail::control
