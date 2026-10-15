#pragma once

#include "types.hpp"

namespace doku::texture {

Result<CompressedTexture> transcode_basis_ktx2(
    ByteSpan file, GpuFormat target, const ExecutionOptions& execution);

} // namespace doku::texture
