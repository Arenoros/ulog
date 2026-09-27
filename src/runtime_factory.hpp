#pragma once

#include <ulog/runtime.hpp>

#include "io/raw_file_sink.hpp"

namespace ulog::detail {

/// Private Runtime construction with deterministic file faults for Ulog's own tests.
struct RuntimeFactoryAccess final {
  [[nodiscard]] static RuntimeCreateResult CreateRawFileRuntime(
      RuntimeConfig config, const RawFileRouteConfig& route,
      const io::FileFaultPlan& faults) noexcept;
};

}  // namespace ulog::detail
