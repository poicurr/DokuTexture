#include "DokuTexture/Encode.hpp"
#include "bc7enc.h"
#include "validation.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <vector>

namespace doku::texture {

namespace {

static std::once_flag g_bc7enc_init_flag;

void ensure_bc7enc_initialized() {
  std::call_once(g_bc7enc_init_flag, []() { bc7enc_compress_block_init(); });
}

inline std::uint32_t calc_mip_dim(std::uint32_t baseDim, std::uint32_t level) {
  std::uint32_t d = baseDim >> level;
  return d > 0 ? d : 1;
}

bool make_layout(Rgba8MipView mip, std::uint32_t level, std::uint64_t offset,
                 MipLayout &layout) {
  layout.level = level;
  layout.width = mip.width;
  layout.height = mip.height;
  layout.blocksX = detail::blocks(mip.width);
  layout.blocksY = detail::blocks(mip.height);
  layout.offsetBytes = offset;
  layout.rowPitchBytes = std::uint64_t(layout.blocksX) * 16;
  return detail::multiply(layout.rowPitchBytes, layout.blocksY,
                          layout.sizeBytes);
}

bool init_params(const EncodeOptions &options,
                 bc7enc_compress_block_params &params) {
  bc7enc_compress_block_params_init(&params);
  if (options.target == GpuFormat::BC7_UNorm) {
    bc7enc_compress_block_params_init_linear_weights(&params);
  } else {
    bc7enc_compress_block_params_init_perceptual_weights(&params);
  }

  switch (options.quality) {
    case Quality::Fast:
      params.m_uber_level = 0;
      params.m_max_partitions_mode = 16;
      params.m_try_least_squares = BC7ENC_FALSE;
      return true;
    case Quality::Balanced:
    case Quality::High:
      params.m_uber_level = options.quality == Quality::Balanced ? 1 : 2;
      params.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
      params.m_try_least_squares = BC7ENC_TRUE;
      return true;
  }
  return false;
}

constexpr std::size_t BLOCKS_PER_CHUNK = 16;
// Leave enough work per participant to amortize thread startup and scheduling.
constexpr std::size_t MIN_BLOCKS_PER_WORKER = 64;

struct MipJob {
  Rgba8MipView mip;
  std::uint32_t blocksX;
  std::size_t blockCount;
  std::byte *output;
  const bc7enc_compress_block_params &params;
  const ExecutionOptions &execution;
  std::atomic<std::size_t> nextBlock{0};
  std::atomic_bool cancelled{false};

  void encode_range(std::size_t begin, std::size_t end) noexcept {
    auto by = static_cast<std::uint32_t>(begin / blocksX);
    auto bx = static_cast<std::uint32_t>(begin % blocksX);
    auto *dst = output + begin * 16;
    for (auto block = begin; block < end; ++block) {
      if (detail::cancelled(execution)) {
        cancelled.store(true, std::memory_order_relaxed);
        return;
      }
      alignas(16) std::uint8_t blockPixels[64];
      for (std::uint32_t py = 0; py < 4; ++py) {
        const auto iy = std::min(by * 4 + py, mip.height - 1);
        const auto *srcRow =
            mip.bytes.data + static_cast<std::size_t>(iy * mip.rowPitchBytes);
        for (std::uint32_t px = 0; px < 4; ++px) {
          const auto ix = std::min(bx * 4 + px, mip.width - 1);
          std::memcpy(blockPixels + (py * 4 + px) * 4,
                      srcRow + static_cast<std::size_t>(ix) * 4, 4);
        }
      }
      bc7enc_compress_block(dst, blockPixels, &params);
      dst += 16;
      if (++bx == blocksX) {
        bx = 0;
        ++by;
      }
    }
  }

  void work() noexcept {
    while (!cancelled.load(std::memory_order_relaxed)) {
      const auto begin =
          nextBlock.fetch_add(BLOCKS_PER_CHUNK, std::memory_order_relaxed);
      if (begin >= blockCount)
        return;
      encode_range(begin, std::min(begin + BLOCKS_PER_CHUNK, blockCount));
    }
  }
};

class EncodeWorkers {
public:
  explicit EncodeWorkers(std::size_t count) {
    m_threads.reserve(count);
    try {
      for (std::size_t i = 0; i < count; ++i)
        m_threads.emplace_back([this, i] { worker(i); });
    } catch (...) {
      stop();
      throw;
    }
  }

  ~EncodeWorkers() { stop(); }

  void run(MipJob &job) {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_job = &job;
      m_active = std::min(m_threads.size(),
                          job.blockCount / MIN_BLOCKS_PER_WORKER - 1);
      m_remaining = m_active;
      ++m_generation;
    }
    m_ready.notify_all();
    job.work();
    std::unique_lock<std::mutex> lock(m_mutex);
    m_done.wait(lock, [&] { return m_remaining == 0; });
    m_job = nullptr;
  }

private:
  void worker(std::size_t index) {
    std::unique_lock<std::mutex> lock(m_mutex);
    std::size_t generation = 0;
    for (;;) {
      m_ready.wait(lock,
                   [&] { return m_stopping || m_generation != generation; });
      if (m_stopping)
        return;
      generation = m_generation;
      if (index >= m_active)
        continue;
      auto &job = *m_job;
      lock.unlock();
      job.work();
      lock.lock();
      if (--m_remaining == 0)
        m_done.notify_one();
    }
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_stopping = true;
    }
    m_ready.notify_all();
    for (auto &thread : m_threads)
      thread.join();
  }

  std::mutex m_mutex;
  std::condition_variable m_ready;
  std::condition_variable m_done;
  std::vector<std::thread> m_threads;
  // The caller keeps the current job alive until every worker has completed it.
  MipJob *m_job = nullptr;
  std::size_t m_generation = 0;
  std::size_t m_active = 0;
  std::size_t m_remaining = 0;
  bool m_stopping = false;
};

std::optional<Error> encode_payload(Span<Rgba8MipView> mips,
                                    const ExecutionOptions &execution,
                                    const bc7enc_compress_block_params &params,
                                    CompressedTexture &result) {
  ensure_bc7enc_initialized();
  std::optional<EncodeWorkers> workers;
  const auto baseBlocks = result.mips.front().sizeBytes / 16;
  auto participants = std::min<std::uint64_t>(
      execution.maxWorkers, baseBlocks / MIN_BLOCKS_PER_WORKER);
  if (participants > 1)
    participants = std::min<std::uint64_t>(
        participants, std::max(1U, std::thread::hardware_concurrency()));
  if (participants > 1 && !detail::cancelled(execution)) {
    try {
      workers.emplace(static_cast<std::size_t>(participants) - 1);
    } catch (const std::system_error &) {
      return Error{ErrorCode::BackendFailure,
                   "Could not create encoder worker.", 0};
    }
  }

  for (const auto &layout : result.mips) {
    if (detail::cancelled(execution))
      return Error{ErrorCode::Cancelled,
                   "Encoding cancelled before encoding mip level."};

    MipJob job{mips.data[layout.level],
               layout.blocksX,
               static_cast<std::size_t>(layout.sizeBytes / 16),
               result.payload.data() + layout.offsetBytes,
               params,
               execution};
    if (workers && job.blockCount >= 2 * MIN_BLOCKS_PER_WORKER)
      workers->run(job);
    else
      job.encode_range(0, job.blockCount);

    if (job.cancelled.load(std::memory_order_relaxed) ||
        detail::cancelled(execution)) {
      return Error{ErrorCode::Cancelled,
                   "Encoding cancelled during block processing.", layout.level};
    }
  }
  return std::nullopt;
}

} // namespace

Result<CompressedTexture> encode_bc7_mip(Rgba8MipView mip,
                                         const EncodeOptions &options) try {

  if (options.target != GpuFormat::BC7_UNorm &&
      options.target != GpuFormat::BC7_Srgb) {
    return Error{ErrorCode::UnsupportedFormat,
                 "Unsupported target format. Only BC7_UNorm and BC7_SRGB are "
                 "supported."};
  }

  const auto &limits = options.execution.limits;
  if (options.metadata.colorSpace != ColorSpace::Unknown &&
      options.metadata.colorSpace != detail::color_space(options.target)) {
    return Error{ErrorCode::InvalidArgument,
                 "Color space conflicts with target format."};
  }
  if (mip.width == 0 || mip.height == 0) {
    return Error{ErrorCode::InvalidArgument,
                 "Mip dimensions must be positive."};
  }
  if (limits.maxWidth > 0 && mip.width > limits.maxWidth) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Mip width exceeds maxWidth limit."};
  }
  if (limits.maxHeight > 0 && mip.height > limits.maxHeight) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Mip height exceeds maxHeight limit."};
  }

  std::uint64_t minRowPitch = static_cast<std::uint64_t>(mip.width) * 4;
  if (mip.rowPitchBytes < minRowPitch) {
    return Error{ErrorCode::InvalidArgument,
                 "Input rowPitchBytes is less than width * 4."};
  }

  std::uint64_t requiredInputBytes;
  if (!detail::multiply(mip.height - 1, mip.rowPitchBytes,
                        requiredInputBytes) ||
      !detail::add(requiredInputBytes, minRowPitch, requiredInputBytes)) {
    return Error{ErrorCode::IntegerOverflow, "Input pitch range overflows."};
  }
  if (mip.bytes.data == nullptr || mip.bytes.size < requiredInputBytes) {
    return Error{ErrorCode::TruncatedInput,
                 "Input byte buffer is smaller than required image dimensions "
                 "and pitch."};
  }
  if (limits.maxInputBytes > 0 && requiredInputBytes > limits.maxInputBytes) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Input bytes exceed maxInputBytes limit."};
  }

  if (detail::cancelled(options.execution)) {
    return Error{ErrorCode::Cancelled, "Encoding cancelled before starting."};
  }

  MipLayout layout;
  if (!make_layout(mip, 0, 0, layout)) {
    return Error{ErrorCode::IntegerOverflow, "BC7 size overflows."};
  }
  if (layout.sizeBytes > SIZE_MAX ||
      detail::exceeds(layout.sizeBytes, limits.maxOutputBytes) ||
      detail::exceeds(layout.sizeBytes, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Output BC7 payload size exceeds maxOutputBytes limit."};
  }

  bc7enc_compress_block_params params;
  if (!init_params(options, params))
    return Error{ErrorCode::UnsupportedOption, "Unknown quality preset."};

  CompressedTexture result;
  result.format = options.target;
  result.metadata = options.metadata;
  result.metadata.colorSpace = detail::color_space(options.target);
  result.mips.push_back(layout);
  result.payload.resize(static_cast<std::size_t>(layout.sizeBytes));
  if (auto error =
          encode_payload({&mip, 1}, options.execution, params, result)) {
    error->mipLevel = kMipLevelNotApplicable;
    return std::move(*error);
  }

  return result;
} catch (const std::bad_alloc &) {
  return Error{ErrorCode::AllocationFailure, "Encoding allocation failed."};
} catch (const std::length_error &) {
  return Error{ErrorCode::IntegerOverflow,
               "Encoding allocation size is not representable."};
} catch (const std::system_error &) {
  return Error{ErrorCode::BackendFailure, "Could not create encoder worker."};
}

Result<CompressedTexture> encode_bc7_chain(Span<Rgba8MipView> suppliedMips,
                                           const EncodeOptions &options) try {

  if (suppliedMips.data == nullptr || suppliedMips.size == 0) {
    return Error{ErrorCode::InvalidArgument, "Supplied mips span is empty."};
  }
  if (detail::cancelled(options.execution))
    return Error{ErrorCode::Cancelled,
                 "Mip chain cancelled before allocation."};
  if (options.target != GpuFormat::BC7_UNorm &&
      options.target != GpuFormat::BC7_Srgb) {
    return Error{ErrorCode::UnsupportedFormat,
                 "Only BC7 targets are supported."};
  }
  if (options.metadata.colorSpace != ColorSpace::Unknown &&
      options.metadata.colorSpace != detail::color_space(options.target)) {
    return Error{ErrorCode::InvalidArgument,
                 "Color space conflicts with target format."};
  }
  bc7enc_compress_block_params params;
  if (!init_params(options, params)) {
    return Error{ErrorCode::UnsupportedOption, "Unknown quality preset."};
  }

  const auto &limits = options.execution.limits;
  if (limits.maxMipCount > 0 && suppliedMips.size > limits.maxMipCount) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Supplied mips count exceeds maxMipCount limit."};
  }

  std::uint32_t baseWidth = suppliedMips.data[0].width;
  std::uint32_t baseHeight = suppliedMips.data[0].height;
  if (!baseWidth || !baseHeight ||
      suppliedMips.size > detail::mip_count(baseWidth, baseHeight)) {
    return Error{ErrorCode::InvalidMipChain,
                 "Mip chain extends beyond 1x1 or has zero dimensions."};
  }
  if (detail::exceeds(baseWidth, limits.maxWidth) ||
      detail::exceeds(baseHeight, limits.maxHeight)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Mip chain dimensions exceed limits."};
  }

  CompressedTexture result;
  result.format = options.target;
  result.metadata = options.metadata;
  result.metadata.colorSpace = detail::color_space(options.target);
  result.mips.reserve(suppliedMips.size);
  std::uint64_t inputBytes = 0, outputBytes = 0;
  for (std::size_t i = 0; i < suppliedMips.size; ++i) {
    std::uint32_t expectedW =
        calc_mip_dim(baseWidth, static_cast<std::uint32_t>(i));
    std::uint32_t expectedH =
        calc_mip_dim(baseHeight, static_cast<std::uint32_t>(i));

    if (suppliedMips.data[i].width != expectedW ||
        suppliedMips.data[i].height != expectedH) {
      return Error{ErrorCode::InvalidMipChain,
                   "Supplied mip chain has incorrect dimensions or gaps."};
    }
    const auto &mip = suppliedMips.data[i];
    const auto rowBytes = std::uint64_t(expectedW) * 4;
    std::uint64_t requiredBytes;
    if (mip.rowPitchBytes < rowBytes)
      return Error{ErrorCode::InvalidArgument, "Mip pitch is too small.",
                   static_cast<std::uint32_t>(i)};
    if (!detail::multiply(expectedH - 1, mip.rowPitchBytes, requiredBytes) ||
        !detail::add(requiredBytes, rowBytes, requiredBytes)) {
      return Error{ErrorCode::IntegerOverflow, "Mip pitch range overflows.",
                   static_cast<std::uint32_t>(i)};
    }
    if (!mip.bytes.data || mip.bytes.size < requiredBytes) {
      return Error{ErrorCode::TruncatedInput, "Mip input is truncated.",
                   static_cast<std::uint32_t>(i)};
    }
    MipLayout layout;
    if (!make_layout(mip, static_cast<std::uint32_t>(i), outputBytes, layout))
      return Error{ErrorCode::IntegerOverflow, "BC7 mip size overflows."};
    if (!detail::add(inputBytes, requiredBytes, inputBytes) ||
        !detail::add(outputBytes, layout.sizeBytes, outputBytes)) {
      return Error{ErrorCode::IntegerOverflow, "Combined mip size overflows."};
    }
    result.mips.push_back(layout);
  }
  if (outputBytes > SIZE_MAX ||
      detail::exceeds(inputBytes, limits.maxInputBytes) ||
      detail::exceeds(outputBytes, limits.maxOutputBytes) ||
      detail::exceeds(outputBytes, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Mip chain exceeds input, output or working limit."};
  }
  if (detail::cancelled(options.execution)) {
    return Error{ErrorCode::Cancelled,
                 "Encoding cancelled before encoding mip level."};
  }
  result.payload.resize(static_cast<std::size_t>(outputBytes));
  if (auto error =
          encode_payload(suppliedMips, options.execution, params, result))
    return std::move(*error);

  return result;
} catch (const std::bad_alloc &) {
  return Error{ErrorCode::AllocationFailure, "Mip chain allocation failed."};
} catch (const std::length_error &) {
  return Error{ErrorCode::IntegerOverflow,
               "Mip chain allocation size is not representable."};
}

} // namespace doku::texture
