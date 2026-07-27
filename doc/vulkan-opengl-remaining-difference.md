# The remaining OpenGL/Vulkan difference

Where the two backends still disagree, how far it was chased, and what was ruled
out along the way. Self-contained: everything needed to pick this up is here.

Written after the work that closed the retro static flag, the target-dump frame
offset, the OpenGL Slang path, SSAO, SSR, bloom, cube-map mip chains, and the
IBL storage and irradiance-LOD parity fixes described below.

## The number

Frame 900, `--pbr 1 --dev 0 --fxaa 0 --sharpen 0 --anisofilter 0 --ssao 0 --ssr 0`,
Vulkan against OpenGL, mean absolute difference on RGB over the whole frame:

| scene | difference | pixels differing | of those, exactly one level |
|---|---|---|---|
| danm14ab | **0.005771** | 0.984% | 96.59% |
| tar_m02aa | **0.001807** | 0.345% | 93.21% |

`--pbr 0` in danm14ab is 0.015719 (1.953% differing pixels; 97.70%
within one RGB level).

Both scenes are now at the floor. Two different shader compilers cannot be
expected to round identically, and over 93% of tar_m02aa's differing pixels are
within one RGB level. The large tar_m02aa outlier is closed; a fresh danm14ab
capture confirms the fix improves both its PBR and retro paths.

## Measure it this way, or the number means nothing

Four things will silently produce a wrong figure. All four have already cost
this project real time.

**Pass `--anisofilter 0`.** The driver's anisotropic filtering differs between
its OpenGL and Vulkan paths on the same GPU. With it on, tar_m02aa reads 0.6671
instead of 0.0083 on the G-buffer diffuse target - the driver difference is
larger than everything else combined and swamps any measurement made through it.
Both backends request 16x and clamp identically, so this is not a configuration
mismatch and not something the engine can fix.

**Compare RGB, not RGBA.** Alpha is 255 on both, so including it divides every
figure by exactly four thirds.

**`--dumptargets` needs no `--capture` any more, but did.** Until the frame-flush
fix, a Vulkan dump read the image before the frame's command buffer was
submitted and returned frame N-1. Any `.npy` taken before that commit is one
frame stale.

**`--slangshaders` no longer exists.** It made OpenGL run Slang SPIR-V, which
silently dropped instanced geometry - grass and hair vanished. Comparisons made
with it on were comparing against an OpenGL that was not drawing the scene.

## IBL storage parity fix

OpenGL renders both derived IBL arrays into `RGB8` textures. Vulkan was using
`R16G16B16A16_SFLOAT`: it retained the convolution's fractional results while
OpenGL quantised after each render. The resolve therefore sampled a different
prefiltered value at every non-base roughness level.

Vulkan now uses `R8G8B8A8_UNORM` for its irradiance and prefiltered cube
arrays. On the exact frame above this changed the output from **0.09349** to
**0.08551**. The prefiltered maps then agreed to quantisation precision:

| derived target | previous mean RGB difference | current mean RGB difference |
|---|---:|---:|
| prefiltered mip 0 | 0.04819 | 0.00171 |
| prefiltered mip 1 | 0.09234 | 0.00006 |
| prefiltered mip 2 | 0.08790 | 0.00001 |
| prefiltered mip 3 | 0.08799 | 0.00000 |
| prefiltered mip 4 | 0.07581 | 0.00000 |
| irradiance | 0.12181 | 0.06120 |

The old source-cube hypothesis was tested directly. Levels 0 through 5 of the
active BC1 cube differ only by one RGB level, spread across their interiors
rather than concentrated at face edges. OpenGL also exposes a final 1×1 level
where Vulkan ends at 2×2, but adding an equivalent terminal level changed
neither the prefilter arrays nor the frame. It is not the remaining cause.

## The irradiance LOD mismatch that closed tar_m02aa

OpenGL's irradiance convolution uses implicit-LOD `texture(...)` lookups, whose
direction gradients choose the source mip. Vulkan forced every lookup to
`SampleLevel(..., 0.0)`. This does not affect prefiltering, which explicitly
chooses its own LOD, but it changed the cosine convolution enough to remain
visible on almost every reflective surface.

Vulkan now uses an implicit `Sample(...)` lookup for irradiance. Irradiance
difference fell from **0.06120** to **0.000003**, and the final tar_m02aa frame
from **0.08551** to **0.001807**. The remaining output difference is comparable
to the already-upstream G-buffer difference (**0.001943**), rather than a
distinct IBL problem.

The old source-cube hypothesis was also tested directly. Levels 0 through 5 of
the active BC1 cube differ only by one RGB level, spread across their interiors
rather than concentrated at face edges. OpenGL also exposes a final 1×1 level
where Vulkan ends at 2×2, but adding an equivalent terminal level changed
neither the derived maps nor the frame.

## What was ruled out, so it is not re-investigated

Each of these was tested rather than reasoned about, and each is a dead end.

- **The IBL convolution mathematics.** The prefilter pair has the same 1024
  samples, `resolution = 512.0`, GGX, Hammersley and mip-selection formula.
  The irradiance sampling-LOD difference was real and is fixed above.
- **Prefiltered array allocation and LOD mapping.** Both backends allocate five
  mips at 128/64/32/16/8, both write roughness as `mip / 4`, both resolve with
  `roughness * kMaxReflectionLOD`, and both now store the result as 8-bit
  normalised RGB(A). The previous half-float Vulkan allocation was a mismatch
  and is fixed.
- **Missing 2D texture mips in tar_m02aa.** The relevant environment sources are
  cube maps, so completing Vulkan's generated uncompressed 2D chains does not
  move this frame. The 2D upload path now nevertheless follows OpenGL's rule:
  generate a chain only when no authored chain is present.
- **DXT1 semantics.** OpenGL uses `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` and Vulkan
  `VK_FORMAT_BC1_RGBA_UNORM_BLOCK`, which differ on three-colour blocks. Switching
  Vulkan to `BC1_RGB` produced **bit-identical** output in all three scenes. The
  mismatch is real but does not manifest in this content.
- **Hashed alpha.** The integer hash matches line for line between
  `slang/lib/hashedalpha.slang` and `glsl/i_hash.glsl` since the commit that
  replaced the sine hash.
- **Sampler state.** Filters, wrap modes, LOD clamps and anisotropy are built
  from the same `Texture::Properties` on both sides.

## Smaller known differences, unrelated to the above

- **Resolve alpha.** OpenGL writes `fragColor` alpha as
  `step(0.0001, mainTexSample.a)`; Vulkan writes `1.0`. Invisible to an RGB
  comparison, but it feeds compositing.
- **Post-processing order in the retro pipeline.** OpenGL runs FXAA and sharpen
  before lens flares; Vulkan runs flares first, matching its own PBR ordering.
  Only visible with a filter enabled.
- **`retroAABBFragment`** exists in `slang/common.slang` but nothing selects it,
  and `aabbVertex` is not in the shaderpack entry list. Debug AABBs are not drawn
  on Vulkan at all.

## Tools

`--dumptargets <dir>` writes every exposed target as `.npy`. On the PBR path that
now includes the G-buffer, SSAO, SSR, the deferred highlights, the OIT pair, the
output, the irradiance array, the prefiltered array per mip, and every source
environment map that occupies a derived IBL layer. Cube arrays are
unrolled face after face, layer 0 +X through -Z first, with Vulkan's row order
normalised to OpenGL's so the two subtract directly. Source environment maps
are named `environment_map_layer<N>_mip<M>.npy`; BC1 and BC3 sources are decoded
to RGBA8 in the Vulkan dump so their texels compare directly with OpenGL.

Validate a single pass against **its own** backend's frame with the feature off,
never against the other backend - a pass inherits whatever difference preceded
it. That is what showed Vulkan's SSAO changing its own frame by 1.8691 over 68%
of pixels where OpenGL's changed 0.1106 over 12%, which no cross-backend figure
would have separated.
