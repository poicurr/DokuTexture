# Recorded CPU validation

These results were recorded for the code in
`205252e0c571234e600eea62c23cebab49caf634`. The subsequent MIT license commit
`a3835874a002e455ab9a813279b2219dc1bdf7ac` changed license/documentation only.
Current build and format commands are in [README.md](README.md).

## Results

Environment: Windows x64, Visual Studio 2026 / MSVC 19.51, CMake 4.4.0,
clang-format 22.1.8 and independent KTX-Software v4.4.2. Builds used
`--parallel 2`; CTest ran with `-j 1`.

| Check | Recorded result |
| --- | --- |
| Release CTest | 7/7 passed |
| Debug CTest | 7/7 passed |
| Independent KTX validator | UNorm and sRGB writer outputs passed, included in CTest |
| Minimal example | Built and exited 0 |
| Parent consumer: full / native-only / both optional features disabled | All built and exited 0 |
| Benchmark source | Compiled in Release and Debug; workload not executed |
| clang-format dry run with warnings as errors | All 17 owned C++ files passed |

The parent consumer starts at C++14 and receives C++17 by linking only
`DokuTexture::DokuTexture`. It checks ByteSpan alias identity, read-only access,
aggregate/default construction and borrowing from a const byte array. Existing
CPU tests cover the container/encoder boundaries and independent fixtures;
fixture provenance and hashes are in [tests/fixtures/README.md](tests/fixtures/README.md).

## Source and dependency checks

The root-layout work starts from user main
`20317185bf706de6949948eff2b9d60ed09bdd3a`. After uniform clang-format, its
shared C++ files/documents and dependency notices matched the earlier review
`4aea41899ecbb83085dd0769a69902156b674af0`. Those review fixes, including the
public memory/OOM limits, were retained. A normalized diff of the header/style
update found no runtime implementation changes beyond the requested ByteSpan
alias; the additional C++ changes were its comments and consumer checks.

The header/style validation reused the exact pinned dependency sources from
the independent review checkout. Earlier Windows review builds had fetched
both dependencies in separate empty `build-msvc` and `build-clean` directories
without local vendor sources or FetchContent source/cache overrides. The later
validation is not an additional cache-free acquisition claim. Dependency pins,
third-party license/NOTICE files and binary fixtures were unchanged.

The project MIT permission/disclaimer text was compared with the requested
[hfsm reference](https://raw.githubusercontent.com/poicurr/hfsm/refs/heads/main/LICENSE).
Only the user-approved project attribution was substituted, to
`Copyright (c) 2026 poicurr`. No HFSM implementation was incorporated.

## Not exercised

Linux, VS2022 specifically, ASan/UBSan, coverage-guided fuzzing, thread/OOM fault
injection, active-call DLL unload, GPU validation, real-asset performance and
peak-memory/alpha-coverage benchmarks were not performed in this validation.
The current memory and cancellation guarantees are stated in README section 4.
