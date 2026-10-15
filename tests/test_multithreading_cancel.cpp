#include "DokuTexture/Encode.hpp"
#include "DokuTexture/Ktx2.hpp"
#include "DokuTexture/Transcode.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>
#include <vector>

using namespace doku::texture;

void test_concurrent_independent_calls() {
  std::cout << "[Test] Concurrent Independent Calls across Threads"
            << std::endl;

  constexpr int kNumThreads = 2;
  std::vector<std::thread> threads;
  std::atomic<int> successCount{0};

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&successCount]() {
      std::uint32_t w = 64, h = 64;
      std::vector<std::byte> rawRgba(w * h * 4, static_cast<std::byte>(150));

      Rgba8MipView mip;
      mip.width = w;
      mip.height = h;
      mip.rowPitchBytes = w * 4;
      mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

      EncodeOptions opts;
      opts.target = GpuFormat::BC7_UNorm;
      opts.quality = Quality::Fast;

      auto res = encode_bc7_mip(mip, opts);
      if (res.has_value() && res.value().payload.size() == 4096) {
        successCount.fetch_add(1);
      }
    });
  }

  for (auto &th : threads) {
    if (th.joinable())
      th.join();
  }

  assert(successCount.load() == kNumThreads);
  std::cout << "  -> PASSED" << std::endl;
}

void test_immediate_cancel() {
  std::cout << "[Test] Immediate Cancellation Test" << std::endl;

  std::uint32_t w = 512, h = 512;
  std::vector<std::byte> rawRgba(w * h * 4, static_cast<std::byte>(200));

  Rgba8MipView mip;
  mip.width = w;
  mip.height = h;
  mip.rowPitchBytes = w * 4;
  mip.bytes = ByteSpan{rawRgba.data(), rawRgba.size()};

  std::atomic_bool cancelFlag{true};

  EncodeOptions opts;
  opts.execution.cancelFlag = &cancelFlag;

  auto res = encode_bc7_mip(mip, opts);
  assert(!res.has_value());
  assert(res.error().code == ErrorCode::Cancelled);

  std::cout << "  -> PASSED" << std::endl;
}

void test_cancel_during_processing_and_reuse() {
  std::vector<std::byte> pixels(512 * 512 * 4);
  for (std::size_t i = 0; i < pixels.size(); ++i)
    pixels[i] = static_cast<std::byte>((i * 71 + i / 17) % 256);
  std::atomic_bool cancel{false};
  EncodeOptions opts;
  opts.quality = Quality::High;
  opts.execution.cancelFlag = &cancel;
  std::thread requestCancel([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    cancel.store(true, std::memory_order_relaxed);
  });
  auto result =
      encode_bc7_mip({512, 512, 2048, {pixels.data(), pixels.size()}}, opts);
  requestCancel.join();
  assert(!result && result.error().code == ErrorCode::Cancelled);
  cancel.store(false);
  auto reuse = encode_bc7_mip({1, 1, 4, {pixels.data(), 4}}, opts);
  assert(reuse && reuse.value().payload.size() == 16);
}

int main() {
  std::cout << "========================================" << std::endl;
  std::cout << " Running test_multithreading_cancel" << std::endl;
  std::cout << "========================================" << std::endl;

  test_concurrent_independent_calls();
  test_immediate_cancel();
  test_cancel_during_processing_and_reuse();

  std::cout << "\nALL Multithreading & Cancel Tests Passed Successfully!"
            << std::endl;
  return 0;
}
