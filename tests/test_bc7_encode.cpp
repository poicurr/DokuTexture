#include "DokuTexture/Encode.hpp"
#include "bc7decomp.h"

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
    assert(std::abs(static_cast<int>(decoded[i].r) - 200) <= 8);
    assert(std::abs(static_cast<int>(decoded[i].g) - 100) <= 8);
    assert(std::abs(static_cast<int>(decoded[i].b) - 50) <= 8);
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
    assert(std::abs(static_cast<int>(decoded[i].r) - 200) <= 8);
    assert(std::abs(static_cast<int>(decoded[i].g) - 100) <= 8);
    assert(std::abs(static_cast<int>(decoded[i].b) - 50) <= 8);
    assert(std::abs(static_cast<int>(decoded[i].a) - 128) <= 8);
  }

  std::cout << "  -> PASSED" << std::endl;
}

void test_mip_chain_encoding() {
  std::cout << "[Test] Mip Chain Encoding (8x8 -> 4x4 -> 2x2 -> 1x1)"
            << std::endl;

  std::vector<std::vector<std::byte>> rawMips(4);
  std::vector<Rgba8MipView> mips(4);

  std::uint32_t dims[4] = {8, 4, 2, 1};
  for (int l = 0; l < 4; ++l) {
    std::uint32_t d = dims[l];
    rawMips[l].resize(d * d * 4, static_cast<std::byte>(128));

    mips[l].width = d;
    mips[l].height = d;
    mips[l].rowPitchBytes = d * 4;
    mips[l].bytes = ByteSpan{rawMips[l].data(), rawMips[l].size()};
  }

  Span<Rgba8MipView> chainView{mips.data(), mips.size()};
  EncodeOptions opts;
  opts.quality = Quality::Fast;

  auto res = encode_bc7_chain(chainView, opts);
  assert(res.has_value());

  const auto &tex = res.value();
  assert(tex.mips.size() == 4);

  // Check offsets and sizes
  std::uint64_t expectedOffset = 0;
  std::uint32_t expectedBlocks[4] = {2, 1, 1, 1}; // blocks per side
  for (int l = 0; l < 4; ++l) {
    assert(tex.mips[l].level == static_cast<std::uint32_t>(l));
    assert(tex.mips[l].blocksX == expectedBlocks[l]);
    assert(tex.mips[l].blocksY == expectedBlocks[l]);
    assert(tex.mips[l].offsetBytes == expectedOffset);

    std::uint64_t size =
        static_cast<std::uint64_t>(expectedBlocks[l]) * expectedBlocks[l] * 16;
    assert(tex.mips[l].sizeBytes == size);
    expectedOffset += size;
  }

  assert(tex.payload.size() == expectedOffset);

  std::cout << "  -> PASSED" << std::endl;
}

void test_multithreading_and_cancel() {
  std::cout << "[Test] Multithreading & Cancel Flag" << std::endl;

  std::uint32_t w = 256, h = 256;
  std::vector<std::byte> rawRgba(w * h * 4, static_cast<std::byte>(100));

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = w * 4;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  // 1. Multithreading maxWorkers = 4
  EncodeOptions opts;
  opts.execution.maxWorkers = 2;
  opts.quality = Quality::Fast;

  auto res4 = encode_bc7_mip(mip, opts);
  assert(res4.has_value());

  // 2. Single thread maxWorkers = 1
  opts.execution.maxWorkers = 1;
  auto res1 = encode_bc7_mip(mip, opts);
  assert(res1.has_value());

  // Payload size must match
  assert(res4.value().payload.size() == res1.value().payload.size());

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
