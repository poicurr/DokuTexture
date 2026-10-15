# Third Party Notices

This project (`DokuTexture`) incorporates open-source software dependencies.

## 1. richgel999/bc7enc
- **Official Repository**: https://github.com/richgel999/bc7enc
- **Upstream Revision**: Commit SHA `f66c2e489b07138f2673a2fb3d27c1aa1d565c48`
- **License**: MIT selected from upstream MIT/public-domain alternatives; see `licenses/bc7enc-LICENSE`.
- **Copyright**: Copyright (c) 2020 Richard Geldreich, Jr.
- **Components Used**:
  - `bc7enc.h`, `bc7enc.c`: Standalone C/C++ BC7 CPU Encoder.
  - `bc7decomp.h`, `bc7decomp.cpp`: Standalone CPU BC7 Decoder used for quality verification and CTest verification.

## 2. Binomial Basis Universal (`basis_universal`)
- **Official Repository**: https://github.com/BinomialLLC/basis_universal
- **Upstream Revision**: Commit SHA `99f52d63aa6799cbdaecfe977111dc5ec3b31d47`
- **License**: Apache License, Version 2.0 for the transcoder, with upstream `licenses/basis-universal-NOTICE` and `licenses/basis-universal-LICENSE` retained. The selected compiled subset does not build the Basis encoder/image loaders.
- **Copyright**: Copyright (C) 2019-2026 Binomial LLC. All Rights Reserved.
- **Components Used**:
  - `transcoder/basisu_transcoder.h`, `transcoder/basisu_transcoder.cpp`: Basis Universal ETC1S and UASTC transcoding to native GPU formats (including BC7).
  - `transcoder/basisu_file_headers.h`: KTX2 and Data Format Descriptor (DFD) header layouts.
  - `zstd/zstd.c`: bundled Zstandard implementation for UASTC + Zstd (BasisLZ uses Basis codebooks, not Zstd). BSD-3-Clause selected from BSD/GPL alternatives; see `licenses/zstd-LICENSE` and `licenses/zstd-NOTICE`, including xxHash/Tino Reichardt attribution and the MIT libdivsufsort-lite notice in the amalgamation.

`bc7decomp.cpp` is compiled into a separate test/benchmark helper, not the production library. The pinned bc7enc and Basis upstream files are unmodified. Test fixtures are original generated patterns with provenance and licensing in `tests/fixtures/README.md`.
