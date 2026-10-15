#pragma once

#include "Types.hpp"

namespace doku::texture {

struct CompressedMipView {
  MipLayout layout;
  ByteSpan bytes;
};

struct CompressedTextureView {
  GpuFormat format = GpuFormat::BC7_UNorm;
  TextureMetadata metadata;
  Span<CompressedMipView> mips;
};

Result<OwningBuffer> write_ktx2_native_bc7(const CompressedTextureView &texture,
                                           const ExecutionOptions &execution);

Result<CompressedTexture>
read_ktx2_native_bc7(ByteSpan file, const ExecutionOptions &execution);

enum class SourceEncoding { NativeBC7, BasisETC1S, BasisUASTC, Other };

struct Ktx2LevelIndex {
  std::uint64_t byteOffset = 0;
  std::uint64_t byteLength = 0;
  std::uint64_t uncompressedByteLength = 0;
};

struct Ktx2Info {
  SourceEncoding sourceEncoding = SourceEncoding::Other;
  GpuFormat targetFormat = GpuFormat::BC7_UNorm;
  std::uint32_t vkFormat = 0;
  bool needsTranscode = false;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layerCount = 0;
  std::uint32_t faceCount = 0;
  std::uint32_t levelCount = 0;
  std::uint32_t supercompressionScheme = 0;
  TextureMetadata metadata;
  std::vector<Ktx2LevelIndex> levelIndices;
};

Result<Ktx2Info> inspect_ktx2(ByteSpan file, const ExecutionOptions &execution);

} // namespace doku::texture
