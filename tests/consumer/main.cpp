#include <DokuTexture/Encode.hpp>
#include <DokuTexture/Ktx2.hpp>
#include <DokuTexture/Transcode.hpp>
#include <optional>
#include <type_traits>

int main() {
  static_assert(std::is_same_v<std::optional<int>::value_type, int>);
  using namespace doku::texture;
  static_assert(std::is_same_v<ByteSpan, Span<std::byte>>);
  static_assert(std::is_same_v<decltype(ByteSpan{}.data), const std::byte *>);
  static_assert(std::is_same_v<decltype(Span<Rgba8MipView>{}.data),
                               const Rgba8MipView *>);
  static_assert(!std::is_assignable_v<decltype(*ByteSpan{}.data), std::byte>);
  static_assert(std::is_aggregate_v<ByteSpan>);
  constexpr ByteSpan empty{};
  static_assert(empty.data == nullptr && empty.size == 0);
  constexpr std::byte immutable[] = {std::byte{0x42}};
  const ByteSpan borrowed{immutable, 1};
  if (*borrowed.data != std::byte{0x42})
    return 4;
  std::byte rgba[4]{};
  auto encoded = encode_bc7_mip({1, 1, 4, {rgba, 4}}, {});
  if (!encoded || encoded.value().payload.size() != 16)
    return 1;
  const auto &t = encoded.value();
  CompressedMipView mip{t.mips[0], {t.payload.data(), t.payload.size()}};
  auto written = write_ktx2_native_bc7({t.format, t.metadata, {&mip, 1}}, {});
  if (!written && written.error().code != ErrorCode::MissingDependency)
    return 2;
  auto transcode = transcode_basis_ktx2({}, GpuFormat::BC7_UNorm, {});
  if (transcode || (transcode.error().code != ErrorCode::TruncatedInput &&
                    transcode.error().code != ErrorCode::MissingDependency))
    return 3;
  return 0;
}
