#include "doku/texture/ktx2.hpp"
#include "doku/texture/transcode.hpp"

#include "basisu_transcoder.h"
#include "validation.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace doku::texture {

namespace {

static std::once_flag g_basisu_transcoder_init_flag;

void ensure_basisu_transcoder_initialized() {
  std::call_once(g_basisu_transcoder_init_flag,
                 []() { basist::basisu_transcoder_init(); });
}

inline std::uint32_t calc_blocks(std::uint32_t dim) {
  if (dim == 0)
    return 0;
  return detail::blocks(dim);
}

} // namespace

Result<CompressedTexture>
transcode_basis_ktx2(ByteSpan file, GpuFormat target,
                     const ExecutionOptions &execution) try {

  if (target != GpuFormat::BC7_UNorm && target != GpuFormat::BC7_Srgb) {
    return Error{ErrorCode::UnsupportedFormat,
                 "Unsupported target format. Only BC7_UNorm and BC7_Srgb are "
                 "supported in transcode_basis_ktx2."};
  }

  if (file.data == nullptr || file.size < 80) {
    return Error{ErrorCode::TruncatedInput,
                 "File size is smaller than KTX2 header."};
  }

  const auto &limits = execution.limits;
  if (limits.maxInputBytes > 0 && file.size > limits.maxInputBytes) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Input file size exceeds maxInputBytes limit."};
  }

  if (execution.cancelFlag &&
      execution.cancelFlag->load(std::memory_order_relaxed)) {
    return Error{ErrorCode::Cancelled,
                 "Transcoding cancelled before initialization."};
  }
  if (file.size > UINT32_MAX)
    return Error{ErrorCode::IntegerOverflow,
                 "Basis backend accepts at most UINT32_MAX input bytes."};
  auto infoResult = inspect_ktx2(file, execution);
  if (!infoResult)
    return infoResult.error();
  const auto &info = infoResult.value();
  if (!info.needsTranscode)
    return Error{ErrorCode::UnsupportedFormat,
                 "Input is not Basis ETC1S or UASTC LDR."};
  if (info.metadata.colorSpace != detail::color_space(target)) {
    return Error{ErrorCode::UnsupportedOption,
                 "Target color space must match source; no gamma conversion is "
                 "performed."};
  }
  // Bound declared inflated levels before initializing/decompressing the
  // backend.
  std::uint64_t inflatedBytes = 0;
  for (const auto &level : info.levelIndices) {
    if (!detail::add(inflatedBytes, level.uncompressedByteLength,
                     inflatedBytes)) {
      return Error{ErrorCode::IntegerOverflow,
                   "Inflated level sizes overflow."};
    }
  }
  std::uint64_t workingBytes;
  if (!detail::add(file.size, inflatedBytes, workingBytes) ||
      detail::exceeds(workingBytes, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Basis input and inflated levels exceed working limit."};
  }
  ensure_basisu_transcoder_initialized();

  basist::ktx2_transcoder transcoder;
  if (!transcoder.init(file.data, static_cast<std::uint32_t>(file.size))) {
    return Error{ErrorCode::InvalidContainer,
                 "Failed to initialize Basis KTX2 transcoder on input file."};
  }

  const auto channel = transcoder.get_dfd_channel_id0();
  if ((transcoder.is_etc1s() && (channel != basist::KTX2_DF_CHANNEL_ETC1S_RGB ||
                                 (transcoder.get_dfd_total_samples() == 2 &&
                                  transcoder.get_dfd_channel_id1() !=
                                      basist::KTX2_DF_CHANNEL_ETC1S_AAA))) ||
      (transcoder.is_uastc() && channel != basist::KTX2_DF_CHANNEL_UASTC_RGB &&
       channel != basist::KTX2_DF_CHANNEL_UASTC_RGBA)) {
    return Error{ErrorCode::UnsupportedMetadata,
                 "Only RGB/RGBA Basis channel mappings are supported."};
  }

  std::uint32_t width = transcoder.get_width();
  std::uint32_t height = transcoder.get_height();
  std::uint32_t levelCount = transcoder.get_levels();

  if (width == 0 || height == 0 || levelCount == 0) {
    return Error{ErrorCode::InvalidContainer,
                 "Invalid image dimensions or level count in Basis KTX2."};
  }

  if (limits.maxWidth > 0 && width > limits.maxWidth) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Texture width exceeds maxWidth limit."};
  }
  if (limits.maxHeight > 0 && height > limits.maxHeight) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Texture height exceeds maxHeight limit."};
  }
  if (limits.maxMipCount > 0 && levelCount > limits.maxMipCount) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Mip count exceeds maxMipCount limit."};
  }

  CompressedTexture result;
  result.format = target;
  result.metadata = info.metadata;
  result.metadata.alphaSemantic = transcoder.get_has_alpha()
                                      ? AlphaSemantic::Unknown
                                      : AlphaSemantic::Opaque;

  std::uint64_t totalPayloadBytes = 0;
  std::vector<MipLayout> layouts(levelCount);

  std::uint32_t currW = width;
  std::uint32_t currH = height;

  for (std::uint32_t l = 0; l < levelCount; ++l) {
    std::uint32_t bx = calc_blocks(currW);
    std::uint32_t by = calc_blocks(currH);
    std::uint64_t levelBytes;
    if (!detail::multiply(std::uint64_t(bx) * 16, by, levelBytes) ||
        std::uint64_t(bx) * by > UINT32_MAX) {
      return Error{ErrorCode::IntegerOverflow,
                   "Basis output block count overflows."};
    }

    layouts[l].level = l;
    layouts[l].width = currW;
    layouts[l].height = currH;
    layouts[l].blocksX = bx;
    layouts[l].blocksY = by;
    layouts[l].offsetBytes = totalPayloadBytes;
    layouts[l].sizeBytes = levelBytes;
    layouts[l].rowPitchBytes = static_cast<std::uint64_t>(bx) * 16;

    if (!detail::add(totalPayloadBytes, levelBytes, totalPayloadBytes)) {
      return Error{ErrorCode::IntegerOverflow, "Basis output size overflows."};
    }

    currW = std::max(1U, currW / 2);
    currH = std::max(1U, currH / 2);
  }

  if (!detail::add(workingBytes, totalPayloadBytes, workingBytes) ||
      totalPayloadBytes > SIZE_MAX ||
      detail::exceeds(totalPayloadBytes, limits.maxOutputBytes) ||
      detail::exceeds(workingBytes, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Transcoded payload size exceeds maxOutputBytes limit."};
  }
  if (detail::cancelled(execution))
    return Error{ErrorCode::Cancelled,
                 "Transcoding cancelled before codebook expansion."};
  if (!transcoder.start_transcoding()) {
    return Error{ErrorCode::BackendFailure,
                 "Basis transcoder start_transcoding() failed."};
  }

  result.payload.resize(totalPayloadBytes);
  result.mips = layouts;

  for (std::uint32_t l = 0; l < levelCount; ++l) {
    if (execution.cancelFlag &&
        execution.cancelFlag->load(std::memory_order_relaxed)) {
      return Error{ErrorCode::Cancelled,
                   "Transcoding cancelled during level transcode."};
    }

    const auto &layout = result.mips[l];
    std::uint32_t totalBlocks = layout.blocksX * layout.blocksY;
    void *dstPtr = result.payload.data() + layout.offsetBytes;

    bool status = transcoder.transcode_image_level(
        l, 0, 0, dstPtr, totalBlocks,
        basist::transcoder_texture_format::cTFBC7_RGBA);

    if (!status) {
      return Error{ErrorCode::BackendFailure,
                   "Basis transcode_image_level failed on mip level."};
    }
  }

  if (detail::cancelled(execution))
    return Error{ErrorCode::Cancelled, "Transcoding cancelled."};
  return result;
} catch (const std::bad_alloc &) {
  return Error{ErrorCode::AllocationFailure, "Basis allocation failed."};
} catch (const std::length_error &) {
  return Error{ErrorCode::IntegerOverflow, "Basis allocation is too large."};
}

} // namespace doku::texture
