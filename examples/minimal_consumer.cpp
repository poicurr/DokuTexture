#include "DokuTexture/Encode.hpp"
#include "DokuTexture/Ktx2.hpp"
#include "DokuTexture/Transcode.hpp"

#include <iostream>
#include <vector>

int main() {
  using namespace doku::texture;

  std::cout << "DokuTexture Minimal Consumer Example" << std::endl;

  // 1. Prepare 64x64 RGBA8 test mip
  std::uint32_t w = 64, h = 64;
  std::vector<std::byte> rawRgba(w * h * 4, static_cast<std::byte>(200));

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = w * 4;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  // 2. Encode to BC7_UNorm
  EncodeOptions opts;
  opts.target = GpuFormat::BC7_UNorm;
  opts.quality = Quality::Fast;
  opts.execution.maxWorkers = 2;

  auto encRes = encode_bc7_mip(mip, opts);
  if (!encRes) {
    std::cerr << "Encoding failed: " << encRes.error().message << std::endl;
    return 1;
  }

  CompressedTexture encTex = std::move(encRes.value());
  std::cout << "Encoded 64x64 RGBA8 to BC7 payload: " << encTex.payload.size()
            << " bytes." << std::endl;

  // 3. Write to Native BC7 KTX2 container
  CompressedMipView mipView;
  mipView.layout = encTex.mips[0];
  mipView.bytes = ByteSpan{encTex.payload.data(), encTex.payload.size()};

  CompressedTextureView texView;
  texView.format = encTex.format;
  texView.metadata = encTex.metadata;
  texView.mips = Span<CompressedMipView>{&mipView, 1};

  ExecutionOptions execOpts;
  auto writeRes = write_ktx2_native_bc7(texView, execOpts);
  if (!writeRes) {
    std::cerr << "KTX2 write failed: " << writeRes.error().message << std::endl;
    return 1;
  }

  OwningBuffer ktx2File = std::move(writeRes.value());
  std::cout << "Generated KTX2 file size: " << ktx2File.size() << " bytes."
            << std::endl;

  // 4. Inspect KTX2 file
  ByteSpan fileSpan{ktx2File.data(), ktx2File.size()};
  auto inspectRes = inspect_ktx2(fileSpan, execOpts);
  if (inspectRes) {
    std::cout << "KTX2 Inspect: " << inspectRes.value().width << "x"
              << inspectRes.value().height
              << ", Mip Levels: " << inspectRes.value().levelCount
              << ", Needs Transcode: "
              << (inspectRes.value().needsTranscode ? "Yes" : "No")
              << std::endl;
  }

  // 5. Read back native BC7 KTX2
  auto readRes = read_ktx2_native_bc7(fileSpan, execOpts);
  if (readRes) {
    std::cout << "Read native BC7 KTX2 payload size: "
              << readRes.value().payload.size() << " bytes." << std::endl;
  }

  std::cout << "Minimal Consumer Example Completed Successfully!" << std::endl;
  return 0;
}
