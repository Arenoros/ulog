#pragma once

#include <chrono>
#include <cstdint>
#include <ulog/operation.hpp>

namespace ulog::detail {

struct OperationVTable final {
  OperationPollResult (*poll)(const void*, std::uint64_t) noexcept;
  OperationWaitResult (*wait_until)(void*, std::uint64_t,
                                    std::chrono::steady_clock::time_point) noexcept;
  OperationCallbackResult (*on_complete)(void*, std::uint64_t, OperationCallback&&) noexcept;
  void (*release)(void*, std::uint64_t) noexcept;
};

struct OperationCallbackAccess final {
  static void Invoke(OperationCallback& callback, const OperationResult& result) noexcept {
    callback.Invoke(result);
  }
  static void Reset(OperationCallback& callback) noexcept { callback.Reset(); }
};

struct OperationResultAccess final {
  [[nodiscard]] static constexpr OperationResult Make(const OperationOutcome outcome) noexcept {
    return OperationResult{outcome};
  }
};

struct OperationAccess final {
  [[nodiscard]] static Operation Make(void* state, std::uint64_t generation,
                                      const OperationVTable& operations) noexcept {
    return Operation{state, generation, operations};
  }
};

}  // namespace ulog::detail
