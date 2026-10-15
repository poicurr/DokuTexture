#include "DokuTexture/Ktx2.hpp"
#include "DokuTexture/Transcode.hpp"

namespace doku::texture {
#if !TP_HAS_KTX2
Result<Ktx2Info> inspect_ktx2(ByteSpan, const ExecutionOptions &) {
  return Error{ErrorCode::MissingDependency,
               "KTX2 support was disabled at build time."};
}
Result<CompressedTexture> read_ktx2_native_bc7(ByteSpan,
                                               const ExecutionOptions &) {
  return Error{ErrorCode::MissingDependency,
               "KTX2 support was disabled at build time."};
}
Result<OwningBuffer> write_ktx2_native_bc7(const CompressedTextureView &,
                                           const ExecutionOptions &) {
  return Error{ErrorCode::MissingDependency,
               "KTX2 support was disabled at build time."};
}
#endif
#if !TP_HAS_BASIS
Result<CompressedTexture> transcode_basis_ktx2(ByteSpan, GpuFormat,
                                               const ExecutionOptions &) {
  return Error{ErrorCode::MissingDependency,
               "Basis support was disabled at build time."};
}
#endif
} // namespace doku::texture
