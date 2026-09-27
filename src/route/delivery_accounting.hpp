#pragma once

#include <cstddef>
#include <cstdint>

namespace ulog::detail::route {

/// Wakes the Runtime worker after an asynchronous delivery event; invoked under the route lock.
struct RouteWake final {
  void* context{nullptr};
  void (*notify)(void*) noexcept {nullptr};

  void Notify() const noexcept {
    if (notify != nullptr) {
      notify(context);
    }
  }
};

enum class DeliveryOutcome : std::uint8_t { kDelivered, kFailed };

/// Terminal outcome of one delivery that the worker retires in admission order.
struct DeliveryRetirement final {
  DeliveryOutcome outcome{DeliveryOutcome::kFailed};
  std::size_t encoded_bytes{0};
};

/// Submitted deliveries that the worker has not retired yet, limited to one watermark.
struct InFlightSummary final {
  std::uint64_t records{0};
  std::uint64_t bytes{0};
  std::uint64_t delivered_records{0};
  std::uint64_t delivered_bytes{0};
  std::uint64_t failed_records{0};
  std::uint64_t failed_bytes{0};
};

struct StoreResult final {
  std::size_t encoded_bytes{0};
  bool submitted{false};
};

}  // namespace ulog::detail::route
