#include "bc7decomp.h"
#include "doku/texture/ktx2.hpp"
#include "doku/texture/transcode.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace doku::texture;

int main() {
  for (const auto *name : {"etc1s-srgb.ktx2", "etc1s-rgb-srgb.ktx2",
                           "uastc-linear.ktx2", "uastc-zstd-srgb.ktx2"}) {
    std::ifstream in(std::string(TP_FIXTURE_DIR) + "/" + name,
                     std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(in)), {});
    assert(!raw.empty());
    ByteSpan file{reinterpret_cast<const std::byte *>(raw.data()), raw.size()};
    ExecutionOptions e;
    auto info = inspect_ktx2(file, e);
    assert(info && info.value().needsTranscode);
    assert(info.value().width == 8 && info.value().height == 8 &&
           info.value().levelCount == 4);
    auto native = read_ktx2_native_bc7(file, e);
    assert(!native &&
           native.error().code == ErrorCode::UnsupportedSupercompression);
    const auto target = info.value().metadata.colorSpace == ColorSpace::SRGB
                            ? GpuFormat::BC7_Srgb
                            : GpuFormat::BC7_UNorm;
    auto result = transcode_basis_ktx2(file, target, e);
    assert(result);
    auto texture = std::move(result.value());
    assert(texture.payload.size() == 112 && texture.mips.size() == 4);
    unsigned maxAlphaError = 0;
    unsigned maxRgbError = 0;
    double rgbSquaredError = 0;
    for (unsigned by = 0; by < 2; ++by)
      for (unsigned bx = 0; bx < 2; ++bx) {
        bc7decomp::color_rgba decoded[16];
        assert(bc7decomp::unpack_bc7(
            texture.payload.data() + (by * 2 + bx) * 16, decoded));
        for (unsigned y = 0; y < 4; ++y)
          for (unsigned x = 0; x < 4; ++x) {
            const int expected = std::string(name) == "etc1s-rgb-srgb.ktx2"
                                     ? 255
                                     : ((by * 4 + y) + (bx * 4 + x)) * 255 / 14;
            maxAlphaError =
                std::max(maxAlphaError,
                         static_cast<unsigned>(
                             std::abs(int(decoded[y * 4 + x].a) - expected)));
            const int source[3] = {int((bx * 4 + x) * 255 / 7),
                                   int((by * 4 + y) * 255 / 7), 100};
            const int decodedRgb[3] = {decoded[y * 4 + x].r,
                                       decoded[y * 4 + x].g,
                                       decoded[y * 4 + x].b};
            for (unsigned c = 0; c < 3; ++c) {
              const int error = decodedRgb[c] - source[c];
              maxRgbError =
                  std::max(maxRgbError, static_cast<unsigned>(std::abs(error)));
              rgbSquaredError += error * error;
            }
          }
      }
    std::cout << name << " alpha max absolute error: " << maxAlphaError << '\n';
    const double psnr =
        10 * std::log10(255.0 * 255 * 64 * 3 / std::max(1.0, rgbSquaredError));
    std::cout << "RGB max absolute error: " << maxRgbError
              << ", encoded-value RGB PSNR: " << psnr << " dB" << std::endl;
    const bool etc = info.value().sourceEncoding == SourceEncoding::BasisETC1S;
    const bool opaque = std::string(name) == "etc1s-rgb-srgb.ktx2";
    const double baselinePsnr = opaque ? 17.8329 : (etc ? 17.2155 : 30.0974);
    assert(psnr >=
           baselinePsnr - 1.0); // fixed fixture baseline with 1 dB tolerance
    assert(maxAlphaError <= (opaque ? 2U : (etc ? 21U : 36U)));
    for (std::size_t i = 0; i < 4; ++i) {
      assert(texture.mips[i].width == (8U >> i));
      assert(texture.mips[i].height == (8U >> i));
    }
    raw.clear();
    raw.shrink_to_fit();
    for (std::size_t offset = 0; offset < texture.payload.size();
         offset += 16) {
      bc7decomp::color_rgba decoded[16];
      assert(bc7decomp::unpack_bc7(texture.payload.data() + offset, decoded));
    }
    std::ifstream again(std::string(TP_FIXTURE_DIR) + "/" + name,
                        std::ios::binary);
    raw.assign(std::istreambuf_iterator<char>(again), {});
    file = {reinterpret_cast<const std::byte *>(raw.data()), raw.size()};
    auto unsupported = transcode_basis_ktx2(file, GpuFormat::ETC1_RGB, e);
    assert(!unsupported &&
           unsupported.error().code == ErrorCode::UnsupportedFormat);
    auto mismatch = transcode_basis_ktx2(file,
                                         target == GpuFormat::BC7_Srgb
                                             ? GpuFormat::BC7_UNorm
                                             : GpuFormat::BC7_Srgb,
                                         e);
    assert(!mismatch && mismatch.error().code == ErrorCode::UnsupportedOption);
    e.limits.maxOutputBytes = 16;
    auto limited = transcode_basis_ktx2(file, target, e);
    assert(!limited &&
           limited.error().code == ErrorCode::ResourceLimitExceeded);
    e = {};
    e.limits.maxWorkingBytes = 16;
    limited = transcode_basis_ktx2(file, target, e);
    assert(!limited &&
           limited.error().code == ErrorCode::ResourceLimitExceeded);
    e = {};
    std::atomic_bool cancel{true};
    e.cancelFlag = &cancel;
    auto cancelled = transcode_basis_ktx2(file, target, e);
    assert(!cancelled && cancelled.error().code == ErrorCode::Cancelled);
    e = {};
    for (auto length : {std::size_t(79), raw.size() - 1}) {
      auto truncated = transcode_basis_ktx2({file.data, length}, target, e);
      assert(!truncated);
    }
    std::uint32_t layers = 2;
    std::memcpy(raw.data() + 32, &layers, 4);
    auto array = transcode_basis_ktx2(file, target, e);
    assert(!array && array.error().code == ErrorCode::UnsupportedTextureType);
    layers = 0;
    std::memcpy(raw.data() + 32, &layers, 4);
    std::uint32_t dfdOffset;
    std::memcpy(&dfdOffset, raw.data() + 48, 4);
    const auto savedPrimaries = raw[dfdOffset + 13];
    raw[dfdOffset + 13] =
        10; // valid Display P3 primaries cannot be represented by this API
    auto primaries = transcode_basis_ktx2(file, target, e);
    assert(!primaries &&
           primaries.error().code == ErrorCode::UnsupportedMetadata);
    raw[dfdOffset + 13] = savedPrimaries;
    // Valid but out-of-scope Red channel semantics must not be silently mapped
    // to BC7 RGB.
    const auto savedChannel = raw[dfdOffset + 31];
    raw[dfdOffset + 31] =
        info.value().sourceEncoding == SourceEncoding::BasisETC1S ? 3 : 4;
    auto red = transcode_basis_ktx2(file, target, e);
    assert(!red && red.error().code == ErrorCode::UnsupportedMetadata);
    raw[dfdOffset + 31] = savedChannel;
    if (info.value().sourceEncoding == SourceEncoding::BasisETC1S) {
      const std::uint64_t noGlobalData = 0;
      std::memcpy(raw.data() + 72, &noGlobalData, 8);
      auto missing = transcode_basis_ktx2(file, target, e);
      assert(!missing && missing.error().code == ErrorCode::InvalidContainer);
    }
  }
}
