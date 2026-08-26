#pragma once

#include <cstddef>
#include <span>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::encoding {

struct RawEncodeResult final {
  std::size_t encoded_bytes{0};
  bool complete{false};
};

[[nodiscard]] std::size_t MaximumRawEncodedBytes(std::size_t maximum_record_bytes) noexcept;
[[nodiscard]] RawEncodeResult EncodeRawRecord(const producer::RecordView& record,
                                              std::span<char> output) noexcept;

}  // namespace ulog::detail::encoding
