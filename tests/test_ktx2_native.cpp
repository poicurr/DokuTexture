#include "doku/texture/encode.hpp"
#include "doku/texture/ktx2.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace doku::texture;

void test_ktx2_roundtrip_bc7_unorm() {
  std::cout
      << "[Test] KTX2 Native BC7 UNorm Roundtrip (Encode -> Write -> Read)"
      << std::endl;

  std::uint32_t w = 16, h = 16;
  std::vector<std::byte> rawRgba(w * h * 4, static_cast<std::byte>(180));

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = w * 4;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  EncodeOptions opts;
  opts.target = GpuFormat::BC7_UNorm;
  opts.metadata.colorSpace = ColorSpace::Linear;

  auto encRes = encode_bc7_mip(mip, opts);
  assert(encRes.has_value());

  CompressedTexture encTex = std::move(encRes.value());

  CompressedMipView mipView;
  mipView.layout = encTex.mips[0];
  mipView.bytes = ByteSpan{encTex.payload.data(), encTex.payload.size()};

  CompressedTextureView texView;
  texView.format = encTex.format;
  texView.metadata = encTex.metadata;
  texView.mips = Span<CompressedMipView>{&mipView, 1};

  ExecutionOptions execOpts;
  auto writeRes = write_ktx2_native_bc7(texView, execOpts);
  assert(writeRes.has_value());

  OwningBuffer ktx2File = std::move(writeRes.value());
  ByteSpan fileSpan{ktx2File.data(), ktx2File.size()};

  // 1. Inspect file
  auto inspectRes = inspect_ktx2(fileSpan, execOpts);
  assert(inspectRes.has_value());

  const auto &info = inspectRes.value();
  assert(info.sourceEncoding == SourceEncoding::NativeBC7);
  assert(info.targetFormat == GpuFormat::BC7_UNorm);
  assert(!info.needsTranscode);
  assert(info.width == 16);
  assert(info.height == 16);
  assert(info.levelCount == 1);
  assert(info.supercompressionScheme == 0);

  // 2. Read back
  auto readRes = read_ktx2_native_bc7(fileSpan, execOpts);
  assert(readRes.has_value());

  const auto &readTex = readRes.value();
  assert(readTex.format == GpuFormat::BC7_UNorm);
  assert(readTex.mips.size() == 1);
  assert(readTex.mips[0].width == 16);
  assert(readTex.mips[0].height == 16);

  // Byte-exact payload roundtrip check
  assert(readTex.payload.size() == encTex.payload.size());
  assert(std::memcmp(readTex.payload.data(), encTex.payload.data(),
                     encTex.payload.size()) == 0);

  std::cout << "  -> PASSED" << std::endl;
}

void test_ktx2_roundtrip_bc7_srgb_chain() {
  std::cout << "[Test] KTX2 Native BC7 SRGB Chain Roundtrip (3 levels)"
            << std::endl;

  std::vector<std::vector<std::byte>> rawMips(3);
  std::vector<Rgba8MipView> mips(3);
  std::uint32_t dims[3] = {32, 16, 8};

  for (int l = 0; l < 3; ++l) {
    std::uint32_t d = dims[l];
    rawMips[l].resize(d * d * 4, static_cast<std::byte>(210));

    mips[l].width = d;
    mips[l].height = d;
    mips[l].rowPitchBytes = d * 4;
    mips[l].bytes = ByteSpan{rawMips[l].data(), rawMips[l].size()};
  }

  EncodeOptions opts;
  opts.target = GpuFormat::BC7_Srgb;
  opts.metadata.colorSpace = ColorSpace::SRGB;

  Span<Rgba8MipView> chainView{mips.data(), mips.size()};
  auto encRes = encode_bc7_chain(chainView, opts);
  assert(encRes.has_value());

  CompressedTexture encTex = std::move(encRes.value());

  std::vector<CompressedMipView> mipViews(3);
  for (int l = 0; l < 3; ++l) {
    mipViews[l].layout = encTex.mips[l];
    mipViews[l].bytes =
        ByteSpan{encTex.payload.data() + encTex.mips[l].offsetBytes,
                 encTex.mips[l].sizeBytes};
  }

  CompressedTextureView texView;
  texView.format = encTex.format;
  texView.metadata = encTex.metadata;
  texView.mips = Span<CompressedMipView>{mipViews.data(), mipViews.size()};

  ExecutionOptions execOpts;
  auto writeRes = write_ktx2_native_bc7(texView, execOpts);
  assert(writeRes.has_value());

  OwningBuffer ktx2File = std::move(writeRes.value());
  ByteSpan fileSpan{ktx2File.data(), ktx2File.size()};

  // Inspect
  auto inspectRes = inspect_ktx2(fileSpan, execOpts);
  assert(inspectRes.has_value());
  assert(inspectRes.value().targetFormat == GpuFormat::BC7_Srgb);
  assert(inspectRes.value().levelCount == 3);

  // Read back
  auto readRes = read_ktx2_native_bc7(fileSpan, execOpts);
  assert(readRes.has_value());

  const auto &readTex = readRes.value();
  assert(readTex.format == GpuFormat::BC7_Srgb);
  assert(readTex.mips.size() == 3);

  assert(readTex.payload.size() == encTex.payload.size());
  assert(std::memcmp(readTex.payload.data(), encTex.payload.data(),
                     encTex.payload.size()) == 0);

  std::cout << "  -> PASSED" << std::endl;
}

void test_ktx2_corrupted_inputs() {
  std::cout << "[Test] KTX2 Corrupted Inputs & Error Codes" << std::endl;

  ExecutionOptions execOpts;

  // 1. Truncated input
  std::vector<std::byte> truncated(30, std::byte{0});
  auto res =
      inspect_ktx2(ByteSpan{truncated.data(), truncated.size()}, execOpts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::TruncatedInput);

  // 2. Invalid magic identifier
  std::vector<std::byte> invalidMagic(100, std::byte{0});
  res = inspect_ktx2(ByteSpan{invalidMagic.data(), invalidMagic.size()},
                     execOpts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::InvalidContainer);

  std::cout << "  -> PASSED" << std::endl;
}

int main() {
  std::cout << "========================================" << std::endl;
  std::cout << " Running test_ktx2_native" << std::endl;
  std::cout << "========================================" << std::endl;

  test_ktx2_roundtrip_bc7_unorm();
  test_ktx2_roundtrip_bc7_srgb_chain();
  test_ktx2_corrupted_inputs();

  std::cout << "\nALL Stage 2 KTX2 Native Tests Passed Successfully!"
            << std::endl;
  return 0;
}
