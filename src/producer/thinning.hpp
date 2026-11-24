#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ulog::detail::producer {

/// Keep ratios are fixed point: kThinningScale keeps every call, zero keeps none.
inline constexpr std::uint32_t kThinningScale = 1U << 16U;

/// SplitMix64 finalizer: a stateless bijective mix of 64 bits.
[[nodiscard]] constexpr std::uint64_t MixThinningBits(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ULL;
  value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
  value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
  return value ^ (value >> 31U);
}

/// Call-site identity: FNV-1a of the source path, mixed with the line. It reads only the
/// SourceLocation, so it needs no per-site table.
[[nodiscard]] constexpr std::uint64_t CallSiteHash(std::string_view path,
                                                   std::uint32_t line) noexcept {
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (const char byte : path) {
    hash = (hash ^ static_cast<std::uint8_t>(byte)) * 0x100000001B3ULL;
  }
  return MixThinningBits(hash ^ line);
}

/// Occupancy-dependent keep ratio: everything at or below `low_watermark` bytes, then falling
/// linearly to none when `occupied` reaches `capacity`.
[[nodiscard]] constexpr std::uint32_t ThinningKeepRatio(std::size_t occupied,
                                                        std::size_t low_watermark,
                                                        std::size_t capacity) noexcept {
  if (occupied <= low_watermark) {
    return kThinningScale;
  }
  if (occupied >= capacity || capacity <= low_watermark) {
    return 0;
  }
  return static_cast<std::uint32_t>(static_cast<std::uint64_t>(capacity - occupied) *
                                    kThinningScale / (capacity - low_watermark));
}

/// Whether the call with this site and reservation-attempt nonce is kept at `keep_ratio`. The same
/// inputs always give the same answer; across nonces each site is kept at about the ratio.
[[nodiscard]] constexpr bool ThinningKeeps(std::uint64_t site_hash, std::uint64_t nonce,
                                           std::uint32_t keep_ratio) noexcept {
  const std::uint64_t draw = MixThinningBits(site_hash ^ MixThinningBits(nonce)) >> 48U;
  return draw < keep_ratio;
}

}  // namespace ulog::detail::producer
