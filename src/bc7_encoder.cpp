#include "DokuTexture/Encode.hpp"
#include "bc7enc.h"
#include "validation.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace doku::texture {

namespace {

static std::once_flag g_bc7enc_init_flag;

void ensure_bc7enc_initialized() {
  std::call_once(g_bc7enc_init_flag, []() { bc7enc_compress_block_init(); });
}

inline std::uint32_t calc_blocks(std::uint32_t dim) {
  if (dim == 0)
    return 0;
  return detail::blocks(dim);
}

inline std::uint32_t calc_mip_dim(std::uint32_t baseDim, std::uint32_t level) {
  std::uint32_t d = baseDim >> level;
  return d > 0 ? d : 1;
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

  if (options.execution.cancelFlag &&
      options.execution.cancelFlag->load(std::memory_order_relaxed)) {
    return Error{ErrorCode::Cancelled, "Encoding cancelled before starting."};
  }
  ensure_bc7enc_initialized();

  std::uint32_t blocksX = calc_blocks(mip.width);
  std::uint32_t blocksY = calc_blocks(mip.height);
  std::uint64_t rowPitchBytes = static_cast<std::uint64_t>(blocksX) * 16;
  std::uint64_t totalSizeBytes;
  if (!detail::multiply(rowPitchBytes, blocksY, totalSizeBytes)) {
    return Error{ErrorCode::IntegerOverflow, "BC7 size overflows."};
  }

  if (totalSizeBytes > SIZE_MAX ||
      detail::exceeds(totalSizeBytes, limits.maxOutputBytes) ||
      detail::exceeds(totalSizeBytes, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Output BC7 payload size exceeds maxOutputBytes limit."};
  }

  bc7enc_compress_block_params compParams;
  bc7enc_compress_block_params_init(&compParams);

  if (options.target == GpuFormat::BC7_UNorm) {
    bc7enc_compress_block_params_init_linear_weights(&compParams);
  } else {
    bc7enc_compress_block_params_init_perceptual_weights(&compParams);
  }

  switch (options.quality) {
    case Quality::Fast:
      compParams.m_uber_level = 0;
      compParams.m_max_partitions_mode = 16;
      compParams.m_try_least_squares = BC7ENC_FALSE;
      break;
    case Quality::Balanced:
      compParams.m_uber_level = 1;
      compParams.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
      compParams.m_try_least_squares = BC7ENC_TRUE;
      break;
    case Quality::High:
      compParams.m_uber_level = 2;
      compParams.m_max_partitions_mode = BC7ENC_MAX_PARTITIONS1;
      compParams.m_try_least_squares = BC7ENC_TRUE;
      break;
    default:
      return Error{ErrorCode::UnsupportedOption, "Unknown quality preset."};
  }

  OwningBuffer payload(totalSizeBytes);
  std::atomic<bool> cancelled{false};

  std::uint32_t numWorkers =
      std::min(blocksY, std::max(1U, options.execution.maxWorkers));

  auto encode_row_range = [&](std::uint32_t startBY, std::uint32_t endBY) {
    for (std::uint32_t by = startBY; by < endBY; ++by) {
      if (options.execution.cancelFlag &&
          options.execution.cancelFlag->load(std::memory_order_relaxed)) {
        cancelled.store(true, std::memory_order_relaxed);
        return;
      }
      if (cancelled.load(std::memory_order_relaxed)) {
        return;
      }

      for (std::uint32_t bx = 0; bx < blocksX; ++bx) {
        if (detail::cancelled(options.execution)) {
          cancelled.store(true, std::memory_order_relaxed);
          return;
        }
        alignas(16) std::uint8_t blockPixels[64];

        for (std::uint32_t py = 0; py < 4; ++py) {
          std::uint32_t iy = std::min(by * 4 + py, mip.height - 1);
          const std::byte *srcRow =
              mip.bytes.data + static_cast<std::size_t>(iy * mip.rowPitchBytes);

          for (std::uint32_t px = 0; px < 4; ++px) {
            std::uint32_t ix = std::min(bx * 4 + px, mip.width - 1);
            const std::byte *srcPixel =
                srcRow + static_cast<std::size_t>(ix) * 4;

            std::size_t dstOffset = (py * 4 + px) * 4;
            std::memcpy(&blockPixels[dstOffset], srcPixel, 4);
          }
        }

        std::size_t dstOffset = static_cast<std::size_t>(
            by * rowPitchBytes + std::uint64_t(bx) * 16);
        bc7enc_compress_block(payload.data() + dstOffset, blockPixels,
                              &compParams);
      }
    }
  };

  if (numWorkers == 1 || blocksY <= 1) {
    encode_row_range(0, blocksY);
  } else {
    std::vector<std::thread> threads;
    threads.reserve(numWorkers);
    // Join already-started workers even if a later thread constructor throws.
    struct JoinThreads {
      std::vector<std::thread> &threads;
      ~JoinThreads() {
        for (auto &t : threads)
          if (t.joinable())
            t.join();
      }
    } join{threads};
    std::uint32_t rowsPerThread =
        blocksY / numWorkers + (blocksY % numWorkers != 0);

    for (std::uint32_t t = 0; t < numWorkers; ++t) {
      std::uint32_t startY = t * rowsPerThread;
      std::uint32_t endY = std::min(startY + rowsPerThread, blocksY);
      if (startY < endY) {
        threads.emplace_back(encode_row_range, startY, endY);
      }
    }

    for (auto &th : threads) {
      if (th.joinable()) {
        th.join();
      }
    }
  }

  if (cancelled.load(std::memory_order_relaxed) ||
      (options.execution.cancelFlag &&
       options.execution.cancelFlag->load(std::memory_order_relaxed))) {
    return Error{ErrorCode::Cancelled,
                 "Encoding cancelled during block processing."};
  }

  CompressedTexture result;
  result.format = options.target;
  result.metadata = options.metadata;
  result.metadata.colorSpace = detail::color_space(options.target);
  result.payload = std::move(payload);

  MipLayout layout;
  layout.level = 0;
  layout.width = mip.width;
  layout.height = mip.height;
  layout.blocksX = blocksX;
  layout.blocksY = blocksY;
  layout.offsetBytes = 0;
  layout.sizeBytes = totalSizeBytes;
  layout.rowPitchBytes = rowPitchBytes;

  result.mips.push_back(layout);

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
  if (options.quality != Quality::Fast &&
      options.quality != Quality::Balanced &&
      options.quality != Quality::High) {
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

  std::uint64_t inputBytes = 0, outputBytes = 0, largestMip = 0;
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
    std::uint64_t size;
    if (!detail::multiply(static_cast<std::uint64_t>(calc_blocks(expectedW)) *
                              16,
                          calc_blocks(expectedH), size)) {
      return Error{ErrorCode::IntegerOverflow, "BC7 mip size overflows."};
    }
    largestMip = std::max(largestMip, size);
    if (!detail::add(inputBytes, requiredBytes, inputBytes) ||
        !detail::add(outputBytes, size, outputBytes)) {
      return Error{ErrorCode::IntegerOverflow, "Combined mip size overflows."};
    }
  }
  std::uint64_t workingBytes;
  if (!detail::add(outputBytes, largestMip, workingBytes) ||
      outputBytes > SIZE_MAX ||
      detail::exceeds(inputBytes, limits.maxInputBytes) ||
      detail::exceeds(outputBytes, limits.maxOutputBytes) ||
      detail::exceeds(workingBytes, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Mip chain exceeds input, output or working limit."};
  }

  CompressedTexture result;
  result.format = options.target;
  result.metadata = options.metadata;
  result.metadata.colorSpace = detail::color_space(options.target);
  result.payload.reserve(static_cast<std::size_t>(outputBytes));

  std::uint64_t currentOffset = 0;

  for (std::size_t i = 0; i < suppliedMips.size; ++i) {
    if (options.execution.cancelFlag &&
        options.execution.cancelFlag->load(std::memory_order_relaxed)) {
      return Error{ErrorCode::Cancelled,
                   "Encoding cancelled before encoding mip level."};
    }

    auto mipRes = encode_bc7_mip(suppliedMips.data[i], options);
    if (!mipRes) {
      Error err = mipRes.error();
      err.mipLevel = static_cast<std::uint32_t>(i);
      return err;
    }

    CompressedTexture singleMipTex = std::move(mipRes.value());

    MipLayout layout = singleMipTex.mips[0];
    layout.level = static_cast<std::uint32_t>(i);
    layout.offsetBytes = currentOffset;

    result.mips.push_back(layout);
    result.payload.insert(result.payload.end(), singleMipTex.payload.begin(),
                          singleMipTex.payload.end());

    currentOffset += layout.sizeBytes;

    if (limits.maxOutputBytes > 0 &&
        result.payload.size() > limits.maxOutputBytes) {
      return Error{ErrorCode::ResourceLimitExceeded,
                   "Combined BC7 payload size exceeds maxOutputBytes limit."};
    }
  }

  return result;
} catch (const std::bad_alloc &) {
  return Error{ErrorCode::AllocationFailure, "Mip chain allocation failed."};
} catch (const std::length_error &) {
  return Error{ErrorCode::IntegerOverflow,
               "Mip chain allocation size is not representable."};
}

} // namespace doku::texture
