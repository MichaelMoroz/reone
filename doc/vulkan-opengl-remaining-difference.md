# The remaining OpenGL/Vulkan difference

Where the two backends still disagree, how far it was chased, and what was ruled
out along the way. Self-contained: everything needed to pick this up is here.

Written after the work that closed the retro static flag, the target-dump frame
offset, the OpenGL Slang path, SSAO, SSR, bloom, and cube-map mip chains.

## The number

Frame 900, `--pbr 1 --dev 0 --fxaa 0 --sharpen 0 --anisofilter 0 --ssao 0 --ssr 0`,
Vulkan against OpenGL, mean absolute difference on RGB over the whole frame:

| scene | difference | pixels differing | of those, exactly one level |
|---|---|---|---|
| danm14ab | **0.00804** | 1.58% | 94% |
| tar_m02aa | **0.09349** | 13.22% | 77% |

`--pbr 0` in danm14ab is 0.01747.

danm14ab is at the floor. Two different shader compilers cannot be expected to
round identically, and 94% of what differs there differs by one least
significant bit. tar_m02aa is the outlier and is what this document is about.

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

## What the remaining tar_m02aa difference is

**It is on reflective surfaces, and it is in mip generation, not mip presence.**

Splitting the frame on the G-buffer's envmapped flag - bit 0 of the lightmap
alpha, see `unpackGeometryFeatures` in `slang/pbr_resolve.slang`:

| | pixels | difference | share of the total |
|---|---|---|---|
| envmapped | 81.3% | 0.11394 | **99.1%** |
| not envmapped | 18.7% | 0.00442 | 0.9% |

Not a brightness artefact: the non-envmapped group is the brighter of the two,
92.3 against 86.5, and restricting both to pixels above 32 does not move either
figure.

Binned by roughness, which is `clamp(mainTexSample.a, 0.2, 1.0)` and is exactly
what selects the prefiltered mip:

| roughness | n | difference |
|---|---|---|
| 0.2-0.3 | 602 | 0.08084 |
| 0.5-0.6 | 49561 | 0.03686 |
| 0.6-0.7 | 166916 | 0.03611 |
| 0.7-0.8 | 40396 | 0.03849 |
| 0.8-0.9 | 199073 | 0.07402 |
| 0.9-1.0 | 1229776 | **0.13657** |

Correlation with roughness is **+0.070**. Before cube mip chains were fixed it
was **-0.33** - the error used to be worst on mirrors and is now worst on rough
surfaces, which sample the coarsest levels at the far end of the chain.

The prefiltered array agrees with that: mip 0 differs by 0.0482 while mips 1
through 4 all sit near 0.08-0.09. Uniformly above the base level, and no longer
the 1.3 to 2.5 they were before.

## The hypothesis, and why it is not proven

OpenGL builds the source cube's chain with `glGenerateMipmap`, and has
`GL_TEXTURE_CUBE_MAP_SEAMLESS` enabled. Vulkan now builds it with successive
`vkCmdBlitImage` at `VK_FILTER_LINEAR`, which is inherently per-face.

Two ways those diverge, both compounding toward the small levels:

- **Face seams.** A driver's `glGenerateMipmap` on a seamless cube map may
  filter across face boundaries. A per-face blit clamps at the edge. Note that
  Vulkan *sampling* is seamless by specification - there is no toggle and none
  is needed. This is only about generation.
- **Accumulated rounding.** Each level is generated from the one before, so any
  per-level difference compounds. This matches the error being largest at the
  coarsest levels.

**Neither is confirmed.** The way to tell them apart is to dump the source
environment cube's own chain level by level on both backends and look at where
the error sits: concentrated at face seams means the first, spread uniformly
across each face means the second. An attempt at that plumbing was made and
abandoned - the Vulkan side emitted nothing, because the recorded derived source
was not a cube at dump time. That is the next step if this is picked up.

If it turns out to be seams, matching OpenGL means writing a seam-aware
downsample pass rather than blitting, to reproduce behaviour the GL driver does
not document. That is a lot of code for roughly 0.09 in one scene, which is why
it stops here rather than being closed.

## What was ruled out, so it is not re-investigated

Each of these was tested rather than reasoned about, and each is a dead end.

- **The IBL convolutions.** `prefilterFragment` in `slang/pbr_ibl.slang` and
  `glsl/f_pbr_prefilter.glsl` are character for character the same algorithm -
  same 1024 samples, same `resolution = 512.0`, same GGX, Hammersley and
  mip-selection formula. So are the irradiance pair.
- **Prefiltered array allocation and LOD mapping.** Both backends allocate five
  mips at 128/64/32/16/8, both write roughness as `mip / 4`, both resolve with
  `roughness * kMaxReflectionLOD`. No mismatch.
- **Missing 2D texture mips.** Suppressing `glGenerateMipmap` on OpenGL moved its
  output by exactly **0.0000**: the game's 2D textures are TPCs carrying authored
  chains that both backends upload. This says nothing about cube textures, which
  took a different upload path - that one was a real bug and is fixed.
- **DXT1 semantics.** OpenGL uses `GL_COMPRESSED_RGB_S3TC_DXT1_EXT` and Vulkan
  `VK_FORMAT_BC1_RGBA_UNORM_BLOCK`, which differ on three-colour blocks. Switching
  Vulkan to `BC1_RGB` produced **bit-identical** output in all three scenes. The
  mismatch is real but does not manifest in this content.
- **Hashed alpha.** The integer hash matches line for line between
  `slang/lib/hashedalpha.slang` and `glsl/i_hash.glsl` since the commit that
  replaced the sine hash.
- **Sampler state.** Filters, wrap modes, LOD clamps and anisotropy are built
  from the same `Texture::Properties` on both sides.
- **An intensity difference.** On envmapped pixels a best-fit multiplicative
  scale gives k = 1.0014 with residual 0.414, and a best-fit offset +0.120 with
  residual 0.414 - both worse than the uncorrected 0.337 measured at the time.
  Whatever this is, it is structured, not a gain.

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
output, the irradiance array, and the prefiltered array per mip. Cube arrays are
unrolled face after face, layer 0 +X through -Z first, with Vulkan's row order
normalised to OpenGL's so the two subtract directly.

Validate a single pass against **its own** backend's frame with the feature off,
never against the other backend - a pass inherits whatever difference preceded
it. That is what showed Vulkan's SSAO changing its own frame by 1.8691 over 68%
of pixels where OpenGL's changed 0.1106 over 12%, which no cross-backend figure
would have separated.
