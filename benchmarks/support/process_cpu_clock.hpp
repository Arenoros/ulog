#pragma once

#include <chrono>

namespace ulog::benchmark_support {

[[nodiscard]] std::chrono::nanoseconds ReadProcessCpuTime();

}  // namespace ulog::benchmark_support
