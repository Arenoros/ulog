#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace ulog::test_support {

/// Reusable barrier that wakes every waiter as soon as the last participant arrives.
///
/// libc++ std::barrier waits by polling with sleeps of up to half the time already waited, so on
/// a loaded runner one descheduled participant stretches every later phase. Tests with hundreds
/// of phases use this barrier to keep phase latency bounded by the scheduler alone.
class PhaseBarrier final {
 public:
  explicit PhaseBarrier(std::size_t participants) noexcept : participants_{participants} {}

  PhaseBarrier(const PhaseBarrier&) = delete;
  PhaseBarrier& operator=(const PhaseBarrier&) = delete;
  PhaseBarrier(PhaseBarrier&&) = delete;
  PhaseBarrier& operator=(PhaseBarrier&&) = delete;
  ~PhaseBarrier() = default;

  void ArriveAndWait() {
    std::unique_lock lock{mutex_};
    const std::uint64_t phase = phase_;
    if (++arrived_ == participants_) {
      arrived_ = 0;
      ++phase_;
      lock.unlock();
      condition_.notify_all();
      return;
    }
    condition_.wait(lock, [this, phase] { return phase_ != phase; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::size_t participants_;
  std::size_t arrived_{0};
  std::uint64_t phase_{0};
};

}  // namespace ulog::test_support
