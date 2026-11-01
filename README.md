# DokuTexture

Standalone C++17 library for CPU BC7 texture encoding, native BC7 KTX2 container I/O, and Basis Universal (ETC1S / UASTC) to BC7 transcoding.

- **CMake target**: `DokuTexture::DokuTexture` (build-tree integration; no installed package export yet)
- **C++ Namespace**: `doku::texture`
- **C++ Standard**: C++17

Public headers are `<DokuTexture/Types.hpp>`, `<DokuTexture/Encode.hpp>`,
`<DokuTexture/Ktx2.hpp>` and `<DokuTexture/Transcode.hpp>`, in namespace
`doku::texture`. `ByteSpan` is `Span<std::byte>`; `Span<T>` exposes `const T*`
as a borrowed, read-only view and does not own storage or check element access.

---

## 1. Overview

`DokuTexture` provides a strongly-typed, memory-to-memory C++17 API backed by
`bc7enc` and `basis_universal`. It can be used in standalone tools, asset
pipelines, and applications without depending on a particular rendering engine,
graphics API, or filesystem.

### Features
- CPU RGBA8 image compression into GPU-ready BC7 blocks (`BC7_UNorm` and `BC7_Srgb`).
- Reading, writing, and inspecting native BC7 KTX2 container files (`supercompressionScheme = NONE`) with KVD (`KTXorientation`) and DFD headers.
- Transcoding Basis Universal (ETC1S + BasisLZ and UASTC + Zstd) KTX2 files directly into BC7 payloads.
- Reentrant row encoding with cooperative `std::atomic_bool` cancellation and checked input/output sizes. Working-memory checks are estimates; see limitations below.

### Out of Scope (Caller Responsibility)
- GPU device management, VkFormat/DXGI_FORMAT conversions, and staging upload buffers.
- File I/O (caller provides read-only memory spans `ByteSpan` and receives `OwningBuffer`).
- Asset cooking, Virtual Texturing (VT) page tables, tile gutters, or streaming scheduling.
- Auto-generating missing mip levels or implicit lossy fallback to non-BC7 formats.

---

## 2. CMake Integration

CMake 3.18+ and a C++17 compiler are required. Add the library as a subdirectory
and link its build-tree target:

```cmake
add_subdirectory(path/to/DokuTexture)
target_link_libraries(my_app PRIVATE DokuTexture::DokuTexture)
```

The repository does not yet provide an installed CMake package or exported targets.

### Configuration Options

| Option | Default | Purpose |
| :--- | :--- | :--- |
| `TP_ENABLE_KTX2` | `ON` | Enable native BC7 KTX2 reading and writing |
| `TP_ENABLE_BASIS_TRANSCODER` | `ON` | Enable Basis Universal KTX2 transcoding |
| `TP_BUILD_TESTS` | `ON` standalone, `OFF` as subproject | Build CTest tests |
| `TP_BUILD_BENCHMARKS` | `OFF` | Build the throughput benchmark |
| `TP_BUILD_EXAMPLES` | `ON` standalone, `OFF` as subproject | Build the minimal example |

For an encode-to-KTX2 usage example, see
[examples/minimal_consumer.cpp](examples/minimal_consumer.cpp). The
[tests/consumer](tests/consumer) project demonstrates integration from a
separate CMake parent project.

---

## 3. Support Matrix

| Source Representation | Target GPU Format | Supercompression | Mip Chains | Metadata Support | Status |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **CPU RGBA8 Mip / Chain** | `BC7_UNorm`, `BC7_Srgb` | None (`0`) | Base-only, Partial, Full Chain | Linear / sRGB, TopLeft | **Supported** |
| **Native BC7 KTX2** | `BC7_UNorm`, `BC7_Srgb` | None (`0`) | Base-only, Partial, Full Chain | DFD 1.3, KVD (`KTXorientation`) | **Supported** |
| **Basis ETC1S KTX2** | `BC7_UNorm`, `BC7_Srgb` | BasisLZ (`1`) | Single / Mipmap levels | Header / Level Indices | **Supported** |
| **Basis UASTC KTX2** | `BC7_UNorm`, `BC7_Srgb` | None (`0`), Zstd (`2`) | Single / Mipmap levels | Header / Level Indices | **Supported** |
| **Other Formats (ASTC/BC1/BC4/etc)**| Non-BC7 | Any | - | - | **UnsupportedFormat Error** |

---

## 4. Contracts & Guarantees

### 4.1 sRGB / UNorm Contract
- `GpuFormat::BC7_Srgb` configures perceptual color error weighting (`bc7enc_compress_block_params_init_perceptual_weights`).
- `GpuFormat::BC7_UNorm` configures linear color error weighting (`bc7enc_compress_block_params_init_linear_weights`).
- `DokuTexture` does not perform implicit gamma transforms or alter raw pixel values.
- Unknown encode color space is resolved from the target; contradictory metadata is rejected. Basis targets must match the source transfer function, otherwise `UnsupportedOption` is returned.
- Basis color primaries must be BT.709; other primaries return `UnsupportedMetadata` because this API cannot represent or convert them.

### 4.2 Alpha Contract
- Opaque RGBA8 blocks (alpha=255) utilize BC7 Mode 1 or Mode 6 for optimal RGB fidelity.
- The pinned encoder uses Mode 6 for translucent blocks (Mode 7 is not supported).
- Alpha semantics/cutoff are caller annotations during encode. No premultiplication, alpha testing, coverage preservation or mip filtering is performed.
- Native KTX2 reads/writes straight or premultiplied storage flags and `rd`/`ru` orientation. Unknown storage is written as straight. Writer rejects known alpha semantics and nonidentity swizzles rather than silently losing them. Reader preserves standard swizzles; unrelated KVD is not returned or preserved.
- Native reader currently accepts one canonical BT.709 BC7 basic DFD. Other descriptor layouts/primaries return `UnsupportedMetadata`; this is a limited 2D reader, not a complete KTX2 validator.

### 4.3 Supplied Mip Contract
- `encode_bc7_chain` requires an ascending sequence of valid mip views starting from Level 0.
- Dimensions must satisfy `max(1, baseWidth >> level)` and `max(1, baseHeight >> level)`. Missing levels or dimension gaps return `ErrorCode::InvalidMipChain`.
- Base-only and partial chains are supported. Repeated 1x1 levels are rejected. Tiny and NPOT dimensions are supported; KTX2 itself imposes no multiple-of-four base restriction. Device or profile restrictions remain the caller's responsibility.

### 4.4 Threading & Cancellation
- Functions are reentrant and safe for concurrent execution across independent caller threads.
- For single-call parallelism (`maxWorkers > 1`), row ranges are divided among worker threads without thread pool leaks.
- When `options.execution.cancelFlag` is set to `true`, workers stop block processing and join safely before returning `ErrorCode::Cancelled`.
- Encode checks cancellation per block. KTX I/O and Basis check per level; an upstream transcode/codebook call cannot be interrupted mid-call. `maxWorkers` controls encode only; Basis is serial. Independent caller jobs have independent worker limits, with no global scheduler.
- Input spans and the cancel flag must remain alive and unchanged until the synchronous call returns (the flag may be atomically changed). Results own their payloads. There is no asynchronous context/shutdown API; callers must join active calls before DLL unload.

### 4.5 Limits and allocation failures
- A zero resource limit means no caller-specified bound. Checked arithmetic, dimensions, mip counts, cumulative input/output size and representability are validated before payload allocation.
- `maxWorkingBytes` checks known payload/temporary sizes (Basis includes file plus declared inflated levels plus output). It does **not** account for allocator overhead, thread stacks, initialization tables, all backend scratch allocations, or process RSS. It is not a strict memory budget.
- Wrapper `std::bad_alloc` becomes `AllocationFailure`; the pinned Basis container can terminate on an internal allocation failure. Strict OOM recovery and hostile-input process isolation are not provided.
- Disabling Basis avoids fetching/compiling Basis and Zstd. Disabled APIs return `MissingDependency`; the inspector/native reader remains available when Basis is enabled because transcode uses it.

---

## 5. Benchmarking

The optional `bench_throughput` executable measures CPU BC7 encoding on
synthetic, opaque RGBA8 gradients. Build it with:

```sh
cmake -S . -B build-bench -DTP_BUILD_BENCHMARKS=ON
cmake --build build-bench --config Release
```

Run `bench_throughput` from the generated build output directory (for
example, `build-bench/bench/bench_throughput` with a single-configuration
generator, or `build-bench/bench/Release/bench_throughput.exe` with a
multi-configuration Visual Studio generator).

The benchmark covers 512×512 and 2048×2048 images with Fast, Balanced, and
High presets using one or four workers. It reports wall time, throughput,
encoded payload size, and RGB-only PSNR calculated in the encoded 0–255 value
space. It excludes alpha, is not linear-light PSNR, and does not represent
typical texture assets. Results depend on the CPU, compiler, build flags, and
workload; measure on your target environment rather than relying on historical
figures.

---

## 6. Build and Test

Configure and run the standalone tests from the repository root:

```sh
cmake -S . -B build -DTP_BUILD_TESTS=ON -DTP_BUILD_EXAMPLES=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

For single-configuration generators such as Unix Makefiles or Ninja, add
`-DCMAKE_BUILD_TYPE=Release` when configuring to select a Release build.
For multi-configuration generators such as Visual Studio, `--config Release`
selects the configuration.

The first configuration downloads pinned third-party dependencies if local
sources are unavailable. Offline configuration requires those sources to
already be available, for example through `src/third_party` or CMake
`FETCHCONTENT_SOURCE_DIR_*` overrides. If KTX-Software's `ktx` command is on
`PATH`, additional KTX validator tests are enabled.

---

## 7. Pinned Dependencies & Licensing

1. **richgel999/bc7enc**
   - Official Repo: https://github.com/richgel999/bc7enc (Commit SHA `f66c2e489b07138f2673a2fb3d27c1aa1d565c48`)
   - License: MIT selected from MIT/public-domain dual license
2. **Binomial Basis Universal (`basis_universal`)**
   - Official Repo: https://github.com/BinomialLLC/basis_universal (Commit SHA `99f52d63aa6799cbdaecfe977111dc5ec3b31d47`)
   - Transcoder: Apache License 2.0; bundled Zstd: BSD-3-Clause selected from BSD/GPL dual license, plus MIT divsufsort code in the amalgamation

Dependencies are fetched by full commit SHA when local sources are absent.
Local `src/third_party` fallbacks and CMake `FETCHCONTENT_SOURCE_DIR_*` overrides
are intentionally supported and are the caller's responsibility to verify.
License texts and Basis NOTICE are retained in [licenses/](licenses/).
DokuTexture is distributed under the [MIT License](LICENSE),
Copyright (c) 2026 poicurr. Dependencies retain the licenses listed above.
Redistributable packages must retain the project LICENSE, applicable third-party
license texts and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), including the
Basis NOTICE. This repository does not yet provide install/package export targets.
