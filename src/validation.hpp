#pragma once

#include "DokuTexture/Types.hpp"
#include <limits>
#include <new>
#include <stdexcept>

namespace doku::texture::detail {
inline std::uint32_t blocks(std::uint32_t d) { return d / 4 + (d % 4 != 0); }
inline std::uint32_t mip_count(std::uint32_t w, std::uint32_t h) {
  std::uint32_t n = 0;
  for (auto d = w > h ? w : h; d; d >>= 1)
    ++n;
  return n;
}
inline bool add(std::uint64_t a, std::uint64_t b, std::uint64_t &out) {
  if (b > UINT64_MAX - a)
    return false;
  out = a + b;
  return true;
}
inline bool multiply(std::uint64_t a, std::uint64_t b, std::uint64_t &out) {
  if (a && b > UINT64_MAX / a)
    return false;
  out = a * b;
  return true;
}
inline bool exceeds(std::uint64_t size, std::uint64_t limit) {
  return limit && size > limit;
}
inline bool cancelled(const ExecutionOptions &e) {
  return e.cancelFlag && e.cancelFlag->load(std::memory_order_relaxed);
}
inline ColorSpace color_space(GpuFormat f) {
  return f == GpuFormat::BC7_Srgb ? ColorSpace::SRGB : ColorSpace::Linear;
}
} // namespace doku::texture::detail
