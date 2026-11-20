#include "DokuTexture/Encode.hpp"
#include "bc7decomp.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>

using namespace doku::texture;

void test_single_mip_layout_npot() {
  std::cout << "[Test] Single Mip NPOT Layout (5x7, 1x1, 7x1)" << std::endl;

  // 5x7 texture
  std::uint32_t w = 5, h = 7;
  std::vector<std::byte> rawRgba(w * h * 4);
  for (size_t i = 0; i < rawRgba.size(); i += 4) {
    rawRgba[i + 0] = static_cast<std::byte>(255); // R
    rawRgba[i + 1] = static_cast<std::byte>(128); // G
    rawRgba[i + 2] = static_cast<std::byte>(64);  // B
    rawRgba[i + 3] = static_cast<std::byte>(255); // A
  }

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = w * 4;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  EncodeOptions opts;
  opts.target = GpuFormat::BC7_UNorm;
  opts.quality = Quality::Fast;

  auto res = encode_bc7_mip(mip, opts);
  assert(res.has_value());

  const auto &tex = res.value();
  assert(tex.mips.size() == 1);

  const auto &layout = tex.mips[0];
  assert(layout.width == 5);
  assert(layout.height == 7);
  assert(layout.blocksX == 2);        // ceil(5/4) = 2
  assert(layout.blocksY == 2);        // ceil(7/4) = 2
  assert(layout.rowPitchBytes == 32); // 2 * 16
  assert(layout.sizeBytes == 64);     // 32 * 2
  assert(tex.payload.size() == 64);

  // 1x1 texture
  mip.width = 1;
  mip.height = 1;
  mip.rowPitchBytes = 4;
  mip.bytes = ByteSpan{rawRgba.data(), 4};

  res = encode_bc7_mip(mip, opts);
  assert(res.has_value());
  assert(res.value().mips[0].blocksX == 1);
  assert(res.value().mips[0].blocksY == 1);
  assert(res.value().payload.size() == 16);

  // 7x1 texture
  mip.width = 7;
  mip.height = 1;
  mip.rowPitchBytes = 28;
  mip.bytes = ByteSpan{rawRgba.data(), 28};

  res = encode_bc7_mip(mip, opts);
  assert(res.has_value());
  assert(res.value().mips[0].blocksX == 2);
  assert(res.value().mips[0].blocksY == 1);
  assert(res.value().payload.size() == 32);

  std::cout << "  -> PASSED" << std::endl;
}

void test_padded_row_pitch() {
  std::cout << "[Test] Padded Row Pitch Input" << std::endl;

  std::uint32_t w = 4, h = 4;
  std::uint64_t paddedPitch = 32; // 32 bytes pitch instead of 16
  std::vector<std::byte> rawRgba(paddedPitch * h, std::byte{0});

  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::size_t offset = y * paddedPitch + x * 4;
      rawRgba[offset + 0] = static_cast<std::byte>(200);
      rawRgba[offset + 1] = static_cast<std::byte>(100);
      rawRgba[offset + 2] = static_cast<std::byte>(50);
      rawRgba[offset + 3] = static_cast<std::byte>(255);
    }
  }

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = paddedPitch;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  EncodeOptions opts;
  auto res = encode_bc7_mip(mip, opts);
  assert(res.has_value());
  assert(res.value().payload.size() == 16);

  bc7decomp::color_rgba decoded[16];
  bool unpackOk = bc7decomp::unpack_bc7(res.value().payload.data(), decoded);
  assert(unpackOk);

  // Verify decoded RGB color is close to original (200, 100, 50)
  for (int i = 0; i < 16; ++i) {
    assert(std::abs(static_cast<int>(decoded[i][0]) - 200) <= 8);
    assert(std::abs(static_cast<int>(decoded[i][1]) - 100) <= 8);
    assert(std::abs(static_cast<int>(decoded[i][2]) - 50) <= 8);
  }

  std::cout << "  -> PASSED" << std::endl;
}

void test_translucent_alpha_encoding() {
  std::cout << "[Test] Translucent Alpha Encoding (BC7 Mode 6/7)" << std::endl;

  std::uint32_t w = 4, h = 4;
  std::vector<std::byte> rawRgba(w * h * 4);
  for (size_t i = 0; i < rawRgba.size(); i += 4) {
    rawRgba[i + 0] = static_cast<std::byte>(200); // R
    rawRgba[i + 1] = static_cast<std::byte>(100); // G
    rawRgba[i + 2] = static_cast<std::byte>(50);  // B
    rawRgba[i + 3] = static_cast<std::byte>(128); // Translucent Alpha = 128
  }

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = w * 4;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  EncodeOptions opts;
  opts.target = GpuFormat::BC7_UNorm;

  auto res = encode_bc7_mip(mip, opts);
  assert(res.has_value());
  assert(res.value().payload.size() == 16);

  bc7decomp::color_rgba decoded[16];
  bool unpackOk = bc7decomp::unpack_bc7(res.value().payload.data(), decoded);
  assert(unpackOk);

  for (int i = 0; i < 16; ++i) {
    assert(std::abs(static_cast<int>(decoded[i][0]) - 200) <= 8);
    assert(std::abs(static_cast<int>(decoded[i][1]) - 100) <= 8);
    assert(std::abs(static_cast<int>(decoded[i][2]) - 50) <= 8);
    assert(std::abs(static_cast<int>(decoded[i][3]) - 128) <= 8);
  }

  std::cout << "  -> PASSED" << std::endl;
}

void test_mip_chain_encoding() {
  std::cout << "[Test] Padded NPOT mip chain matches individual encodes"
            << std::endl;
  EncodeOptions opts;
  opts.quality = Quality::Fast;
  opts.target = GpuFormat::BC7_Srgb;

  std::vector<OwningBuffer> rawMips;
  std::vector<Rgba8MipView> mips;
  OwningBuffer expected;
  for (std::uint32_t w = 257, h = 129;;) {
    const auto pitch = w * 4 + 12;
    auto &pixels = rawMips.emplace_back((h - 1) * pitch + w * 4);
    for (std::size_t i = 0; i < pixels.size(); ++i)
      pixels[i] = std::byte((i * 71 + i / 17 + mips.size()) % 256);
    mips.push_back({w, h, pitch, {pixels.data(), pixels.size()}});
    auto mip = encode_bc7_mip(mips.back(), opts);
    assert(mip);
    const auto &payload = mip.value().payload;
    expected.insert(expected.end(), payload.begin(), payload.end());
    if (w == 1 && h == 1)
      break;
    w = std::max(1U, w / 2);
    h = std::max(1U, h / 2);
  }

  opts.execution.maxWorkers = 4;
  opts.execution.limits.maxWorkingBytes = expected.size();
  const Span<Rgba8MipView> chainView{mips.data(), mips.size()};
  auto res = encode_bc7_chain(chainView, opts);
  assert(res.has_value());

  const auto &tex = res.value();
  assert(tex.mips.size() == mips.size());
  assert(tex.payload == expected);
  assert(tex.metadata.colorSpace == ColorSpace::SRGB);

  std::uint64_t expectedOffset = 0;
  for (std::size_t level = 0; level < mips.size(); ++level) {
    const auto &layout = tex.mips[level];
    const auto blocksX = (mips[level].width + 3) / 4;
    const auto blocksY = (mips[level].height + 3) / 4;
    const auto size = std::uint64_t(blocksX) * blocksY * 16;
    assert(layout.level == level);
    assert(layout.width == mips[level].width &&
           layout.height == mips[level].height);
    assert(layout.blocksX == blocksX && layout.blocksY == blocksY);
    assert(layout.rowPitchBytes == blocksX * 16);
    assert(layout.offsetBytes == expectedOffset && layout.sizeBytes == size);
    expectedOffset += size;
  }
  assert(tex.payload.size() == expectedOffset);
  --opts.execution.limits.maxWorkingBytes;
  auto limited = encode_bc7_chain(chainView, opts);
  assert(!limited && limited.error().code == ErrorCode::ResourceLimitExceeded);

  std::cout << "  -> PASSED" << std::endl;
}

void test_multithreading_and_cancel() {
  std::cout << "[Test] Multithreading & Cancel Flag" << std::endl;

  std::uint32_t w = 16381, h = 3;
  std::vector<std::byte> rawRgba(w * h * 4);
  for (std::size_t i = 0; i < rawRgba.size(); ++i)
    rawRgba[i] = std::byte((i * 71 + i / 17) % 256);

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = w * 4;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  // 1. Multithreading maxWorkers = 4
  EncodeOptions opts;
  opts.execution.maxWorkers = 4;
  opts.quality = Quality::Fast;

  auto res4 = encode_bc7_mip(mip, opts);
  assert(res4.has_value());

  // 2. Single thread maxWorkers = 1
  opts.execution.maxWorkers = 1;
  auto res1 = encode_bc7_mip(mip, opts);
  assert(res1.has_value());

  assert(res4.value().payload == res1.value().payload);

  // 3. Cancel flag test
  std::atomic_bool cancelFlag{true};
  opts.execution.cancelFlag = &cancelFlag;

  auto resCancel = encode_bc7_mip(mip, opts);
  assert(!resCancel.has_value());
  assert(resCancel.error().code == ErrorCode::Cancelled);

  std::cout << "  -> PASSED" << std::endl;
}

void test_error_handling_and_limits() {
  std::cout << "[Test] Error Handling & Resource Limits" << std::endl;

  std::vector<std::byte> dummy(100);
  Rgba8MipView mip;
  mip.width = 16;
  mip.height = 16;
  mip.rowPitchBytes = 64;
  mip.bytes = ByteSpan{dummy.data(), dummy.size()};

  EncodeOptions opts;

  // 1. Invalid width / height = 0
  mip.width = 0;
  auto res = encode_bc7_mip(mip, opts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::InvalidArgument);

  // 2. Row pitch too small
  mip.width = 16;
  mip.rowPitchBytes = 10; // min is 16*4 = 64
  res = encode_bc7_mip(mip, opts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::InvalidArgument);

  // 3. Buffer truncated
  mip.rowPitchBytes = 64;
  mip.bytes = ByteSpan{dummy.data(), 10}; // required is (15*64 + 64) = 1024
  res = encode_bc7_mip(mip, opts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::TruncatedInput);

  // 4. Resource limits: maxWidth
  mip.bytes = ByteSpan{dummy.data(), 1024};
  opts.execution.limits.maxWidth = 8;
  res = encode_bc7_mip(mip, opts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::ResourceLimitExceeded);

  // 5. Unsupported format
  opts.execution.limits.maxWidth = 16384;
  opts.target = GpuFormat::ETC1_RGB;
  res = encode_bc7_mip(mip, opts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::UnsupportedFormat);

  std::cout << "  -> PASSED" << std::endl;
}

int main() {
  std::cout << "========================================" << std::endl;
  std::cout << " Running test_bc7_encode" << std::endl;
  std::cout << "========================================" << std::endl;

  test_single_mip_layout_npot();
  test_padded_row_pitch();
  test_translucent_alpha_encoding();
  test_mip_chain_encoding();
  test_multithreading_and_cancel();
  test_error_handling_and_limits();

  std::cout << "\nALL Stage 1 BC7 Encode Tests Passed Successfully!"
            << std::endl;
  return 0;
}
