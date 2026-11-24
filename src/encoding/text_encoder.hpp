#pragma once

#include <cstddef>
#include <span>
#include <ulog/format.hpp>

#include "producer/producer_kernel.hpp"

namespace ulog::detail::encoding {

struct EncodeResult final {
  std::size_t encoded_bytes{0};
  bool complete{false};
};

/// True for every Format value; false for a value outside the enumeration.
[[nodiscard]] bool IsEncodable(Format format) noexcept;

/// Worst-case complete frame for a Record of at most `maximum_record_bytes` serialized bytes;
/// zero for a format that is not encodable or a bound that overflows.
[[nodiscard]] std::size_t MaximumEncodedBytes(Format format,
                                              std::size_t maximum_record_bytes) noexcept;

/// Writes one complete frame or nothing: the frame is sized first and written only if it fits.
[[nodiscard]] EncodeResult EncodeRecord(Format format, const producer::RecordView& record,
                                        std::span<char> output) noexcept;

}  // namespace ulog::detail::encoding
