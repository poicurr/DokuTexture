# DokuTexture

Lightweight, independent C++17 library for CPU BC7 texture encoding, native BC7 KTX2 container I/O, and Basis Universal (ETC1S / UASTC) to BC7 transcoding.

- **CMake build-tree alias target**: `DokuTexture::DokuTexture` (no installed package export yet)
- **C++ Namespace**: `doku::texture`
- **C++ Standard**: C++17

Public headers are `<DokuTexture/Types.hpp>`, `<DokuTexture/Encode.hpp>`,
`<DokuTexture/Ktx2.hpp>` and `<DokuTexture/Transcode.hpp>`, in namespace
`doku::texture`. `ByteSpan` is `Span<std::byte>`; `Span<T>` exposes `const T*`
as a borrowed, read-only view and does not own storage or check element access.

---

## 1. Overview & Architectural Boundaries

`DokuTexture` is designed as a small, high-quality, lightweight utility library. It wraps trusted open-source codecs (`bc7enc` and `basis_universal`) with a clean, strongly-typed C++17 API.

This is preparation for possible future offline compression in DokuEngine after
its texture streaming work. There are currently no adopted consumers or Engine
integration. The consumer under `tests/consumer` is a build-validation fixture.

### What `DokuTexture` Handles:
- CPU RGBA8 image compression into GPU-ready BC7 blocks (`BC7_UNorm` and `BC7_Srgb`).
- Reading, writing, and inspecting native BC7 KTX2 container files (`supercompressionScheme = NONE`) with KVD (`KTXorientation`) and DFD headers.
- Transcoding Basis Universal (ETC1S + BasisLZ and UASTC + Zstd) KTX2 files directly into BC7 payloads.
- Reentrant row encoding with cooperative `std::atomic_bool` cancellation and checked input/output sizes. Working-memory checks are estimates; see limitations below.

### What `DokuTexture` Does NOT Handle (Caller Responsibility):
- GPU device management, VkFormat/DXGI_FORMAT conversions, and staging upload buffers.
- File I/O (caller provides read-only memory spans `ByteSpan` and receives `OwningBuffer`).
- Asset cooking, Virtual Texturing (VT) page tables, tile gutters, or streaming scheduling.
- Auto-generating missing mip levels or implicit lossy fallback to non-BC7 formats.

---

## 2. CMake Build Options

```cmake
option(TP_ENABLE_KTX2 "Enable KTX2 native read/write" ON)
option(TP_ENABLE_BASIS_TRANSCODER "Enable Basis Universal transcoding" ON)
option(TP_BUILD_TESTS "Build CTest unit tests" ON) # OFF when embedded
option(TP_BUILD_BENCHMARKS "Build performance benchmarks" OFF)
option(TP_BUILD_EXAMPLES "Build minimal consumer example" ON) # OFF when embedded
```

### Integration Example:
```cmake
add_subdirectory(DokuTexture)
target_link_libraries(my_game_engine PRIVATE DokuTexture::DokuTexture)
```

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

## 5. Performance Benchmark Results

Historical pre-review measurements reported by the original implementation on a 4-thread x86_64 Linux environment using `bench_throughput` (synthetic gradient test pattern). They were not reproduced during this review. The review corrected default UNorm weighting, so these are not measurements of the revised encoder:

| Quality Preset | Image Size | Threads | Wall Time (ms) | Throughput (Mpixels/s) | PSNR (dB) | Input -> Output Payload |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Fast** | 512x512 | 4 | 95.52 ms | **2.74** | 53.24 dB | 1024 KB -> 256 KB (4:1) |
| **Balanced** | 512x512 | 4 | 272.93 ms | **0.96** | 53.20 dB | 1024 KB -> 256 KB (4:1) |
| **High** | 512x512 | 4 | 601.95 ms | **0.44** | 53.27 dB | 1024 KB -> 256 KB (4:1) |
| **Fast** | 2048x2048 | 4 | 901.40 ms | **4.65** | 58.59 dB | 16 MB -> 4 MB (4:1) |
| **Balanced** | 2048x2048 | 4 | 2572.26 ms | **1.63** | 58.94 dB | 16 MB -> 4 MB (4:1) |
| **High** | 2048x2048 | 4 | 5802.44 ms | **0.72** | 58.92 dB | 16 MB -> 4 MB (4:1) |

The benchmark averages squared error over RGB channels in the encoded 0..255 value space, excludes alpha, and uses a single smooth opaque gradient. It is not linear-light PSNR or a representative asset corpus. CPU model, compiler flags, repetitions, peak memory and alpha coverage were not recorded; these results do not establish general quality or throughput. No new long-running benchmark or GPU test was run in this review.

---

## 6. Build, Test & Format

CMake 3.18+ is required. Run from the repository root. Initial dependency
acquisition requires Git and network access unless sources are already supplied.

### Windows x64 / Visual Studio 2026:

```powershell
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64 -DTP_BUILD_BENCHMARKS=ON -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build build-msvc --config Release --parallel 2
ctest --test-dir build-msvc -C Release --output-on-failure -j 1
cmake --build build-msvc --config Debug --parallel 2
ctest --test-dir build-msvc -C Debug --output-on-failure -j 1
```

The benchmark is compiled by this configuration; run it separately only when
performance measurements are intended. Optional `ktx` on PATH adds two validator
tests. `FETCHCONTENT_UPDATES_DISCONNECTED=ON` permits the initial download and
avoids subsequent dependency update checks.

The standalone parent consumer verifies the public CMake target:

```powershell
cmake -S tests/consumer -B consumer-full -G "Visual Studio 18 2026" -A x64 -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build consumer-full --config Release --parallel 2
.\consumer-full\Release\consumer.exe
```

For native-only or encode-only consumer checks, disable
`TP_ENABLE_BASIS_TRANSCODER`, and additionally disable `TP_ENABLE_KTX2` for
encode-only. Linux can use `cmake -S . -B build`, `cmake --build build --parallel 2`
and `ctest --test-dir build --output-on-failure -j 1`; see the recorded platform
coverage in [VALIDATION.md](VALIDATION.md).

Format all project-owned C++ sources/headers in `src`, `include`, `tests`,
`examples` and `bench` with the root `.clang-format`. Exclude third-party,
FetchContent, generated/build files and license text:

```powershell
$owned = @(git ls-files -- src include tests examples bench | Where-Object { $_ -match '\.(cc|cpp|cxx|h|hh|hpp|hxx|ipp|tpp)$' -and $_ -notmatch '(^|/)(third_party|_deps|generated)(/|$)' })
clang-format --dry-run --Werror --style=file @owned
# Apply with: clang-format -i --style=file @owned
```

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
DokuTexture's project-owned code is distributed under the [MIT License](LICENSE),
Copyright (c) 2026 poicurr. Dependencies retain the licenses listed above.
Redistributable packages must retain the project LICENSE, applicable third-party
license texts and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), including the
Basis NOTICE. This repository does not yet provide install/package export targets.
