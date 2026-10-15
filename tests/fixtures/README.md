# External-tool KTX2 fixtures

These tiny files were generated from original synthetic pixels for this project,
not copied from upstream photographs. The raw pixels and these fixtures are
dedicated to the public domain under CC0-1.0:
https://creativecommons.org/publicdomain/zero/1.0/

Generator: installed Khronos KTX-Software `ktx version: v4.4.2`
([release source](https://github.com/KhronosGroup/KTX-Software/tree/v4.4.2)).
Generator `ktx.exe` SHA256:
`9718ed380605db33e18a74621978434cedaf119fa4fe25e142a30cabe02c34ef`.
The installed executable's provenance was not independently rebuilt or attested.
The generator uses its own codecs, independently of DokuTexture's pinned backend.

Generate the four source levels with Python (outside the repository):

```python
from pathlib import Path
for d in (8, 4, 2, 1):
    for kind in ("rgba", "rgb"):
        Path(f"{kind}{d}.raw").write_bytes(bytes(
            c for y in range(d) for x in range(d)
            for c in (x*255//max(1,d-1), y*255//max(1,d-1), 100,
                      255 if kind == "rgb" else (x+y)*255//max(1,2*d-2))))
```

All commands use `--threads 2 --testrun --raw --width 8 --height 8 --levels 4`.

```powershell
ktx create --raw --format R8G8B8A8_SRGB --width 8 --height 8 --levels 4 --encode basis-lz --threads 2 --testrun rgba8.raw rgba4.raw rgba2.raw rgba1.raw etc1s-srgb.ktx2
ktx create --raw --format R8G8B8A8_SRGB --width 8 --height 8 --levels 4 --encode basis-lz --threads 2 --testrun rgb8.raw rgb4.raw rgb2.raw rgb1.raw etc1s-rgb-srgb.ktx2
ktx create --raw --format R8G8B8A8_UNORM --width 8 --height 8 --levels 4 --encode uastc --threads 2 --testrun rgba8.raw rgba4.raw rgba2.raw rgba1.raw uastc-linear.ktx2
ktx create --raw --format R8G8B8A8_SRGB --width 8 --height 8 --levels 4 --encode uastc --zstd 3 --threads 2 --testrun rgba8.raw rgba4.raw rgba2.raw rgba1.raw uastc-zstd-srgb.ktx2
ktx transcode --target bc7 --testrun uastc-linear.ktx2 native-bc7-linear.ktx2
```

| File | SHA256 |
| --- | --- |
| etc1s-srgb.ktx2 | b120cd7d7d77c043545ae8d371863cc467a9216aad670cdeb6044dbacee1a552 |
| etc1s-rgb-srgb.ktx2 | 37bcc85b33d499551b38a8437c815890be35d1f554241b62de3c73e54826bc25 |
| uastc-linear.ktx2 | 458b6478166a61ec38fe6cf275c62e177cc5a1fbba31f03a9e51f691424b8231 |
| uastc-zstd-srgb.ktx2 | 54802b1290d589b1031f5eba9ccab96ea83fdf13b9c7f8c868b7884855141582 |
| native-bc7-linear.ktx2 | b48f3891309f62a00101702161d62d345a288ae75af14094a7fc462fff271dcb |

All have dimensions 8x8, 4x4, 2x2, 1x1. All passed `ktx validate`.
Tests run without network or a generator. They decode BC7 with bc7decomp and
check this fixture's RGB/alpha errors, not a general texture quality guarantee.
