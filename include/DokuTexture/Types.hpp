#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace doku::texture {

// Borrowed, read-only view: Span<T> exposes const T even when T is not const.
// Does not own storage or extend its lifetime. APIs validate null/size/ranges.
template <class T>
struct Span {
  const T *data = nullptr;
  std::size_t size = 0;
};

using ByteSpan = Span<std::byte>;

using OwningBuffer = std::vector<std::byte>;

enum class GpuFormat {
  BC7_UNorm,
  BC7_Srgb,
  BC4_UNorm,
  ETC1_RGB,
  ETC2_RGBA,
  ASTC4x4_UNorm,
  ASTC4x4_Srgb
};

enum class ColorSpace { Linear, SRGB, Unknown };

enum class AlphaSemantic { Opaque, Mask, Blend, Unknown };

enum class AlphaStorage { Straight, Premultiplied, Unknown };

enum class Orientation { TopLeft, BottomLeft, Other };

enum class Channel { R, G, B, A, Zero, One };

enum class Quality { Fast, Balanced, High };

enum class ErrorCode {
  InvalidArgument,
  InvalidMipChain,
  InvalidContainer,
  TruncatedInput,
  IntegerOverflow,
  UnsupportedFormat,
  UnsupportedTextureType,
  UnsupportedSupercompression,
  UnsupportedMetadata,
  UnsupportedOption,
  MissingDependency,
  ResourceLimitExceeded,
  // Reports recoverable allocation failures; Basis may terminate on internal
  // OOM.
  AllocationFailure,
  Cancelled,
  BackendFailure
};

constexpr std::uint32_t kMipLevelNotApplicable = 0xFFFFFFFF;

struct Error {
  ErrorCode code = ErrorCode::InvalidArgument;
  std::string message;
  std::uint32_t mipLevel = kMipLevelNotApplicable;
};

template <class T>
class Result {
public:
  Result(T val) : m_value(std::move(val)), m_hasValue(true) {}
  Result(Error err) : m_error(std::move(err)), m_hasValue(false) {}

  bool has_value() const { return m_hasValue; }
  explicit operator bool() const { return m_hasValue; }

  const T &value() const { return m_value; }
  T &value() { return m_value; }

  const Error &error() const { return m_error; }

private:
  T m_value{};
  Error m_error{ErrorCode::InvalidArgument, ""};
  bool m_hasValue = false;
};

struct TextureMetadata {
  ColorSpace colorSpace = ColorSpace::Unknown;
  AlphaSemantic alphaSemantic = AlphaSemantic::Unknown;
  AlphaStorage alphaStorage = AlphaStorage::Unknown;
  float alphaCutoff = 0.5f;
  Orientation orientation = Orientation::TopLeft;
  std::array<Channel, 4> swizzle = {Channel::R, Channel::G, Channel::B,
                                    Channel::A};
};

struct Rgba8MipView {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint64_t rowPitchBytes = 0;
  ByteSpan bytes;
};

struct MipLayout {
  std::uint32_t level = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t blocksX = 0;
  std::uint32_t blocksY = 0;
  std::uint64_t offsetBytes = 0;
  std::uint64_t sizeBytes = 0;
  std::uint64_t rowPitchBytes = 0;
};

struct CompressedTexture {
  GpuFormat format = GpuFormat::BC7_UNorm;
  TextureMetadata metadata;
  OwningBuffer payload;
  std::vector<MipLayout> mips;
};

struct ResourceLimits {
  std::uint32_t maxWidth = 16384;
  std::uint32_t maxHeight = 16384;
  std::uint32_t maxMipCount = 16;
  std::uint64_t maxInputBytes = 1024ULL * 1024ULL * 1024ULL; // 1 GB
  std::uint64_t maxOutputBytes = 1024ULL * 1024ULL * 1024ULL;
  // Preflight estimate of known buffers, not a total process-memory/RSS cap.
  // Excludes backend scratch, initialization tables, thread stacks and
  // allocator overhead.
  std::uint64_t maxWorkingBytes = 1024ULL * 1024ULL * 1024ULL;
};

struct ExecutionOptions {
  std::uint32_t maxWorkers = 1;
  ResourceLimits limits;
  const std::atomic_bool *cancelFlag = nullptr;
};

struct EncodeOptions {
  GpuFormat target = GpuFormat::BC7_UNorm;
  Quality quality = Quality::Balanced;
  TextureMetadata metadata;
  ExecutionOptions execution;
};

} // namespace doku::texture
