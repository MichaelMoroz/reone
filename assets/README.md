# Runtime assets

Data the engine ships itself, as opposed to anything read out of a KotOR
installation. Resolved the same way `slang/` is: a copy beside the executable
wins, otherwise the tree named by `REONE_ASSET_SOURCE_DIR` at build time.

## bluenoise_rgba_64x64x64.tga

Sixty-four independent 64x64 blue-noise tiles, RGBA, packed into one 512x512
image as an 8x8 grid. Tile `n` occupies `(n % 8, n / 8) * 64`.

From Christoph Peters' free blue noise textures
(<https://momentsingraphics.de/BlueNoise.html>), mirrored at
<https://github.com/Calinou/free-blue-noise-textures> as `64_64/HDR_RGBA_*.png`.
Peters placed them in the public domain via CC0
(<https://creativecommons.org/publicdomain/zero/1.0/>), so they carry no
obligation into this GPL-3 tree. Repacking into a single TGA is the only change:
each tile round-trips byte-for-byte out of the atlas.

Measured on the set as shipped: high-frequency energy 0.995 of total against
0.813 for white noise of the same size, so the spectrum is what it claims to be;
channel-to-channel correlation within a tile at most 0.022, so R and G serve as
an independent pair; tile-to-tile correlation 0.025, so consecutive frames drawn
from consecutive tiles are independent fields.

That last property is the reason there are sixty-four of them rather than one.
Animating a single noise field by adding a per-frame offset to its *value*
shifts every pixel by the same amount, which translates the pattern rigidly
instead of replacing it - on interleaved gradient noise, whose field is a set of
diagonal stripes, that is seen directly as the stripes marching across a
penumbra. Cycling through independent tiles has no such structure to march.
Sixty-four frames is longer than a temporal resolve accumulates over, so a pixel
does not see the same field twice inside one accumulation window.

TGA rather than PNG because the engine has a TGA reader and no PNG decoder;
adding one for a single texture was not worth the dependency.

## reone.rc, toolkit.rc, reone.ico, toolkit.ico, icons.xcf

Build inputs, not runtime assets, and not resolved by the rule above. The `.rc`
files are Windows resource scripts, each naming the matching `.ico`; the build
appends one to a target's sources only under `WIN32` — `reone.rc` to `engine`
(`src/apps/engine/CMakeLists.txt:35-36`) and `launcher`
(`src/apps/launcher/CMakeLists.txt:26-27`), `toolkit.rc` to `toolkit`
(`src/apps/toolkit/CMakeLists.txt:68-69`). The icons are therefore linked into
the executables, never read from disk. `icons.xcf` is the GIMP artwork the two
were exported from; nothing in the build refers to it.
