# Porting weighted-blended OIT to the Vulkan backend

What OpenGL does, and what the Vulkan backend has to grow to match it. Written
from the OpenGL implementation rather than from the technique, because parity
with this renderer is the goal, not a good OIT.

## What OpenGL does

Two colour attachments, `src/libs/scene/render/pipeline/pbr.cpp:220-238`:

| target | format | holds |
|---|---|---|
| `cbTransparentGeometry1` | `RGBA16F` | rgb = weighted colour sum, a = revealage |
| `cbTransparentGeometry2` | `R16F` | r = weight sum |

`beginTransparentGeometryPass` (`pbr.cpp:532`) clears colour to `(0, 0, 0, 1)`,
pushes `BlendMode::OIT_Transparent`, and disables depth writes. Depth *test*
stays on, against the geometry pass's buffer.

The blend, `src/libs/graphics/context.cpp:428-431`:

```
glBlendEquationSeparate(GL_FUNC_ADD, GL_FUNC_ADD);
glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
```

One blend state for both attachments - **no `independentBlend` feature is
needed on the Vulkan side.** RGB accumulates additively; alpha multiplies down
by `(1 - srcAlpha)`, which is what turns attachment 0's alpha into revealage
given the clear of 1.0.

The resolve, `glsl/f_oit_blend.glsl`:

```glsl
vec3  accumColor  = oitAccumSample.rgb;
float accumWeight = oitRevealageSample.r;
float revealage   = oitAccumSample.a;
float alpha = 1.0 - revealage;
vec3 color = alpha * (accumColor / max(0.0001, accumWeight))
           + (1.0 - alpha) * (mainTexSample.rgb + hilightsSample.rgb);
fragColor = vec4(color, alpha + mainTexSample.a);
```

`sMainTex` is the deferred opaque resolve and `sHilights` is the bloom
highlights target. Driven from `blendTransparentGeometry`, `pbr.cpp:544`.

## What the Vulkan backend has today

`VulkanRenderPipeline::transparencyPass` (`pipeline/vulkan.cpp:557`) calls
`drawOntoOutput`, which renders transparent geometry straight into `_output`
with ordinary alpha blending. That is the deliberate earlier decision this
work reverses. Consequences visible in `danm14ab` frame 900: the saber region
differs from OpenGL by 8.09/255, the largest single disparity left.

## Steps

1. **Blend mode.** `BlendMode::OIT_Transparent` is unhandled in
   `src/libs/graphics/vulkan/pipeline.cpp:120-129` - it falls through to the
   default and silently blends normally. Add the case: `srcColor=ONE`,
   `dstColor=ONE`, `srcAlpha=ZERO`, `dstAlpha=ONE_MINUS_SRC_ALPHA`, both
   equations `ADD`. Apply the same state to every attachment, matching GL.

2. **Targets.** Two `VulkanImage`s beside `_ping` in the pipeline:
   `_oitAccum` at `VK_FORMAT_R16G16B16A16_SFLOAT` and `_oitRevealage` at
   `VK_FORMAT_R16_SFLOAT`, both `initColorAttachment` at target size, both
   given the ColorBuffer sampler from `VulkanSamplers` - see the assignments
   already made for `_output` and `_ping`, and note the trap that renderer-
   created images need their sampler set explicitly (commit e2cd6629).

3. **Transparency pass.** Rewrite `transparencyPass` so it does not use
   `drawOntoOutput`: two colour attachments, `loadOp` CLEAR with clear values
   `(0,0,0,1)` for accum and `(0,0,0,0)` for revealage, depth attachment loaded
   read-only as now, and the same flipped viewport. The `VulkanRenderPass`
   constructor takes the colour formats, so pass both.

4. **Resolve pass.** New pass between transparency and post-processing. Reads
   `_oitAccum`, `_oitRevealage` and `_output`; writes `_ping`; then `_ping` and
   `_output` swap, the same trick `filterChainPass` already uses. A fullscreen
   triangle, so reuse `postVertex` from `slang/postprocess.slang`.

5. **Shaders.** Port `glsl/f_oit_model.glsl` and `glsl/f_oit_particles.glsl` to
   Slang as additional entry points, and `f_oit_blend.glsl` as
   `oitBlendFragment`. Both OIT model shaders apply the legacy environment map
   the same way the opaque one now does - `env * (1 - alpha)` at
   `f_oit_model.glsl:70` - so whatever the opaque path settles on has to be
   mirrored here.

6. **Hilights.** `f_oit_blend.glsl` adds a bloom highlights target the Vulkan
   backend does not have. Add zero and leave a comment; it is real but belongs
   with the bloom port, not here.

## Verifying

`--dumptargets` exposes the scene targets, so accum and revealage should be
registered there too - being able to diff them against OpenGL's is the whole
reason the harness exists. The number that has to move is the saber crop
`(700..830, 1330..1620)`, 8.09/255 today, and the whole frame, 1.323 today.

Both backends must run with matching flags. Note that `--slangshaders`
defaults to false, so a run without it is not testing the Slang path at all.
