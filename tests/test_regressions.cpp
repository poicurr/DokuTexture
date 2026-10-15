#include "doku/texture/encode.hpp"
#include "doku/texture/ktx2.hpp"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <fstream>
#include <iterator>

using namespace doku::texture;
static std::uint64_t u64(const OwningBuffer &b, std::size_t p) {
  std::uint64_t n;
  std::memcpy(&n, b.data() + p, 8);
  return n;
}
template <class T>
void put(OwningBuffer &b, std::size_t p, T n) {
  std::memcpy(b.data() + p, &n, sizeof(n));
}

int main() {
  {
    std::ifstream in(std::string(TP_FIXTURE_DIR) + "/native-bc7-linear.ktx2",
                     std::ios::binary);
    std::vector<char> file((std::istreambuf_iterator<char>(in)), {});
    assert(!file.empty());
    ByteSpan span{reinterpret_cast<const std::byte *>(file.data()),
                  file.size()};
    auto info = inspect_ktx2(span, {});
    auto result = read_ktx2_native_bc7(span, {});
    assert(info && result && result.value().payload.size() == 112);
    assert(result.value().mips.size() == 4 &&
           result.value().metadata.colorSpace == ColorSpace::Linear);
    for (unsigned i = 0; i < 4; ++i) {
      const auto &layout = result.value().mips[i];
      const auto &source = info.value().levelIndices[i];
      assert(std::memcmp(result.value().payload.data() + layout.offsetBytes,
                         file.data() + source.byteOffset,
                         layout.sizeBytes) == 0);
    }
  }
  EncodeOptions o;
  o.quality = Quality::Fast;
  std::vector<std::byte> raw(200, std::byte{127});
  auto overflow =
      encode_bc7_mip({1, 3, UINT64_MAX / 2 + 1, {raw.data(), 4}}, o);
  assert(!overflow && overflow.error().code == ErrorCode::IntegerOverflow);
  Rgba8MipView overflowingPitch{1, 3, UINT64_MAX / 2 + 1, {raw.data(), 4}};
  auto chainOverflow = encode_bc7_chain({&overflowingPitch, 1}, o);
  assert(!chainOverflow &&
         chainOverflow.error().code == ErrorCode::IntegerOverflow);
  for (auto w : {1U, 2U, 3U, 4U, 5U, 7U, 8U})
    for (auto h : {1U, 3U, 7U}) {
      const auto pitch = w * 4 + 12;
      std::vector<std::byte> padded((h - 1) * pitch + w * 4, std::byte{0x7F});
      std::vector<std::byte> tight(w * h * 4, std::byte{0x7F});
      auto a = encode_bc7_mip({w, h, pitch, {padded.data(), padded.size()}}, o);
      auto b = encode_bc7_mip({w, h, w * 4, {tight.data(), tight.size()}}, o);
      assert(a && b && a.value().payload == b.value().payload);
      assert(a.value().payload.size() == ((w + 3) / 4) * ((h + 3) / 4) * 16);
    }
  Rgba8MipView tiny{1, 1, 4, {raw.data(), 4}};
  Rgba8MipView repeated[] = {tiny, tiny};
  auto chain = encode_bc7_chain({repeated, 2}, o);
  assert(!chain && chain.error().code == ErrorCode::InvalidMipChain);
  Rgba8MipView pair[] = {{4, 4, 16, {raw.data(), 64}},
                         {2, 2, 8, {raw.data(), 16}}};
  o.execution.limits.maxOutputBytes = 16;
  chain = encode_bc7_chain({pair, 2}, o);
  assert(!chain && chain.error().code == ErrorCode::ResourceLimitExceeded);
  o.execution = {};
  std::atomic_bool preCancelled{true};
  o.execution.cancelFlag = &preCancelled;
  chain = encode_bc7_chain({pair, 2}, o);
  assert(!chain && chain.error().code == ErrorCode::Cancelled);
  o.execution = {};
  pair[1].bytes.data = nullptr;
  chain = encode_bc7_chain({pair, 2}, o);
  assert(!chain && chain.error().code == ErrorCode::TruncatedInput &&
         chain.error().mipLevel == 1);
  o.execution = {};
  o.execution.limits.maxWorkingBytes = 1;
  auto working = encode_bc7_mip(tiny, o);
  assert(!working && working.error().code == ErrorCode::ResourceLimitExceeded);
  o.execution = {};
  o.execution.maxWorkers = UINT32_MAX;
  auto hugeWorkers = encode_bc7_mip({4, 8, 16, {raw.data(), 128}}, o);
  assert(hugeWorkers && hugeWorkers.value().payload.size() == 32);
  o.execution = {};
  for (auto format : {GpuFormat::BC7_UNorm, GpuFormat::BC7_Srgb}) {
    o.target = format;
    std::vector<OwningBuffer> inputs;
    std::vector<Rgba8MipView> mips;
    unsigned w = 100, h = 200;
    do {
      inputs.emplace_back(w * h * 4, std::byte{100});
      mips.push_back(
          {w, h, w * 4, {inputs.back().data(), inputs.back().size()}});
      if (w == 1 && h == 1)
        break;
      w = std::max(1U, w / 2);
      h = std::max(1U, h / 2);
    } while (true);
    auto encoded = encode_bc7_chain({mips.data(), mips.size()}, o);
    assert(encoded);
    auto &tex = encoded.value();
    assert(tex.metadata.colorSpace == (format == GpuFormat::BC7_Srgb
                                           ? ColorSpace::SRGB
                                           : ColorSpace::Linear));
    std::vector<CompressedMipView> views;
    for (auto l : tex.mips)
      views.push_back({l,
                       {tex.payload.data() + l.offsetBytes,
                        static_cast<std::size_t>(l.sizeBytes)}});
    TextureMetadata metadata = tex.metadata;
    metadata.orientation = Orientation::BottomLeft;
    metadata.alphaStorage = AlphaStorage::Premultiplied;
    CompressedTextureView view{format, metadata, {views.data(), views.size()}};
    ExecutionOptions e;
    auto written = write_ktx2_native_bc7(view, e);
    assert(written);
    auto file = std::move(written.value());
    auto span = [&] { return ByteSpan{file.data(), file.size()}; };
    auto read = read_ktx2_native_bc7(span(), e);
    assert(read && read.value().payload == tex.payload);
    assert(read.value().metadata.orientation == Orientation::BottomLeft);
    assert(read.value().metadata.alphaStorage == AlphaStorage::Premultiplied);
    for (unsigned i = 1; i < views.size(); ++i)
      assert(u64(file, 80 + i * 24) < u64(file, 80 + (i - 1) * 24));
    std::ofstream out(format == GpuFormat::BC7_Srgb ? "native-srgb.ktx2"
                                                    : "native-unorm.ktx2",
                      std::ios::binary);
    out.write(reinterpret_cast<const char *>(file.data()),
              static_cast<std::streamsize>(file.size()));
    assert(out.good());
    out.close();
    auto original = file;
    put(file, 80, u64(file, 104));
    assert(!inspect_ktx2(span(), e));
    file = original;
    put(file, 80, std::uint64_t(0));
    assert(!inspect_ktx2(span(), e));
    file = original;
    put(file, 88, std::uint64_t(16));
    assert(!read_ktx2_native_bc7(span(), e));
    file = original;
    put(file, 48, std::uint32_t(UINT32_MAX));
    assert(!inspect_ktx2(span(), e));
    file = original;
    const auto dfdOffset = 80 + views.size() * 24;
    file[dfdOffset + 14] =
        format == GpuFormat::BC7_Srgb ? std::byte{1} : std::byte{2};
    assert(!inspect_ktx2(span(), e));
    file = original;
    put(file, 28, std::uint32_t(1));
    auto unsupported = read_ktx2_native_bc7(span(), e);
    assert(!unsupported &&
           unsupported.error().code == ErrorCode::UnsupportedTextureType);
    file = original;
    put(file, 44, std::uint32_t(2));
    unsupported = read_ktx2_native_bc7(span(), e);
    assert(!unsupported &&
           unsupported.error().code == ErrorCode::UnsupportedSupercompression);
    file = original;
    std::atomic_bool cancel{true};
    e.cancelFlag = &cancel;
    assert(!write_ktx2_native_bc7(view, e));
    assert(!read_ktx2_native_bc7(span(), e));
    e = {};
    views[0].bytes.data = nullptr;
    assert(!write_ktx2_native_bc7(view, e));
  }
}
