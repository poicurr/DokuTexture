#pragma once

#include "types.hpp"

namespace doku::texture {

Result<CompressedTexture> encode_bc7_mip(
    Rgba8MipView mip, const EncodeOptions& options);

Result<CompressedTexture> encode_bc7_chain(
    Span<Rgba8MipView> suppliedMips, const EncodeOptions& options);

} // namespace doku::texture
