# Vulkan RT backend — architecture plan

Status: **design proposal, nothing implemented**. Written 2026-07-25.

Goal: a second rendering backend built on Vulkan with hardware ray tracing, Slang
shaders, and FSR upscaling, with real-time path tracing as the final target.

---

## 1. Current state of the graphics stack

### 1.1 Two kinds of code live in `src/libs/graphics`

**API-agnostic (roughly three quarters of the library, unaffected by this work):**
format readers (`mdlmdxreader`, `tpcreader`, `tgareader`, `bwmreader`,
`txireader`, `lipreader`), `model`, `modelnode`, `animation`, `keyframetrack`,
`walkmesh`, `aabb`, `camera`, `pixelutil`, `dxtutil`, `textureutil`, `font`,
`textutil`.

**OpenGL-concrete — 9 files, ~150 call sites:**

| File | `gl*` call sites |
| --- | --- |
| `src/libs/graphics/context.cpp` | 50 |
| `src/libs/graphics/mesh.cpp` | 36 |
| `src/libs/graphics/texture.cpp` | 24 |
| `src/libs/graphics/shaderprogram.cpp` | 10 |
| `src/libs/graphics/uniformbuffer.cpp` | 8 |
| `src/libs/graphics/framebuffer.cpp` | 8 |
| `src/libs/graphics/shader.cpp` | 6 |
| `src/libs/graphics/renderbuffer.cpp` | 5 |
| `src/libs/graphics/pbrtextures.cpp` | 3 |

Exactly one leak outside the library: `src/libs/scene/render/pipeline/retro.cpp`
includes glad directly.

GL handles are already fenced into explicit `// OpenGL … // END OpenGL` blocks
(`texture.h:167-172`, `mesh.h:277-283`). The surrounding classes hold only CPU
data — pixel layers, vertex layout, faces, AABB — and are portable as-is.

### 1.2 Seams that already exist and are worth keeping

- **`GraphicsServices`** (`include/reone/graphics/di/services.h:32`) is
  interface-only: `IContext`, `IMeshRegistry`, `IPBRTextures`,
  `IShaderRegistry`, `IStatistic`, `ITextureRegistry`, `IUniforms`. Downstream
  code already consumes abstractions.
- **`IRenderPipeline` / `IRenderPipelineFactory` / `RendererType{Retro,PBR}`**
  (`include/reone/scene/render/pipeline.h:55-77`), selected at
  `src/libs/scene/graph.cpp:458`. A proven frame-graph swap point — it already
  hosts two complete renderers.
- **`IRenderPass`** (`include/reone/scene/render/pass.h:69`). Critically,
  submission is *already deferred*: `SceneGraph::render()` registers callbacks
  via `inRenderPass(name, callback)` and the pipeline invokes them inside its own
  passes. That is the right shape for command-buffer recording.

### 1.3 The seam that is the wrong abstraction

`IContext` (`include/reone/graphics/context.h:45`) is an OpenGL state machine
wearing an interface: `pushDepthMask`, `popPolygonMode`, `blitFramebuffer`,
`bindTexture(unit)`, `withFaceCullMode`. None of that survives translation to
Vulkan, where state is baked into pipeline objects.

**Do not implement `IContext` in Vulkan.** Doing so leads to a per-draw
pipeline-state-object hash lookup emulating a state machine, which is the
classic failed port.

### 1.4 Other relevant facts

- **Shaders**: GLSL `#version 400 core`. `#include` is resolved by a hand-rolled
  regex at `src/libs/resource/provider/shaders.cpp:197`. `R_SSAO` / `R_SSR` are
  injected as defines. The `shaderpack` app packs `glsl/` into `shaderpack.erf`
  via the `create_shaderpack` custom target.
- **Uniforms**: 10 fixed UBO binding points
  (`include/reone/graphics/uniforms.h:27-37`); uniform blocks are bound *by name*
  (`shaderprogram.h:35`).
- **Window**: SDL3 with a hardcoded `SDL_GLContext` (`window.h:63`). Backend
  choice must therefore happen before window creation.
- **Toolkit** uses `wxGLCanvas` (`src/apps/toolkit/view/resource/modelpanel.cpp`)
  and calls `scene.render()` at
  `src/apps/toolkit/viewmodel/resource/model.cpp:113`.
- **`IStatistic`** (`statistic.h:24`) counts draw calls only. There is no GPU
  timing of any kind.
- **No previous-frame transform state exists anywhere** in `SceneNode` or
  `uniforms.h`.

---

## 2. Approach: Vulkan as a parallel backend

The Vulkan backend is a **sibling static library selected at startup**. The
existing OpenGL renderer is not refactored, ported, or wrapped in a new
abstraction layer. The shared surface between backends is the asset layer, scene
graph, animation, and material data — all of which are already API-neutral.

This works here for a specific reason that does not hold in most codebases:
`SceneGraph` hands the pipeline a *description* of the frame (a globals UBO plus
per-pass callbacks) rather than a stream of GL calls. A path-traced pipeline can
ignore `IRenderPass` almost entirely and walk the scene graph to build a TLAS
instead.

### 2.1 Layout

```
src/libs/graphics/          unchanged — GL backend plus all asset/format code
src/libs/graphics/vulkan/   NEW static library: device, swapchain, memory,
                            descriptors, acceleration structures, path tracer,
                            FSR integration
src/libs/scene/render/      gains pipeline/pathtraced.{h,cpp}
```

### 2.2 The one seam that must move

Today the frame output crosses the boundary as a GL texture handle and is
presented with GL (`src/libs/game/game.cpp:938-942`):

```cpp
auto &output = scene.render({w, h});
_services.graphics.uniforms.setLocals(...);
_services.graphics.context.useProgram(shaderRegistry.get(ShaderProgramId::ndcTexture));
_services.graphics.context.bindTexture(output);
_services.graphics.meshRegistry.get(MeshName::quadNDC).draw(statistic);
```

Replace with an `IRenderer` that owns the swapchain and presentation, so the
backend decides what a frame output is. The same change is needed at
`src/apps/toolkit/viewmodel/resource/model.cpp:113`.

### 2.3 Fate of the OpenGL backend

Two live backends is a real, ongoing cost. It taxes every interface decision:
`IRenderer` and the 2D batcher must be genuinely backend-neutral rather than
quietly Vulkan-shaped, and every scene-graph or material change has to be
verified against a GL path nobody is actively developing.

**Decision: freeze GL as maintenance-only, then retire it.**

- Effective immediately, the GL backend receives no new features. It only ever
  needs to do what the toolkit already does — model viewing. Game-side GUI paths
  in GL stay working but are not extended.
- The toolkit stays on `wxGLCanvas` for now, because wxWidgets 3.3 has no Vulkan
  canvas and writing `VkSurfaceKHR`-from-`HWND` glue (plus X11/Wayland) before
  any RT payoff is the wrong order of work.
- **Retirement path (phase 7, optional):** the toolkit is a model viewer and does
  not need a swapchain or high framerate. Render Vulkan offscreen, read back, and
  blit into a plain `wxPanel`. This needs no platform-specific surface code at
  all, and once it works the GL backend can be deleted outright.

---

## 3. The porting tax

A Vulkan backend cannot be "just the 3D renderer" — GL and Vulkan cannot share a
window. Everything the game draws must come along.

Direct `context.*` call counts by directory: scene 172 (replaced wholesale by the
new pipeline, so not a concern), game 27, engine 6, gui 5.

The immediate-mode 2D idiom (`context.useProgram(...)` followed by
`meshRegistry.get(quad).draw(...)`) appears at **19 sites**:

| Location | Sites |
| --- | --- |
| `src/libs/game/gui/selectoverlay.cpp` | 5 |
| `src/libs/game/gui/map.cpp` | 4 |
| `src/libs/game/game.cpp` | 2 |
| `src/libs/game/gui/actionslot.cpp` | 1 |
| `src/libs/game/gui/hud.cpp` | 1 |
| `src/libs/gui/control.cpp` | 1 |
| `src/libs/movie/movie.cpp` | 1 |
| `src/libs/graphics/cursor.cpp` | 1 |
| `src/libs/graphics/font.cpp` | 1 |
| `src/libs/graphics/pbrtextures.cpp` | 3 (internal IBL precompute — moves with the backend) |

So **16 sites** need the shared abstraction. That is the only place both backends
genuinely need a common API, and it defines the scope of the narrow RHI worth
building: **a 2D sprite/text batcher**, nothing more.

Also in scope, and easy to forget:

- **Text** — `src/libs/graphics/font.cpp:59-132`, instanced quads.
- **Movie playback** — `src/libs/movie/movie.cpp:93-103` uploads ffmpeg frames
  per-frame via `bindTexture` + `setPixels`.
- **Cursor**, minimap, action bar, profiler overlay.

---

## 4. Shaders — adopt Slang, ahead of Vulkan

Slang targets both SPIR-V and GLSL, so one source tree can feed both backends
during the transition. It also:

- replaces the regex `#include` preprocessor with a real module system;
- provides reflection, which removes the bind-uniform-blocks-by-name scheme;
- has first-class raytracing entry points (`[shader("raygeneration")]`,
  `[shader("closesthit")]`, `[shader("anyhit")]`);
- supports generics, which matters for a material system shared between raster
  and path tracing.

Plan: extend `shaderpack` to emit SPIR-V into a separate `shaderpack_vk.erf` at
build time; keep runtime Slang compilation behind `--dev` for iteration.

This step is independently valuable and can land before any Vulkan code exists.

---

## 5. Prerequisites specific to path tracing this engine

These are the real work — more so than the Vulkan plumbing.

### 5.1 Skinning must move to compute

Bones are applied in the vertex shader today (`glsl/u_bones.glsl`, `drawSkinned`
at `pass.h:78`). Ray tracing needs world-space vertices in memory to build and
refit a BLAS. The same applies to dangly meshes (`u_dangly.glsl`, `drawDangly`)
and sabers (`u_saber.glsl`).

Plan: compute skinning writes to an output buffer, then BLAS refit per frame per
skinned model.

### 5.2 Motion vectors do not exist

FSR and any temporal denoiser require per-pixel motion vectors, depth, and a
jittered projection. There is no previous-frame view/projection in
`GlobalUniforms` (`uniforms.h:67`) and no previous model matrix in
`LocalUniforms` (`uniforms.h:107`). Confirmed: no prev-frame state anywhere in
the scene node hierarchy.

Needs: per-node previous transforms, previous bone matrices for skinned meshes,
and a jitter sequence. Self-contained, and can be validated in the existing GL
PBR pipeline before Vulkan exists.

### 5.3 Lighting model — treat all albedo maps as albedo

**Decision: diffuse textures are used directly as albedo, and lightmap textures
are ignored entirely by the path tracer.** All lighting is computed for real.

Accepted consequence: KotOR diffuse textures frequently have lighting painted
into them, and that baked-in shading will remain visible on top of the traced
result. This is a known, accepted artifact for now — no de-lighting pass, no
attempt to recover clean albedo. Interiors in particular will not match the
original game's look.

Lighting inputs for the path tracer:

- emissive geometry derived from `selfIllumColor` (`material.h:47`);
- the gameplay point/directional lights in `_activeLights`
  (`graph.cpp:474-486`), promoted to area lights where a radius exists;
- ambient replaced by real global illumination.

The lightmap texture slot stays populated for the raster pipelines, which
continue to use it unchanged.

### 5.4 Material model is thin

`Material` (`material.h:36`) is Odyssey-era: diffuse/lightmap/envmap/bumpmap plus
ambient/diffuse/selfIllum colors. No metallic/roughness comes from game assets;
`PBRTextures` synthesizes IBL. A path-traced BSDF needs a real parameterization,
so expect a heuristic mapping layer.

### 5.5 Smaller items

- Alpha-tested foliage (`glsl/i_hashedalpha.glsl`, grass) needs any-hit shaders.
- Particles and emitters are best left rasterized and composited, not traced.
- `IStatistic` needs GPU timestamp queries added.

---

## 6. Dependencies

| Purpose | Choice | Note |
| --- | --- | --- |
| Loader | volk | avoids linking `vulkan-1` |
| Headers | vulkan-headers | vcpkg |
| Allocation | VMA | non-negotiable |
| Init | vk-bootstrap | optional; saves significant boilerplate |
| Shaders | shader-slang | in vcpkg; its reflection replaces spirv-reflect |
| Upscaler | FidelityFX FSR | native Vulkan backend |
| Denoiser | NRD, or hand-rolled SVGF/ReSTIR | |

Gate behind CMake options so default and CI builds are unaffected:

```cmake
option(ENABLE_VULKAN "build Vulkan backend" OFF)
option(ENABLE_FSR    "build FSR upscaler" OFF)
```

### 6.1 Licensing

reone is GPL-3 (`COPYING`), clean-room, explicitly non-commercial. FidelityFX /
FSR is permissively licensed (MIT) and can be linked directly with no GPL
conflict — *verify the current license text before committing, as these terms
change.*

Because FSR is the only upscaler, no dynamic-plugin indirection is needed. A thin
internal `IUpscaler` seam is still worth having so upscaling can be switched off
at runtime and so the denoiser can be swapped, but it is an ordinary interface
compiled into the backend, not a loadable module.

---

## 7. Phasing

Steps 1–3 are worth doing even if the Vulkan backend never lands. That is the
main argument for this ordering.

1. **Motion vectors and previous-frame transforms** in the existing GL PBR
   pipeline. Small, independently useful, de-risks the uniform plumbing.
   **Done.** `GlobalUniforms` carries unjittered current/previous view-projection
   plus the jitter offset, `LocalUniforms` a previous model matrix, and
   `BoneUniforms` a previous bone set. `SceneNode` latches its previous absolute
   transform once per frame at the end of `SceneGraph::render`. The PBR G-buffer
   gained an RG16F motion target at attachment 4, written by all four opaque-pass
   shaders. Jitter is behind `--taajitter`, off by default until something
   resolves it. Nothing consumes the motion target yet.
2. **Slang migration** of the existing GLSL, still targeting GL. Leaves one
   shader source tree for both backends.
3. **`IRenderer` seam** — move presentation out of `game.cpp` and the toolkit;
   add the 2D batcher abstraction covering the 16 sites in §3.
4. **Vulkan raster backend** to PBR parity. Unglamorous but mandatory: swapchain,
   descriptor management, GUI, text, movie playback.
5. **Acceleration structures and hybrid RT** — compute skinning, BLAS/TLAS, then
   RT shadows/AO/reflections replacing the current SSAO and SSR passes. First
   visible payoff.
6. **Path tracing** as `RendererType::PathTraced`, plus denoiser and FSR.
7. *(Optional)* **Retire OpenGL** — move the toolkit to offscreen Vulkan with
   readback into a plain `wxPanel`, then delete the GL backend.

---

## 8. Open questions

- Should the 2D batcher be broader than a sprite/text batcher? Current scope is
  16 call sites; anything wider needs justification.
- Denoiser choice (NRD vs hand-rolled SVGF/ReSTIR) — defer until phase 5 gives
  real ray-traced input to evaluate against.
- Whether the accepted baked-in-lighting artifact (§5.3) is tolerable in practice,
  or whether a de-lighting pass becomes necessary after seeing phase 6 output.
