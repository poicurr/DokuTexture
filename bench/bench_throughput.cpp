#include "DokuTexture/Encode.hpp"
#include "bc7decomp.h"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace doku::texture;

struct BenchResult {
  std::string name;
  std::uint32_t width, height;
  Quality quality;
  double durationMs;
  double megapixelsPerSec;
  double psnrDb;
  std::size_t inputBytes;
  std::size_t outputBytes;
};

std::vector<std::byte> generate_gradient_pattern(std::uint32_t w,
                                                 std::uint32_t h) {
  std::vector<std::byte> data(static_cast<std::size_t>(w) * h * 4);
  for (std::uint32_t y = 0; y < h; ++y) {
    for (std::uint32_t x = 0; x < w; ++x) {
      std::size_t idx = (static_cast<std::size_t>(y) * w + x) * 4;
      data[idx + 0] = static_cast<std::byte>((x * 255) / w);
      data[idx + 1] = static_cast<std::byte>((y * 255) / h);
      data[idx + 2] = static_cast<std::byte>(((x + y) * 255) / (w + h));
      data[idx + 3] = static_cast<std::byte>(255);
    }
  }
  return data;
}

double compute_psnr(const std::vector<std::byte> &originalRgba, std::uint32_t w,
                    std::uint32_t h, const OwningBuffer &bc7Payload) {
  std::uint32_t blocksX = (w + 3) / 4;
  std::uint32_t blocksY = (h + 3) / 4;

  double mseSum = 0.0;
  std::uint64_t totalPixels = 0;

  for (std::uint32_t by = 0; by < blocksY; ++by) {
    for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
      std::size_t blockOffset = (by * blocksX + bx) * 16;
      bc7decomp::color_rgba decoded[16];
      bc7decomp::unpack_bc7(bc7Payload.data() + blockOffset, decoded);

      for (std::uint32_t py = 0; py < 4; ++py) {
        std::uint32_t iy = by * 4 + py;
        if (iy >= h)
          continue;

        for (std::uint32_t px = 0; px < 4; ++px) {
          std::uint32_t ix = bx * 4 + px;
          if (ix >= w)
            continue;

          std::size_t origIdx = (iy * w + ix) * 4;
          int rOrig = static_cast<int>(originalRgba[origIdx + 0]);
          int gOrig = static_cast<int>(originalRgba[origIdx + 1]);
          int bOrig = static_cast<int>(originalRgba[origIdx + 2]);

          bc7decomp::color_rgba dec = decoded[py * 4 + px];
          int rDec = dec.r;
          int gDec = dec.g;
          int bDec = dec.b;

          double dr = rOrig - rDec;
          double dg = gOrig - gDec;
          double db = bOrig - bDec;

          mseSum += (dr * dr + dg * dg + db * db) / 3.0;
          totalPixels++;
        }
      }
    }
  }

  if (totalPixels == 0)
    return 99.0;
  double mse = mseSum / totalPixels;
  if (mse < 1e-10)
    return 99.0;

  return 10.0 * std::log10((255.0 * 255.0) / mse);
}

BenchResult run_benchmark(std::uint32_t w, std::uint32_t h, Quality quality,
                          std::uint32_t threads) {
  auto rawData = generate_gradient_pattern(w, h);

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = static_cast<std::uint64_t>(w) * 4;
  mip.bytes = ByteSpan{rawData.data(), rawData.size()};

  EncodeOptions opts;
  opts.target = GpuFormat::BC7_UNorm;
  opts.quality = quality;
  opts.execution.maxWorkers = threads;

  auto start = std::chrono::high_resolution_clock::now();
  auto res = encode_bc7_mip(mip, opts);
  auto end = std::chrono::high_resolution_clock::now();

  double durationMs =
      std::chrono::duration<double, std::milli>(end - start).count();
  double totalMpixels = (static_cast<double>(w) * h) / 1000000.0;
  double mpPerSec = totalMpixels / (durationMs / 1000.0);

  double psnr = 0.0;
  std::size_t outputBytes = 0;
  if (res.has_value()) {
    outputBytes = res.value().payload.size();
    psnr = compute_psnr(rawData, w, h, res.value().payload);
  }

  std::string qName = "Balanced";
  if (quality == Quality::Fast)
    qName = "Fast";
  if (quality == Quality::High)
    qName = "High";

  BenchResult result;
  result.name = qName + " (" + std::to_string(threads) + "T)";
  result.width = w;
  result.height = h;
  result.quality = quality;
  result.durationMs = durationMs;
  result.megapixelsPerSec = mpPerSec;
  result.psnrDb = psnr;
  result.inputBytes = rawData.size();
  result.outputBytes = outputBytes;

  return result;
}

int main() {
  std::cout << "==============================================================="
               "==========================="
            << std::endl;
  std::cout << " DokuTexture BC7 Encoding Performance Benchmark" << std::endl;
  std::cout << "==============================================================="
               "==========================="
            << std::endl;

  std::cout << std::left << std::setw(18) << "Preset / Threads" << std::setw(12)
            << "Size" << std::setw(14) << "Time (ms)" << std::setw(18)
            << "Mpixels/sec" << std::setw(14) << "PSNR (dB)" << std::setw(18)
            << "Input -> Output" << std::endl;
  std::cout << "---------------------------------------------------------------"
               "---------------------------"
            << std::endl;

  std::vector<std::pair<std::uint32_t, std::uint32_t>> sizes = {{512, 512},
                                                                {2048, 2048}};

  for (auto [w, h] : sizes) {
    for (Quality q : {Quality::Fast, Quality::Balanced, Quality::High}) {
      for (std::uint32_t threads : {1U, 4U}) {
        auto r = run_benchmark(w, h, q, threads);

        std::string sizeStr = std::to_string(w) + "x" + std::to_string(h);
        std::string bytesStr = std::to_string(r.inputBytes / 1024) + "KB -> " +
                               std::to_string(r.outputBytes / 1024) + "KB";

        std::cout << std::left << std::setw(18) << r.name << std::setw(12)
                  << sizeStr << std::fixed << std::setprecision(2)
                  << std::setw(14) << r.durationMs << std::setw(18)
                  << r.megapixelsPerSec << std::setw(14) << r.psnrDb
                  << std::setw(18) << bytesStr << std::endl;
      }
    }
  }

  std::cout << "==============================================================="
               "==========================="
            << std::endl;
  return 0;
}
