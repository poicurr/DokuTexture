#include "DokuTexture/Encode.hpp"
#include "bc7decomp.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace doku::texture;

using Clock = std::chrono::steady_clock;

enum class Pattern { Gradient, TopQuarter, Interleaved, Noise };

struct Workload {
  const char *name;
  std::uint32_t width;
  std::uint32_t height;
  Quality quality;
  unsigned callsPerBatch = 1;
  Pattern pattern = Pattern::Gradient;
  bool chain = false;
};

OwningBuffer generate_pattern(std::uint32_t w, std::uint32_t h,
                              Pattern pattern) {
  OwningBuffer data(static_cast<std::size_t>(w) * h * 4);
  std::uint32_t state = 1234567;
  // Consume the same noise sequence in block order for both mixed patterns.
  for (std::uint32_t by = 0; by < (h + 3) / 4; ++by) {
    const bool noisy = pattern == Pattern::Noise ||
                       (pattern == Pattern::TopQuarter && by < (h + 3) / 16) ||
                       (pattern == Pattern::Interleaved && by % 4 == 0);
    for (std::uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
      for (auto y = by * 4; y < std::min(by * 4 + 4, h); ++y)
        for (auto x = bx * 4; x < std::min(bx * 4 + 4, w); ++x) {
          const auto i = (std::size_t(y) * w + x) * 4;
          if (noisy) {
            for (unsigned c = 0; c < 3; ++c) {
              state ^= state << 13;
              state ^= state >> 17;
              state ^= state << 5;
              data[i + c] = std::byte(state & 255);
            }
          } else if (pattern == Pattern::Gradient) {
            data[i] = std::byte(x * 255 / w);
            data[i + 1] = std::byte(y * 255 / h);
            data[i + 2] = std::byte((x + y) * 255 / (w + h));
          } else {
            std::fill_n(data.data() + i, 3, std::byte{128});
          }
          data[i + 3] = std::byte{255};
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

Result<CompressedTexture> encode(const Workload &workload,
                                 const std::vector<Rgba8MipView> &mips,
                                 std::uint32_t workers) {
  EncodeOptions opts;
  opts.quality = workload.quality;
  opts.execution.maxWorkers = workers;
  return workload.chain ? encode_bc7_chain({mips.data(), mips.size()}, opts)
                        : encode_bc7_mip(mips.front(), opts);
}

bool check_encode(const Workload &workload, std::uint32_t workers,
                  const Result<CompressedTexture> &result) {
  if (result)
    return true;
  std::cerr << "Encode failed for " << workload.name
            << " (maxWorkers=" << workers << "): " << result.error().message
            << '\n';
  return false;
}

bool run_benchmark(const Workload &workload) {
  std::vector<OwningBuffer> pixels;
  std::vector<Rgba8MipView> mips;
  std::size_t inputBytes = 0;
  auto w = workload.width, h = workload.height;
  for (;;) {
    pixels.push_back(generate_pattern(w, h, workload.pattern));
    const auto &data = pixels.back();
    mips.push_back({w, h, std::uint64_t(w) * 4, {data.data(), data.size()}});
    inputBytes += data.size();
    if (!workload.chain || (w == 1 && h == 1))
      break;
    w = std::max(1U, w / 2);
    h = std::max(1U, h / 2);
  }

  constexpr std::array<std::uint32_t, 2> WORKER_LIMITS = {1, 4};
  auto serial = encode(workload, mips, WORKER_LIMITS[0]);
  if (!check_encode(workload, WORKER_LIMITS[0], serial))
    return false;
  {
    auto parallel = encode(workload, mips, WORKER_LIMITS[1]);
    if (!check_encode(workload, WORKER_LIMITS[1], parallel))
      return false;
    if (serial.value().payload != parallel.value().payload) {
      std::cerr << "Worker limits produced different payloads for "
                << workload.name << '\n';
      return false;
    }
  }

  constexpr std::size_t BATCHES = 7;
  std::array<std::array<double, BATCHES>, WORKER_LIMITS.size()> times{};
  for (std::size_t sample = 0; sample < BATCHES; ++sample) {
    for (std::size_t i = 0; i < WORKER_LIMITS.size(); ++i) {
      const auto slot = (i + sample) % WORKER_LIMITS.size();
      const auto start = Clock::now();
      for (unsigned call = 0; call < workload.callsPerBatch; ++call) {
        auto result = encode(workload, mips, WORKER_LIMITS[slot]);
        if (!check_encode(workload, WORKER_LIMITS[slot], result))
          return false;
      }
      times[slot][sample] =
          std::chrono::duration<double, std::micro>(Clock::now() - start)
              .count() /
          workload.callsPerBatch;
    }
  }

  const auto &payload = serial.value().payload;
  const auto psnr =
      compute_psnr(pixels.front(), workload.width, workload.height, payload);
  const auto size =
      std::to_string(workload.width) + "x" + std::to_string(workload.height);
  for (std::size_t slot = 0; slot < WORKER_LIMITS.size(); ++slot) {
    auto &samples = times[slot];
    std::sort(samples.begin(), samples.end());
    const auto medianUs = samples[BATCHES / 2];
    const auto mpPerSec = (inputBytes / 4.0) / medianUs;
    std::cout << std::left << std::setw(23) << workload.name << std::setw(12)
              << size << std::setw(10) << WORKER_LIMITS[slot] << std::fixed
              << std::setprecision(2) << std::setw(15) << medianUs
              << std::setw(15) << mpPerSec << std::setw(14) << psnr
              << inputBytes << " -> " << payload.size() << '\n';
  }
  return true;
}

int main() {
  // A separate one-block call initializes the backend before all warm-ups.
  constexpr Workload warmup{"Cold 1x1", 1, 1, Quality::Fast};
  const auto pixel = generate_pattern(1, 1, Pattern::Gradient);
  const std::vector<Rgba8MipView> mips{{1, 1, 4, {pixel.data(), pixel.size()}}};
  const auto start = Clock::now();
  const auto result = encode(warmup, mips, 1);
  const auto coldUs =
      std::chrono::duration<double, std::micro>(Clock::now() - start).count();
  if (!check_encode(warmup, 1, result))
    return 1;

  std::cout << "Cold 1x1 encode (includes backend initialization): " << coldUs
            << " us\n";
  std::cout << std::left << std::setw(23) << "Workload" << std::setw(12)
            << "Size" << std::setw(10) << "Workers" << std::setw(15)
            << "Median (us)" << std::setw(15) << "Mpixels/sec" << std::setw(14)
            << "RGB PSNR (dB)" << "Input -> Output (bytes)\n";

  constexpr Workload workloads[] = {
      {"Fast gradient", 8, 8, Quality::Fast, 300},
      {"Fast gradient", 32, 32, Quality::Fast, 100},
      {"Fast gradient", 64, 64, Quality::Fast, 30},
      {"Fast gradient chain", 64, 64, Quality::Fast, 30, Pattern::Gradient,
       true},
      {"Fast gradient", 128, 128, Quality::Fast, 10},
      {"Balanced top quarter", 512, 512, Quality::Balanced, 2,
       Pattern::TopQuarter},
      {"Balanced interleaved", 512, 512, Quality::Balanced, 2,
       Pattern::Interleaved},
      {"Fast noise", 16384, 4, Quality::Fast, 3, Pattern::Noise},
      {"Fast noise", 256, 256, Quality::Fast, 3, Pattern::Noise},
      {"Fast gradient chain", 256, 256, Quality::Fast, 3, Pattern::Gradient,
       true},
      {"Fast gradient", 512, 512, Quality::Fast},
      {"Balanced gradient", 512, 512, Quality::Balanced},
      {"High gradient", 512, 512, Quality::High},
      {"Fast gradient", 2048, 2048, Quality::Fast},
      {"Balanced gradient", 2048, 2048, Quality::Balanced},
      {"High gradient", 2048, 2048, Quality::High},
  };
  for (const auto &workload : workloads)
    if (!run_benchmark(workload))
      return 1;
  std::cout << "All workloads succeeded with matching 1/4-worker payloads.\n";
  return 0;
}
