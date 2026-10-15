#include "doku/texture/ktx2.hpp"
#include "validation.hpp"

#include <algorithm>
#include <cstring>
#include <iostream>

namespace doku::texture {

namespace {

static constexpr std::uint8_t kKtx2Identifier[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

constexpr std::uint32_t VK_FORMAT_UNDEFINED = 0;
constexpr std::uint32_t VK_FORMAT_BC7_UNORM_BLOCK = 145;
constexpr std::uint32_t VK_FORMAT_BC7_SRGB_BLOCK = 146;

constexpr std::size_t kKtx2HeaderSize = 80; // 12 identifier + 68 header fields

inline std::uint32_t read_u32_le(const std::byte *ptr) {
  std::uint32_t v;
  std::memcpy(&v, ptr, sizeof(v));
  return v;
}

inline std::uint64_t read_u64_le(const std::byte *ptr) {
  std::uint64_t v;
  std::memcpy(&v, ptr, sizeof(v));
  return v;
}

inline void write_u32_le(std::byte *ptr, std::uint32_t val) {
  std::memcpy(ptr, &val, sizeof(val));
}

inline void write_u64_le(std::byte *ptr, std::uint64_t val) {
  std::memcpy(ptr, &val, sizeof(val));
}

inline std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) {
  if (alignment == 0)
    return value;
  std::uint64_t rem = value % alignment;
  return rem == 0 ? value : (value + alignment - rem);
}

inline std::uint32_t calc_blocks(std::uint32_t dim) {
  if (dim == 0)
    return 0;
  return detail::blocks(dim);
}

// Generate basic 44-byte Data Format Descriptor (DFD) for BC7
std::vector<std::byte> create_bc7_dfd(GpuFormat format) {
  std::vector<std::byte> dfd(44, std::byte{0});

  // Total size of DFD block
  write_u32_le(&dfd[0], 44);

  // Vendor ID (0 = Khronos), Descriptor Type (0 =
  // KHR_DF_DESCRIPTOR_TYPE_BASICFORMAT)
  std::uint16_t vendorId = 0;
  std::uint16_t descriptorType = 0;
  std::memcpy(&dfd[4], &vendorId, 2);
  std::memcpy(&dfd[6], &descriptorType, 2);

  // Version number (2 = KHR_DF_VERSION_1_3)
  std::uint16_t versionNumber = 2;
  std::uint16_t descriptorBlockSize = 40;
  std::memcpy(&dfd[8], &versionNumber, 2);
  std::memcpy(&dfd[10], &descriptorBlockSize, 2);

  // KHR_DF_MODEL_BC7 (163 denotes ETC1S).
  dfd[12] = static_cast<std::byte>(134);

  // Primaries: 1 = KHR_DF_PRIMARIES_BT709
  dfd[13] = static_cast<std::byte>(1);

  // KHR_DF_TRANSFER_LINEAR = 1, KHR_DF_TRANSFER_SRGB = 2.
  if (format == GpuFormat::BC7_Srgb) {
    dfd[14] = static_cast<std::byte>(2);
  } else {
    dfd[14] = static_cast<std::byte>(1);
  }

  // Flags: 0
  dfd[15] = static_cast<std::byte>(0);

  // Texel block dimensions - 1: [3, 3, 0, 0] (for 4x4x1 block)
  dfd[16] = static_cast<std::byte>(3); // 4 - 1
  dfd[17] = static_cast<std::byte>(3); // 4 - 1
  dfd[18] = static_cast<std::byte>(0);
  dfd[19] = static_cast<std::byte>(0);

  dfd[20] = static_cast<std::byte>(16);      // bytesPlane0, not bytes minus one
  dfd[30] = static_cast<std::byte>(127);     // sample bitLength minus one
  write_u32_le(dfd.data() + 40, UINT32_MAX); // sampleUpper

  return dfd;
}

} // namespace

Result<Ktx2Info> inspect_ktx2(ByteSpan file,
                              const ExecutionOptions &execution) try {

  if (file.data == nullptr || file.size < kKtx2HeaderSize) {
    return Error{ErrorCode::TruncatedInput,
                 "File size is smaller than KTX2 header."};
  }

  const auto &limits = execution.limits;
  if (detail::cancelled(execution))
    return Error{ErrorCode::Cancelled, "Inspection cancelled."};
  if (limits.maxInputBytes > 0 && file.size > limits.maxInputBytes) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Input file size exceeds maxInputBytes limit."};
  }

  if (std::memcmp(file.data, kKtx2Identifier, 12) != 0) {
    return Error{ErrorCode::InvalidContainer, "Invalid KTX2 magic identifier."};
  }

  const std::byte *hdrPtr = file.data + 12;
  std::uint32_t vkFormat = read_u32_le(hdrPtr + 0);
  std::uint32_t pixelWidth = read_u32_le(hdrPtr + 8);
  std::uint32_t pixelHeight = read_u32_le(hdrPtr + 12);
  std::uint32_t layerCount = read_u32_le(hdrPtr + 20);
  std::uint32_t faceCount = read_u32_le(hdrPtr + 24);
  std::uint32_t levelCount = read_u32_le(hdrPtr + 28);
  std::uint32_t supercompressionScheme = read_u32_le(hdrPtr + 32);
  const auto depth = read_u32_le(hdrPtr + 16);
  const auto dfdOffset = read_u32_le(hdrPtr + 36),
             dfdLength = read_u32_le(hdrPtr + 40);
  const auto kvdOffset = read_u32_le(hdrPtr + 44),
             kvdLength = read_u32_le(hdrPtr + 48);
  const auto sgdOffset = read_u64_le(hdrPtr + 52),
             sgdLength = read_u64_le(hdrPtr + 60);

  if (pixelWidth == 0 || pixelHeight == 0 || levelCount == 0) {
    return Error{ErrorCode::InvalidContainer,
                 "Invalid image dimensions or level count in KTX2 header."};
  }
  if (depth || layerCount || faceCount != 1) {
    return Error{ErrorCode::UnsupportedTextureType,
                 "Only non-array 2D single-face textures are supported."};
  }
  if (levelCount > detail::mip_count(pixelWidth, pixelHeight)) {
    return Error{ErrorCode::InvalidContainer,
                 "Mip count exceeds dimension pyramid."};
  }
  if (read_u32_le(hdrPtr + 4) != 1)
    return Error{ErrorCode::InvalidContainer, "Invalid typeSize."};

  if (limits.maxWidth > 0 && pixelWidth > limits.maxWidth) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Texture width exceeds maxWidth limit."};
  }
  if (limits.maxHeight > 0 && pixelHeight > limits.maxHeight) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Texture height exceeds maxHeight limit."};
  }
  if (limits.maxMipCount > 0 && levelCount > limits.maxMipCount) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Level count exceeds maxMipCount limit."};
  }

  std::uint64_t levelIndexTableSize =
      static_cast<std::uint64_t>(levelCount) * 24;
  if (kKtx2HeaderSize + levelIndexTableSize > file.size) {
    return Error{ErrorCode::TruncatedInput,
                 "File is truncated before complete level index table."};
  }
  if (detail::exceeds(levelIndexTableSize + std::uint64_t(kvdLength) * 2,
                      limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Inspection metadata exceeds working limit."};
  }
  auto inFile = [&](std::uint64_t off, std::uint64_t len) {
    return off <= file.size && len <= file.size - off;
  };
  const auto indexEnd = kKtx2HeaderSize + levelIndexTableSize;
  if (!inFile(dfdOffset, dfdLength) || !inFile(kvdOffset, kvdLength) ||
      !inFile(sgdOffset, sgdLength)) {
    return Error{ErrorCode::TruncatedInput, "Metadata range exceeds file."};
  }
  if (dfdLength < 44 || dfdOffset < indexEnd || dfdOffset % 4 ||
      (kvdLength && (kvdOffset < std::uint64_t(dfdOffset) + dfdLength ||
                     kvdOffset % 4 || kvdLength % 4)) ||
      (sgdLength &&
       (sgdOffset < std::max(std::uint64_t(dfdOffset) + dfdLength,
                             std::uint64_t(kvdOffset) + kvdLength) ||
        sgdOffset % 8))) {
    return Error{ErrorCode::InvalidContainer,
                 "Invalid or overlapping metadata layout."};
  }
  const auto *dfd = file.data + dfdOffset;
  const auto descriptorSize = std::to_integer<unsigned>(dfd[10]) |
                              (std::to_integer<unsigned>(dfd[11]) << 8);
  if (read_u32_le(dfd) != dfdLength || read_u32_le(dfd + 4) != 0 ||
      descriptorSize < 40 || (descriptorSize - 24) % 16 ||
      descriptorSize + 4 > dfdLength || dfd[8] != std::byte{2} ||
      dfd[9] != std::byte{0}) {
    return Error{ErrorCode::InvalidContainer, "Invalid basic DFD."};
  }
  const auto model = std::to_integer<unsigned>(dfd[12]);
  const auto transfer = std::to_integer<unsigned>(dfd[14]);
  if (transfer != 1 && transfer != 2)
    return Error{ErrorCode::UnsupportedMetadata,
                 "Unsupported transfer function."};
  if ((model == 163 || model == 166) && dfd[13] != std::byte{1}) {
    return Error{ErrorCode::UnsupportedMetadata,
                 "Basis color primaries must be BT.709; no primaries "
                 "conversion is performed."};
  }

  Ktx2Info info;
  info.vkFormat = vkFormat;
  info.width = pixelWidth;
  info.height = pixelHeight;
  info.layerCount = layerCount;
  info.faceCount = faceCount;
  info.levelCount = levelCount;
  info.supercompressionScheme = supercompressionScheme;

  if (vkFormat == VK_FORMAT_BC7_UNORM_BLOCK) {
    info.sourceEncoding = SourceEncoding::NativeBC7;
    info.targetFormat = GpuFormat::BC7_UNorm;
    info.needsTranscode = false;
    info.metadata.colorSpace = ColorSpace::Linear;
  } else if (vkFormat == VK_FORMAT_BC7_SRGB_BLOCK) {
    info.sourceEncoding = SourceEncoding::NativeBC7;
    info.targetFormat = GpuFormat::BC7_Srgb;
    info.needsTranscode = false;
    info.metadata.colorSpace = ColorSpace::SRGB;
  } else if (vkFormat == VK_FORMAT_UNDEFINED) {
    info.needsTranscode = true;
    if (model == 163 && supercompressionScheme == 1) {
      if (sgdLength < 20 + std::uint64_t(levelCount) * 20) {
        return Error{ErrorCode::InvalidContainer,
                     "BasisLZ global data or image descriptors are missing."};
      }
      info.sourceEncoding = SourceEncoding::BasisETC1S;
    } else if (model == 166 &&
               (supercompressionScheme == 0 || supercompressionScheme == 2)) {
      info.sourceEncoding = SourceEncoding::BasisUASTC;
    } else {
      return Error{ErrorCode::UnsupportedFormat,
                   "Unsupported universal DFD model or supercompression."};
    }
  } else {
    info.sourceEncoding = SourceEncoding::Other;
    return Error{ErrorCode::UnsupportedFormat,
                 "Unsupported vkFormat in KTX2 header."};
  }
  info.metadata.colorSpace =
      transfer == 2 ? ColorSpace::SRGB : ColorSpace::Linear;
  info.metadata.alphaStorage = (std::to_integer<unsigned>(dfd[15]) & 1)
                                   ? AlphaStorage::Premultiplied
                                   : AlphaStorage::Straight;
  if (info.sourceEncoding == SourceEncoding::NativeBC7) {
    if (dfdLength != 44 || dfd[13] != std::byte{1}) {
      return Error{ErrorCode::UnsupportedMetadata,
                   "Native reader supports a single BT.709 BC7 basic DFD."};
    }
    auto expected = create_bc7_dfd(info.targetFormat);
    expected[15] = dfd[15];
    if (std::memcmp(dfd, expected.data(), expected.size()) != 0 ||
        (std::to_integer<unsigned>(dfd[15]) & ~1U)) {
      return Error{ErrorCode::InvalidContainer,
                   "BC7 header/DFD mismatch or unsupported BC7 descriptor."};
    }
    if (sgdLength)
      return Error{ErrorCode::InvalidContainer,
                   "Native BC7 cannot have global data."};
  }
  // Parse only standard metadata we expose; reject unsupported values rather
  // than relabeling pixels.
  for (std::uint64_t p = kvdOffset, end = std::uint64_t(kvdOffset) + kvdLength;
       p < end;) {
    if (end - p < 4)
      return Error{ErrorCode::InvalidContainer, "Truncated KVD entry."};
    const auto n = read_u32_le(file.data + p);
    p += 4;
    if (!n || n > end - p)
      return Error{ErrorCode::InvalidContainer, "Invalid KVD entry length."};
    const auto *begin = reinterpret_cast<const char *>(file.data + p);
    const auto *zero = static_cast<const char *>(std::memchr(begin, 0, n));
    if (!zero)
      return Error{ErrorCode::InvalidContainer, "KVD key is not terminated."};
    std::string key(begin, zero);
    std::string value(zero + 1, begin + n);
    if (!value.empty() && value.back() == '\0')
      value.pop_back();
    if (key == "KTXorientation") {
      if (begin[n - 1] != '\0')
        return Error{ErrorCode::InvalidContainer,
                     "Orientation must be NUL-terminated."};
      if (value == "rd")
        info.metadata.orientation = Orientation::TopLeft;
      else if (value == "ru")
        info.metadata.orientation = Orientation::BottomLeft;
      else
        return Error{ErrorCode::UnsupportedMetadata,
                     "Unsupported orientation."};
    } else if (key == "KTXswizzle") {
      if (value.size() != 4)
        return Error{ErrorCode::InvalidContainer, "Invalid swizzle."};
      const char *channels = "rgba01";
      for (unsigned c = 0; c < 4; ++c) {
        const char *pos = std::strchr(channels, value[c]);
        if (!value[c] || !pos)
          return Error{ErrorCode::InvalidContainer, "Invalid swizzle channel."};
        info.metadata.swizzle[c] = static_cast<Channel>(pos - channels);
      }
    }
    p = align_up(p + n, 4);
    if (p > end)
      return Error{ErrorCode::InvalidContainer, "Missing KVD padding."};
  }

  const std::byte *lvlIndexPtr = file.data + kKtx2HeaderSize;
  info.levelIndices.reserve(levelCount);
  std::uint64_t metadataEnd =
      std::max({indexEnd, std::uint64_t(dfdOffset) + dfdLength,
                std::uint64_t(kvdOffset) + kvdLength, sgdOffset + sgdLength});
  std::uint64_t previousOffset = file.size;

  for (std::uint32_t i = 0; i < levelCount; ++i) {
    Ktx2LevelIndex lvlIdx;
    lvlIdx.byteOffset = read_u64_le(lvlIndexPtr + std::size_t(i) * 24 + 0);
    lvlIdx.byteLength = read_u64_le(lvlIndexPtr + std::size_t(i) * 24 + 8);
    lvlIdx.uncompressedByteLength =
        read_u64_le(lvlIndexPtr + std::size_t(i) * 24 + 16);

    if (lvlIdx.byteOffset > file.size ||
        lvlIdx.byteLength > file.size - lvlIdx.byteOffset) {
      return Error{ErrorCode::TruncatedInput,
                   "Level index offset/length exceeds file boundaries."};
    }
    if (!lvlIdx.byteLength || lvlIdx.byteOffset < metadataEnd ||
        lvlIdx.byteOffset + lvlIdx.byteLength > previousOffset) {
      return Error{ErrorCode::InvalidContainer,
                   "Overlapping or unordered level payloads."};
    }
    previousOffset = lvlIdx.byteOffset;
    if (info.sourceEncoding == SourceEncoding::NativeBC7 &&
        !supercompressionScheme) {
      std::uint64_t size;
      if (!detail::multiply(
              std::uint64_t(calc_blocks(std::max(1U, pixelWidth >> i))) * 16,
              calc_blocks(std::max(1U, pixelHeight >> i)), size)) {
        return Error{ErrorCode::IntegerOverflow, "BC7 level size overflows."};
      }
      if (lvlIdx.byteOffset % 16 || lvlIdx.byteLength != size ||
          lvlIdx.uncompressedByteLength != size) {
        return Error{ErrorCode::InvalidContainer,
                     "Invalid BC7 level alignment or length."};
      }
    }

    info.levelIndices.push_back(lvlIdx);
  }

  return info;
} catch (const std::bad_alloc &) {
  return Error{ErrorCode::AllocationFailure, "Inspection allocation failed."};
} catch (const std::length_error &) {
  return Error{ErrorCode::IntegerOverflow,
               "Inspection allocation is too large."};
}

Result<OwningBuffer>
write_ktx2_native_bc7(const CompressedTextureView &texture,
                      const ExecutionOptions &execution) try {

  if (texture.format != GpuFormat::BC7_UNorm &&
      texture.format != GpuFormat::BC7_Srgb) {
    return Error{ErrorCode::UnsupportedFormat,
                 "Only BC7_UNorm and BC7_Srgb formats are supported for "
                 "write_ktx2_native_bc7."};
  }

  if (texture.mips.data == nullptr || texture.mips.size == 0) {
    return Error{ErrorCode::InvalidArgument,
                 "CompressedTextureView has no mips."};
  }

  const auto &limits = execution.limits;
  if (detail::cancelled(execution))
    return Error{ErrorCode::Cancelled, "Writing cancelled."};
  if (texture.mips.size > UINT32_MAX)
    return Error{ErrorCode::IntegerOverflow, "Mip count overflows."};
  std::uint32_t levelCount = static_cast<std::uint32_t>(texture.mips.size);
  if (limits.maxMipCount > 0 && levelCount > limits.maxMipCount) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Mip count exceeds maxMipCount limit."};
  }

  std::uint32_t baseWidth = texture.mips.data[0].layout.width;
  std::uint32_t baseHeight = texture.mips.data[0].layout.height;
  if (!baseWidth || !baseHeight ||
      levelCount > detail::mip_count(baseWidth, baseHeight)) {
    return Error{ErrorCode::InvalidMipChain,
                 "Invalid base dimensions or too many mips."};
  }
  if (detail::exceeds(baseWidth, limits.maxWidth) ||
      detail::exceeds(baseHeight, limits.maxHeight)) {
    return Error{ErrorCode::ResourceLimitExceeded, "Dimensions exceed limits."};
  }
  if ((texture.metadata.colorSpace != ColorSpace::Unknown &&
       texture.metadata.colorSpace != detail::color_space(texture.format)) ||
      (texture.metadata.orientation != Orientation::TopLeft &&
       texture.metadata.orientation != Orientation::BottomLeft) ||
      (texture.metadata.alphaStorage != AlphaStorage::Unknown &&
       texture.metadata.alphaStorage != AlphaStorage::Straight &&
       texture.metadata.alphaStorage != AlphaStorage::Premultiplied) ||
      texture.metadata.swizzle != std::array<Channel, 4>{Channel::R, Channel::G,
                                                         Channel::B,
                                                         Channel::A} ||
      texture.metadata.alphaSemantic != AlphaSemantic::Unknown) {
    return Error{ErrorCode::UnsupportedMetadata,
                 "Writer supports matching color space, identity swizzle, "
                 "unknown alpha semantic and rd/ru orientation."};
  }

  std::vector<std::byte> dfd = create_bc7_dfd(texture.format);
  dfd[15] = texture.metadata.alphaStorage == AlphaStorage::Premultiplied
                ? std::byte{1}
                : std::byte{0};

  // KTX2 orientation is the NUL-terminated string "rd" or "ru".
  char kvdData[] = "KTXorientation\0rd";
  if (texture.metadata.orientation == Orientation::BottomLeft)
    kvdData[16] = 'u';
  std::uint32_t kvPairLen = sizeof(kvdData);
  std::vector<std::byte> kvd(4 + kvPairLen);
  write_u32_le(kvd.data(), kvPairLen);
  std::memcpy(kvd.data() + 4, kvdData, kvPairLen);
  kvd.resize(static_cast<std::size_t>(align_up(kvd.size(), 4)), std::byte{0});
  const char writer[] = "KTXwriter\0DokuTexture 1.0.0";
  const auto writerStart = kvd.size();
  kvd.resize(
      static_cast<std::size_t>(align_up(writerStart + 4 + sizeof(writer), 4)),
      std::byte{0});
  write_u32_le(kvd.data() + writerStart, sizeof(writer));
  std::memcpy(kvd.data() + writerStart + 4, writer, sizeof(writer));

  std::uint64_t headerAndIndexSize =
      kKtx2HeaderSize + static_cast<std::uint64_t>(levelCount) * 24;
  std::uint64_t dfdOffset = align_up(headerAndIndexSize, 4);
  std::uint64_t kvdOffset = align_up(dfdOffset + dfd.size(), 4);
  std::uint64_t payloadStartOffset = align_up(kvdOffset + kvd.size(), 16);

  std::uint64_t currentPayloadOffset = payloadStartOffset;
  std::vector<Ktx2LevelIndex> levelIndices(levelCount);

  std::uint64_t inputBytes = 0;
  for (std::uint32_t j = levelCount; j > 0; --j) {
    const auto i = j - 1;
    const auto &mipView = texture.mips.data[i];
    std::uint64_t size = mipView.bytes.size;
    const auto w = std::max(1U, baseWidth >> i),
               h = std::max(1U, baseHeight >> i);
    std::uint64_t expectedSize;
    if (!detail::multiply(std::uint64_t(calc_blocks(w)) * 16, calc_blocks(h),
                          expectedSize)) {
      return Error{ErrorCode::IntegerOverflow, "BC7 mip size overflows."};
    }
    if (!mipView.bytes.data || mipView.layout.level != i ||
        mipView.layout.width != w || mipView.layout.height != h ||
        size != expectedSize || mipView.layout.sizeBytes != size ||
        mipView.layout.blocksX != calc_blocks(w) ||
        mipView.layout.blocksY != calc_blocks(h) ||
        mipView.layout.rowPitchBytes != std::uint64_t(calc_blocks(w)) * 16) {
      return Error{ErrorCode::InvalidMipChain,
                   "Invalid BC7 mip view or payload size."};
    }
    if (!detail::add(inputBytes, size, inputBytes))
      return Error{ErrorCode::IntegerOverflow, "Input size overflows."};

    levelIndices[i].byteOffset = currentPayloadOffset;
    levelIndices[i].byteLength = size;
    levelIndices[i].uncompressedByteLength = size;

    if (!detail::add(currentPayloadOffset, size, currentPayloadOffset)) {
      return Error{ErrorCode::IntegerOverflow, "File size overflows."};
    }
  }

  std::uint64_t totalFileSize = currentPayloadOffset;
  if (totalFileSize > SIZE_MAX ||
      detail::exceeds(totalFileSize, limits.maxOutputBytes) ||
      detail::exceeds(inputBytes, limits.maxInputBytes) ||
      detail::exceeds(totalFileSize, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Target KTX2 file size exceeds maxOutputBytes limit."};
  }

  OwningBuffer buffer(totalFileSize, std::byte{0});

  // Write identifier
  std::memcpy(buffer.data(), kKtx2Identifier, 12);

  // Write header fields
  std::byte *hdrPtr = buffer.data() + 12;
  write_u32_le(hdrPtr + 0, texture.format == GpuFormat::BC7_Srgb
                               ? VK_FORMAT_BC7_SRGB_BLOCK
                               : VK_FORMAT_BC7_UNORM_BLOCK);
  write_u32_le(hdrPtr + 4, 1); // typeSize
  write_u32_le(hdrPtr + 8, baseWidth);
  write_u32_le(hdrPtr + 12, baseHeight);
  write_u32_le(hdrPtr + 16, 0); // pixelDepth
  write_u32_le(hdrPtr + 20, 0); // layerCount
  write_u32_le(hdrPtr + 24, 1); // faceCount
  write_u32_le(hdrPtr + 28, levelCount);
  write_u32_le(hdrPtr + 32, 0); // supercompressionScheme = NONE

  write_u32_le(hdrPtr + 36, static_cast<std::uint32_t>(dfdOffset));
  write_u32_le(hdrPtr + 40, static_cast<std::uint32_t>(dfd.size()));

  write_u32_le(hdrPtr + 44, static_cast<std::uint32_t>(kvdOffset));
  write_u32_le(hdrPtr + 48, static_cast<std::uint32_t>(kvd.size()));

  write_u64_le(hdrPtr + 52, 0); // sgdByteOffset
  write_u64_le(hdrPtr + 60, 0); // sgdByteLength

  // Write level indices
  std::byte *lvlPtr = buffer.data() + kKtx2HeaderSize;
  for (std::uint32_t i = 0; i < levelCount; ++i) {
    write_u64_le(lvlPtr + i * 24 + 0, levelIndices[i].byteOffset);
    write_u64_le(lvlPtr + i * 24 + 8, levelIndices[i].byteLength);
    write_u64_le(lvlPtr + i * 24 + 16, levelIndices[i].uncompressedByteLength);
  }

  // Write DFD
  std::memcpy(buffer.data() + dfdOffset, dfd.data(), dfd.size());

  // Write KVD
  std::memcpy(buffer.data() + kvdOffset, kvd.data(), kvd.size());

  // Write payloads
  for (std::uint32_t i = 0; i < levelCount; ++i) {
    const auto &mipView = texture.mips.data[i];
    if (detail::cancelled(execution))
      return Error{ErrorCode::Cancelled, "Writing cancelled."};
    if (mipView.bytes.data != nullptr && mipView.bytes.size > 0) {
      std::memcpy(buffer.data() + levelIndices[i].byteOffset,
                  mipView.bytes.data, mipView.bytes.size);
    }
  }

  return buffer;
} catch (const std::bad_alloc &) {
  return Error{ErrorCode::AllocationFailure, "Writer allocation failed."};
} catch (const std::length_error &) {
  return Error{ErrorCode::IntegerOverflow, "Writer allocation is too large."};
}

Result<CompressedTexture>
read_ktx2_native_bc7(ByteSpan file, const ExecutionOptions &execution) try {

  auto infoRes = inspect_ktx2(file, execution);
  if (!infoRes) {
    return infoRes.error();
  }

  Ktx2Info info = infoRes.value();

  if (info.needsTranscode) {
    return Error{ErrorCode::UnsupportedSupercompression,
                 "KTX2 file requires transcoding (BasisETC1S or BasisUASTC); "
                 "call transcode_basis_ktx2 instead of read_ktx2_native_bc7."};
  }

  if (info.supercompressionScheme != 0) {
    return Error{ErrorCode::UnsupportedSupercompression,
                 "Supercompression is not supported for native BC7 KTX2."};
  }

  CompressedTexture result;
  result.format = info.targetFormat;
  result.metadata = info.metadata;

  std::uint64_t totalPayloadBytes = 0;
  for (const auto &lvlIdx : info.levelIndices) {
    if (!detail::add(totalPayloadBytes, lvlIdx.byteLength, totalPayloadBytes)) {
      return Error{ErrorCode::IntegerOverflow, "Combined BC7 size overflows."};
    }
  }

  const auto &limits = execution.limits;
  if (totalPayloadBytes > SIZE_MAX ||
      detail::exceeds(totalPayloadBytes, limits.maxOutputBytes) ||
      detail::exceeds(totalPayloadBytes, limits.maxWorkingBytes)) {
    return Error{ErrorCode::ResourceLimitExceeded,
                 "Decoded BC7 payload size exceeds maxOutputBytes limit."};
  }

  result.payload.reserve(totalPayloadBytes);

  std::uint64_t currentPayloadOffset = 0;
  std::uint32_t currentWidth = info.width;
  std::uint32_t currentHeight = info.height;

  for (std::uint32_t i = 0; i < info.levelCount; ++i) {
    if (detail::cancelled(execution))
      return Error{ErrorCode::Cancelled, "Reading cancelled."};
    const auto &lvlIdx = info.levelIndices[i];

    if (lvlIdx.byteOffset > file.size ||
        lvlIdx.byteLength > file.size - lvlIdx.byteOffset) {
      return Error{ErrorCode::TruncatedInput,
                   "Level index payload offset exceeds file size."};
    }

    std::uint32_t blocksX = calc_blocks(currentWidth);
    std::uint32_t blocksY = calc_blocks(currentHeight);
    std::uint64_t expectedSize =
        static_cast<std::uint64_t>(blocksX) * blocksY * 16;

    if (lvlIdx.byteLength < expectedSize) {
      return Error{
          ErrorCode::TruncatedInput,
          "Level payload byteLength is smaller than expected BC7 block size."};
    }

    const std::byte *srcPtr = file.data + lvlIdx.byteOffset;
    result.payload.insert(result.payload.end(), srcPtr,
                          srcPtr + lvlIdx.byteLength);

    MipLayout layout;
    layout.level = i;
    layout.width = currentWidth;
    layout.height = currentHeight;
    layout.blocksX = blocksX;
    layout.blocksY = blocksY;
    layout.offsetBytes = currentPayloadOffset;
    layout.sizeBytes = lvlIdx.byteLength;
    layout.rowPitchBytes = static_cast<std::uint64_t>(blocksX) * 16;

    result.mips.push_back(layout);

    currentPayloadOffset += lvlIdx.byteLength;
    currentWidth = std::max(1U, currentWidth / 2);
    currentHeight = std::max(1U, currentHeight / 2);
  }

  return result;
} catch (const std::bad_alloc &) {
  return Error{ErrorCode::AllocationFailure, "Reader allocation failed."};
} catch (const std::length_error &) {
  return Error{ErrorCode::IntegerOverflow, "Reader allocation is too large."};
}

} // namespace doku::texture
