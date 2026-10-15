# Public header and formatting update

This update starts from the user's new root-layout `main` commit
`20317185bf706de6949948eff2b9d60ed09bdd3a` in an independent checkout.
After applying the same clang-format style to both revisions, every C++ file
and shared tracked document/dependency notice in that main matches the earlier
review's `4aea41899ecbb83085dd0769a69902156b674af0` content. The previous fixes
and public memory/OOM comments are retained. The previous review branch is no
longer present on the remote; its local commits remain untouched.

## Include and ByteSpan compatibility

| Previous public header | Current public header |
| --- | --- |
| `doku/texture/types.hpp` | `DokuTexture/Types.hpp` |
| `doku/texture/encode.hpp` | `DokuTexture/Encode.hpp` |
| `doku/texture/ktx2.hpp` | `DokuTexture/Ktx2.hpp` |
| `doku/texture/transcode.hpp` | `DokuTexture/Transcode.hpp` |

The namespace stays `doku::texture`; the CMake target stays
`DokuTexture::DokuTexture`. Its public include directory remains `include`,
so consumers include `<DokuTexture/Encode.hpp>`, not `<include/...>`.
CMake lists the new headers and all project includes use their exact case.
No forwarding headers for the old paths are provided.

`Span<T>` still exposes `const T* data` and `size_t size`: it is a borrowed,
read-only view even for a non-const `T`. `ByteSpan` is now declared after the
template as `using ByteSpan = Span<std::byte>;`. Existing aggregate calls
such as `ByteSpan{buffer.data(), buffer.size()}` remain valid and cannot mutate
the source. A separate `ConstByteSpan` would add no const protection here, so
none is introduced. Default construction remains `{nullptr, 0}`. Span does not
own storage, extend its lifetime or validate an arbitrary pointer's allocation;
callers must supply truthful spans and keep them alive through synchronous calls.
The API's input null/range/overflow checks are retained.

This is a source and ABI migration. Update includes and rebuild the library and
all consumers. `ByteSpan` is no longer a distinct struct: forward declarations
must be replaced with the public include, and overloads/specializations that
previously distinguished `ByteSpan` from `Span<std::byte>` must be consolidated.
The consumer checks alias identity, const access, aggregate/default construction
and borrowing from a const byte array at compile time and runtime.

## Formatting scope

`.clang-format` uses exactly:

```yaml
BasedOnStyle: LLVM
AlwaysBreakTemplateDeclarations: Yes
IndentCaseLabels: true
```

All 17 project-owned C++ sources/headers in `src`, `include`, `tests` (including
the parent consumer), `examples` and `bench` are formatted, using clang-format
22.1.8. This deliberately includes more than `src` alone. Third-party sources,
FetchContent `_deps`, build/generated files, binary fixtures and license texts
are excluded. No codec source or dependency pin is changed.

PowerShell format verification from the repository root:

```powershell
$owned = @(git ls-files -- src include tests examples bench | Where-Object { $_ -match '\.(cc|cpp|cxx|h|hh|hpp|hxx|ipp|tpp)$' -and $_ -notmatch '(^|/)(third_party|_deps|generated)(/|$)' })
clang-format --dry-run --Werror --style=file @owned
# Apply the same style with: clang-format -i --style=file @owned
```

## Build and consumer commands

Run from the new repository root. Fresh configure fetches pinned dependencies;
already populated sources can be reused with CMake's `FETCHCONTENT_SOURCE_DIR_*`
overrides. This update reused the previously verified exact upstream revisions
from the independent review checkout; it is not a new cache-free fetch claim.

```powershell
cmake -S . -B build-msvc -G "Visual Studio 18 2026" -A x64 -DTP_BUILD_BENCHMARKS=ON -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build build-msvc --config Release --parallel 2
ctest --test-dir build-msvc -C Release --output-on-failure -j 1
cmake --build build-msvc --config Debug --parallel 2
ctest --test-dir build-msvc -C Debug --output-on-failure -j 1

cmake -S tests/consumer -B consumer-full -G "Visual Studio 18 2026" -A x64 -DFETCHCONTENT_UPDATES_DISCONNECTED=ON
cmake --build consumer-full --config Release --parallel 2
.\consumer-full\Release\consumer.exe
```

The benchmark is compiled to cover the formatted source; its workload is not
executed. The consumer deliberately starts with C++14 and inherits C++17 by
linking only the public target. Native-only and both-disabled consumer builds
use `-DTP_ENABLE_BASIS_TRANSCODER=OFF`, and additionally
`-DTP_ENABLE_KTX2=OFF` for the latter. No GPU operation is required or performed.

Validation on Windows x64, MSVC 19.51 / CMake 4.4.0: Release 7/7 and Debug
7/7 CTests passed, including both independent KTX validator tests. The example
and full/native-only/both-disabled parent consumers built and exited 0.
`clang-format --dry-run --Werror` passed for all 17 owned C++ files. A normalized
diff confirmed that changes beyond formatting/include spelling are limited to
the ByteSpan alias/comments and consumer constness checks. Dependency source
pins, license/NOTICE files and binary fixtures are unchanged.

## Requested MIT license

The requested reference is
[poicurr/hfsm LICENSE](https://raw.githubusercontent.com/poicurr/hfsm/refs/heads/main/LICENSE),
whose exact attribution is `Copyright (c) 2022 dknb0709`. No HFSM implementation
is incorporated by this update. That notice identifies the reference project's
holder; it does not establish the DokuTexture wrapper's holder. A project LICENSE
has therefore not been added pending confirmation that this attribution also
applies here, or provision of the appropriate DokuTexture year/holder. No new
holder or year is inferred from a GitHub username, commit author or file date.

The wrapper can use the requested MIT terms once its attribution is resolved.
Its dependencies retain their own grants: bc7enc MIT, Basis Apache-2.0, bundled
Zstd BSD-3-Clause plus the MIT divsufsort notice. They are not relicensed to MIT.
Keep the license texts, attribution notices and Apache redistribution conditions
when distributing those components, including binary distributions. All existing
`licenses/`, `THIRD_PARTY_NOTICES.md` and fixture provenance/license files are
unchanged; no incompatible dependency license was identified in the compiled
subset reviewed here.

Reference terms checked: [MIT](https://opensource.org/license/mit) and
[Apache-2.0, especially section 4](https://www.apache.org/licenses/LICENSE-2.0).
