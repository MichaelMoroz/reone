# Vulkan RT backend — architecture plan

Status: **phase 1 landed, the rest is proposal.** Written 2026-07-25, revised
2026-07-26 after the Slang spike (§4.1).

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

**Done.** `include/reone/graphics/renderer.h` defines the interface;
`renderer/gl.h` implements it over the default framebuffer. A frame is
`beginFrame(extent)` / drawing / `endFrame()`, with `drawSceneOutput(Texture &)`
as the hand-off from a pipeline, which produces a texture, to the backend, which
decides how that texture becomes visible. Both call sites above now go through
it, as does the engine's frame loop and the screenshot path.

Two details are load-bearing for Vulkan:

- **`captureFrame()` must precede `endFrame()`.** In GL that is only a
  convention; in Vulkan the contents of a presented swapchain image are
  undefined, and the readback has to be recorded before the present.
- **`beginFrame` pushes the viewport rather than assuming it.** Dear ImGui calls
  `glViewport` directly, behind the context's back, so nothing may rely on
  leftover state from the previous frame.

The renderer takes an optional `Window`. Null means it does not own presentation
and must not swap — the toolkit draws into a canvas wxWidgets presents itself.
That distinction disappears with the GL backend, but until then it is what keeps
one interface serving both hosts.

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

**Done.** `include/reone/graphics/renderer2d.h` defines `I2DRenderer`;
`renderer/gl2d.h` implements it. Five operations cover every site: `drawImage`
by rect or by transform, `drawRect`, `drawFullTargetImage`, `drawText`, with
`withBlendMode` and `withScissor` as scopes. The transform overload exists for
exactly one caller - the minimap arrow, which rotates - and the full-target one
for movie frames.

Three consequences went further than the site count suggested:

- **`Font` no longer draws.** It keeps the atlas and the metrics and exposes its
  glyphs; submitting them is the renderer's business. It dropped five graphics
  services for one, and so did `Fonts` and `Cursors`.
- **`IRenderPass::drawImage` is gone.** It was the *only* member the whole GUI
  layer used - 26 calls, no others - so `IRenderPass` left the GUI entirely,
  along with the `pass` parameter threaded through every control's `render`.
  `IRenderPass` is now purely a scene-geometry pass, which is what it should
  always have been.
- **Billboarded debug text** in `scene/drawdebug.cpp` used `Font::renderLine`
  but is a world-space draw with its own program, so it now fills the glyph
  block itself. It also silently truncates past `kMaxTextChars` rather than
  overrunning the block, which the old path did not check.

Also in scope, and easy to forget:

- **Text** — `src/libs/graphics/font.cpp:59-132`, instanced quads.
- **Movie playback** — `src/libs/movie/movie.cpp:93-103` uploads ffmpeg frames
  per-frame via `bindTexture` + `setPixels`.
- **Cursor**, minimap, action bar, profiler overlay.

### 3.1 The uniform update model

Not incidental plumbing: this is a rewrite of how every `IRenderPass::draw*`
submits data, and it is more tightly coupled to the compute-skinning work in
§5.1 than it first appears.

Today `Uniforms` (`src/libs/graphics/uniforms.cpp`) owns nine `UniformBuffer`s
bound to fixed indices from `UniformBlockBindingPoints`. Each setter mutates a
CPU-side mirror struct and re-uploads the **whole block**:

```cpp
void Uniforms::setGlobals(const std::function<void(GlobalUniforms &)> &block) {
    block(_globals);
    _context.bindUniformBuffer(*_ubGlobals, UniformBlockBindingPoints::globals);
    _ubGlobals->setData(&_globals, sizeof(GlobalUniforms)); // glBufferSubData
}
```

Three properties of that do not survive the move:

- **Per-draw whole-buffer overwrite.** `setLocals` runs on every draw, and
  `BoneUniforms` is 3 KB per skinned draw since previous-frame bones were added.
  Overwriting a buffer that queued draws still reference is legal in OpenGL only
  because the driver renames behind the caller. Under recorded command buffers it
  is simply invalid, and has to become bump-allocated slices of a per-frame buffer
  addressed by dynamic offset.
- **Binding uniform blocks by name.** `bindUniformBlock("Globals", 0)` has no
  Vulkan equivalent, and fails silently on a miss (see §4.1).
- **Loose uniforms.** Seven live call sites set uniforms outside any block:
  `uSaberDisplacement` (pass/pbr.cpp:213, retro.cpp:203), `uCorners`
  (pbr.cpp:327, retro.cpp:296, drawdebug.cpp ×2), `uEnvMapDerivedLayer`
  (pbr.cpp:74), `uRoughness` (pbrtextures.cpp:195). Vulkan has no loose uniforms.
  The small scalars belong in push constants; the per-draw values belong in the
  per-draw record. A `saber` binding point already exists at index 4, declared and
  unused, so the intent predates this document.

Target shape for the raster path: descriptor sets grouped by update frequency -
per frame, per pass, per material, per draw - with the per-draw set addressed by
dynamic offset into a ring buffer.

**Do not over-invest here.** A path tracer does not bind per draw at all (§5.6),
so most of a beautiful per-draw binding scheme is superseded in phase 6. Design
the material representation once, GPU-resident and index-addressed, and let the
raster path index into it too.

---

## 4. Shaders — Slang, at phase 4, targeting SPIR-V only

This section originally proposed migrating the existing GLSL to Slang *ahead* of
Vulkan, on the theory that one source tree could feed both backends. A spike
against slangc 2026.7.1 showed that does not hold for this engine. Revised
position: **Slang enters with the Vulkan backend and targets SPIR-V, its
first-class output. The frozen GL backend keeps its hand-written GLSL until it is
deleted.**

Slang is still the right choice when it arrives:

- a real module system in place of the regex `#include` preprocessor
  (`src/libs/resource/provider/shaders.cpp:197`);
- reflection, which generates the binding tables and the std140 layout contract
  rather than leaving them hand-maintained (§4.2);
- first-class raytracing entry points (`[shader("raygeneration")]`,
  `[shader("closesthit")]`, `[shader("anyhit")]`);
- generics, which matter for a material system shared between raster and path
  tracing.

### 4.1 Why not Slang → GLSL

Verified by compiling representative shaders with `slangc -target glsl`:

**Works.** `[[vk::location(N)]]` emits exactly `layout(location = N)`, preserving
the engine's vertex layouts including deliberately skipped slots. `noperspective`
survives. Varying names are mangled but match by location across stages, so that
is harmless.

**Does not work.**

| Emitted | Consequence |
| --- | --- |
| `block_SLANG_ParameterGroup_ScreenEffect_0` | `bindUniformBlock("ScreenEffect", ...)` misses - and returns silently, so the result is garbage uniforms with no error |
| `sMainTex_0` | `setUniform("sMainTex", unit)` misses |
| Loose uniforms gathered into a `GlobalParams` block | `uSaberDisplacement`, `uCorners`, `uEnvMapDerivedLayer` stop being settable by name |
| `#version 450`, regardless of `-profile glsl_400` | collides with the `#version 400 core` the shader provider prepends |
| `layout(binding = N)` on blocks and samplers | GLSL 420+ only. The engine creates a **GL 4.0 core** context (`window.cpp:33-35`) |

Reflection (§4.2) answers the naming rows - the host can bind from reflection
data instead of names. It does not answer the GL version floor. Adopting Slang on
the GL path therefore means raising the context to 4.5 and migrating the binding
model, on a backend §2.3 has already committed to retiring. The payoff would die
with the backend.

Also note `-matrix-layout-column-major` emits GLSL `layout(row_major)`, and the
row-major flag emits `column_major`. Slang's convention is transposed relative to
GLSL's. Whichever target is used, this must be validated numerically against a
known transform rather than reasoned about.

### 4.2 Reflection is the real prize

`slangc -reflection-json` reports source-level names, assigned binding indices,
and the complete std140 offset table:

```
ScreenEffect  kind=constantBuffer  binding=descriptorTableSlot index=1
sMainTex      kind=resource        binding=descriptorTableSlot index=2
uProjection      offset=0     size=64
uSSAOSamples     offset=192   size=1024
uSharpenAmount   offset=1268  size=4
```

Those offsets match `ScreenEffectUniforms` in `include/reone/graphics/uniforms.h`
byte for byte.

That matters because the contract between the nine `layout(std140)` blocks in
`glsl/u_*.glsl` and the fourteen C++ structs in `uniforms.h` is currently
maintained **by hand and checked by nothing**. Four of those structs are held in
place by hand-written `alignas(16)` derived from std140's struct alignment rule.
A mismatch renders garbage silently. Reflection makes that contract machine
checkable, and in the Vulkan backend it also generates descriptor set layouts.

### 4.3 Closing the std140 hazard before then

The desync hazard is present today and does not need Slang to address. OpenGL
already knows the true layout of every linked program: `glGetUniformIndices` plus
`glGetActiveUniformsiv` with `GL_UNIFORM_OFFSET`, `GL_UNIFORM_ARRAY_STRIDE` and
`GL_UNIFORM_MATRIX_STRIDE` yield the driver's offsets, which can be compared
against `offsetof` in a debug-build validation pass after link.

That is roughly an hour of work, needs no new dependency, and validates what the
driver actually did rather than what a tool predicts. Preferred over pulling
Slang forward purely to catch layout drift.

---

## 5. Prerequisites specific to path tracing this engine

These are the real work — more so than the Vulkan plumbing.

### 5.1 Anything the vertex shader synthesises must become real geometry

Ray tracing cannot see vertex shaders. Whatever the raster path computes on the
fly has to physically exist in a BLAS, which makes three current features the
same problem wearing different names:

| Feature | Synthesised by | Driven by |
| --- | --- | --- |
| Skinning | `u_bones.glsl`, `drawSkinned` | `BoneUniforms` |
| Dangly meshes | `u_dangly.glsl`, `drawDangly` | `DanglyUniforms` |
| Lightsaber blades | `u_saber.glsl`, `drawSaber` | loose `uSaberDisplacement` |

Plan: a compute pass consumes those inputs and writes deformed world-space
vertices to a buffer, followed by a per-frame BLAS refit for each affected model.

Note the coupling with §3.1: all three inputs arrive today through the per-draw
uniform path, and two of them are among the loose uniforms that Vulkan cannot
express. On the path-traced side they stop being shader inputs altogether - the
compute pass consumes them and the closest-hit shader reads plain geometry - so
they should not be carefully ported into a per-draw binding scheme that phase 6
then discards.

### 5.2 Motion vectors — done, conventions unsettled

FSR and any temporal denoiser require per-pixel motion vectors, depth, and a
jittered projection. None of that existed; phase 1 added it. `GlobalUniforms`
carries unjittered current and previous view-projection plus the jitter offset,
`LocalUniforms` a previous model matrix, `BoneUniforms` a previous bone set, and
`SceneNode` latches its previous absolute transform once per frame. The PBR
G-buffer writes an RG16F motion target from all four opaque-pass shaders.

Two things remain open, both recorded in §8: the sign and axis conventions were
chosen arbitrarily because nothing consumes the buffer yet, and the values have
been eyeballed but never numerically verified.

Note the deliberate limits. Dangly and saber meshes deform per-vertex with no
previous vertex positions tracked, so their vectors capture rigid motion only,
and grass billboards ignore their camera-facing re-orientation. §5.1 removes both
limitations as a side effect of moving deformation into compute.

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

### 5.6 The scene has to be GPU-resident

A path tracer has no draws, so there is nothing to bind per draw. A ray may hit
any surface, which means the whole scene must be addressable before tracing
starts:

- one global vertex/index buffer with meshes suballocated into it;
- one material buffer, indexed by instance;
- **bindless textures** - a descriptor-indexed array sampled by index taken from
  the hit record;
- TLAS instances carrying that material index.

This is the endpoint the per-draw uniform work in §3.1 should be aimed at rather
than away from: the material representation wants to be designed once,
GPU-resident and index-addressed, with the raster path indexing into the same
buffer instead of binding a per-material descriptor set.

`SceneGraph::refresh()` already flattens the node tree into typed arrays every
frame, which is the natural hook for building TLAS instances. One caveat: those
arrays are rebuilt and reordered per frame, so a BLAS instance cache needs a
stable per-instance identity that does not exist yet.

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
   resolves it. Nothing consumes the motion target yet, though the render target
   viewer added to the ImGui editor can display it.
2. **std140 layout validation** (§4.3). An hour, no new dependency, and it closes
   a live silent-corruption hazard on code already committed.
3. **`IRenderer` seam** — move presentation out of `game.cpp` and the toolkit;
   add the 2D batcher abstraction covering the 16 sites in §3. **Done** —
   presentation in §2.2, the 2D renderer in §3. Nothing outside the graphics
   library names a shader program or a quad mesh any more.
4. **Vulkan raster backend** to PBR parity. Unglamorous but mandatory: swapchain,
   descriptor management, GUI, text, movie playback, and the uniform update model
   in §3.1. **Slang enters here**, targeting SPIR-V (§4).

   **In progress — the spine exists, nothing is connected to the game yet.**
   Device, swapchain, frames in flight, presentation, screenshot readback,
   buffers, the uniform ring of §3.1, both descriptor sets, pipelines, images and
   samplers, `Mesh` upload, depth, and a five-attachment G-buffer with a deferred
   resolve. All of it exercised by `vulkanprobe` with the validation layers on
   and silent. See §10.

   **Not started:** anything that touches game data. `MeshRegistry`, `Texture`
   and `Material` do not reach this path; the geometry is a synthesised cube and
   the texture a generated checkerboard. Also missing: a pipeline cache, the
   lighting the resolve stands in for, shadows, transparency, particles, GUI,
   text and movie playback.
5. **Acceleration structures and hybrid RT** — compute skinning, BLAS/TLAS, then
   RT shadows/AO/reflections replacing the current SSAO and SSR passes. First
   visible payoff.
6. **Path tracing** as `RendererType::PathTraced`, plus denoiser and FSR.
7. *(Optional)* **Retire OpenGL** — move the toolkit to offscreen Vulkan with
   readback into a plain `wxPanel`, then delete the GL backend.

---

## 8. Open questions

- ~~Should the 2D batcher be broader than a sprite/text batcher?~~ Answered by
  building it: five operations covered all 16 sites plus the 26 that already
  went through `IRenderPass::drawImage`. Nothing wanted more.
- **A reference worktree exists** at `C:/Development/reone-ref`, a second
  checkout with its own build tree, so a comparison build never disturbs the
  working one. Configure it with the same vcpkg toolchain but only the `engine`
  target, and copy `reone.cfg` into its `bin` - without it the settings differ
  and the diff is meaningless.
- **The scene is not frame-deterministic.** Partly fixed, not solved. Two runs
  of the same build, stopped at the same frame, still differ across roughly a
  third of a gameplay frame. GUI frames are bit-identical, so the harness is
  trustworthy for 2D but currently proves nothing automatic about the 3D scene.
  This has to be closed before GL-and-Vulkan parity can be checked
  automatically, which is the whole point of having the harness.

  **Fixed:** the shared random generator was seeded from `time(nullptr)`. Grass
  variants, particle emitters and the SSAO noise texture all draw from it, so
  every run laid the world out differently. It is seedable now, and a capture
  run seeds it deterministically. This removed all the *sharp* differences: peak
  per-pixel error fell from 246 to 64.

  **What remains** is diffuse and low-amplitude - mean error 3, concentrated in
  sky and distant terrain, near geometry almost untouched - and it is *bimodal*:
  any two runs either agree to within 1% of pixels or differ across 33%, with
  nothing in between.

  Ruled out, each by measurement rather than reading:
  - *RNG divergence.* Draw counts are identical between runs (7381 both times),
    so the generator is consumed in the same order and produces the same values.
  - *Frame timing.* The capture path already forces a fixed 1/60 timestep.
  - *SSAO or SSR.* Disabling either appeared to fix it, then the result flipped
    on repeat - the low-variance outcome shows up in about half of all runs
    whatever the flags say. The first reading was an artifact of the bimodality.
  - *An off-by-one frame.* Comparing a reference frame 600 against runs stopped
    at 599 and 601 is worse than against 600, so the state is not simply shifted.
  - *Threaded loading.* There is none in `resource`, `graphics` or `scene`.

  The bimodality is the strongest clue: something settles into one of two states
  early and stays there. Worth checking next: whether the number of `update`
  calls before the capture frame is constant, and whether any pass samples a
  render target that is never cleared.
- Denoiser choice (NRD vs hand-rolled SVGF/ReSTIR) — defer until phase 5 gives
  real ray-traced input to evaluate against.
- Whether the accepted baked-in-lighting artifact (§5.3) is tolerable in practice,
  or whether a de-lighting pass becomes necessary after seeing phase 6 output.
- Motion vector conventions are unsettled. `i_motion.glsl` currently emits
  `0.5 * (cur - prev)` in UV units, y-up: the vector points where the surface
  went. Temporal resolves generally want the reprojection vector, `prev - cur`,
  y-down. Nothing reads the buffer yet, so neither is wrong - but both should be
  fixed deliberately when FSR arrives rather than discovered then.
- Whether the motion vectors are numerically correct is still unverified. They
  have been eyeballed through the render target viewer and look plausible; a
  known-rotation test against expected pixel displacement would settle it.
- Stable per-instance identity across frames, for a BLAS cache (§5.6).

---

## 9. Shader architecture

The shaders are being rewritten rather than transliterated. A mechanical port was
taken far enough to prove the toolchain (§4.1) and is kept as reference, but the
existing shaders were designed against name-based stage linking and a runtime
feature mask, and neither survives the destination.

Rewriting happens against OpenGL, where each shader can be validated visually as
it lands. The Vulkan-shaped decisions - explicit bindings, uniform blocks, no
loose uniforms, no by-name lookup - are already made, so the remaining delta to
SPIR-V is declarations rather than logic: descriptor set indices, push constants,
and replacing the geometry-shader shadow path.

### 9.1 Share code, not entry points

`v_model` is one entry point serving three programs, `v_passthrough` serves nine.
That is why porting `f_texture` forced two unrelated vertex stages onto a common
varyings layout: GLSL matched stage interfaces by name, so the sharing was free;
explicit locations make every fragment stage constrain every vertex stage it
pairs with.

Each program therefore gets its own entry point, and shared work becomes shared
*functions* in modules. Slang makes that free, and the coupling disappears.

### 9.2 Specialise geometry, branch on material

The fifteen feature flags are evaluated per vertex and per fragment. Two reasons
to split them rather than keep branching:

- the skinned, dangly and saber paths have to become compute passes writing real
  geometry (§5.1), so they want to be separate pipelines, not branches;
- a path tracer has no vertex stage at all, so anything the vertex shader
  synthesises has to be resolved before tracing.

So the **geometry** path is specialised - static, skinned, dangly and saber become
distinct entry points over shared transform functions - while **material**
features stay runtime branches. Specialising both would multiply into a
permutation explosion for little gain, since the material branches are uniform
across a draw and predict well.

### 9.3 One surface description, two consumers

The raster resolve and the path tracer's closest-hit shader need the same thing:
a surface's shading parameters at a point. Today each shader re-derives them from
Odyssey texture slots in its own way.

Instead a single `SurfaceParams` - albedo, normal, roughness, metallic, emissive,
alpha - is produced by one function and consumed by a BSDF that neither knows nor
cares which renderer called it.

The mapping from Odyssey slots to those parameters wants to move out of the
shader entirely, into the material buffer described in §5.6, computed once when
the material is built rather than per fragment. The shader-side split is written
now so that the move is a change of where `SurfaceParams` comes from, not a
rewrite of everything that uses it.

### 9.4 Cross-stage uniform block naming

Slang emits only the uniform blocks a given entry point references, and
disambiguates identifiers across whatever it emitted. Two stages of one program
that touch different sets of blocks therefore get different member names for the
blocks they share, and OpenGL links interface block members by name.

Concretely, for the grass program:

| Stage | Blocks emitted | `GlobalUniformsLight` |
| --- | --- | --- |
| vertex | Grass, Globals | `vec4 color_0;` |
| fragment | Grass, **Locals**, Globals | `vec4 color_1;` |

The fragment reads `localUniforms.featureMask` through `isFeatureEnabled`, so
`LocalUniforms::color` takes the unsuffixed name and the light's colour is pushed
to `_1`. Linking fails with "struct fields mismatch between shaders".

The opaque model program is unaffected only by luck - both of its stages happen to
reference Locals.

Compiling every entry point of a program in one slangc invocation would fix it,
but GLSL is a single-entry-point target and slangc rejects multiple `-o` options
for it. Three ways out:

1. **Make member names globally unique**, so disambiguation never applies. The
   collisions are all in the structs nested inside blocks - `color` appears in
   `GlobalUniformsLight`, `LocalUniforms` and `ParticleUniformsParticle`, `radius`
   in `GlobalUniformsLight` and `GrassUniforms`. Renaming those means renaming the
   C++ members too, since the layout assertions map the two by identity.
2. **Force every stage to reference every block**, which is brittle and relies on
   the optimiser not removing the reference.
3. **Rewrite the identifiers after transpiling**, which is fragile.

The first is the only principled option, and it is one more reason the transitional
Slang-to-GLSL path costs more than Slang-to-SPIR-V will: SPIR-V binds by number
and has no cross-stage name matching at all.

### 9.5 Slang's SPIR-V uses Vulkan builtins that OpenGL ignores

`SV_InstanceID` lowers to `InstanceIndex - BaseInstance` and `SV_VertexID` to
`VertexIndex`, under `OpCapability DrawParameters`. Those are Vulkan builtins.
The OpenGL SPIR-V environment uses `InstanceId` and `VertexId` instead.

OpenGL accepts the module anyway - glSpecializeShader reports success - and the
builtin reads zero. Grass therefore drew all 256 instances on top of cluster 0,
which happens to sit behind nearer terrain, so nothing appeared at all. The
symptom looks like missing geometry rather than a wrong index, and every probe
that assumed the geometry was misplaced came back negative.

It was found by reading the vertex output in RenderDoc: instance 13 reported a
world position derived from cluster 0.

The same applies to any shader reading a vertex or instance id, which currently
means the dangly and saber geometry paths as well as grass. The static and
skinned paths are unaffected because they never read one.

Adding `SV_StartInstanceLocation` back cancels the subtraction, but does not help
when the underlying builtin is itself zero.

Three ways out:

1. **Supply the index as an instanced vertex attribute** with divisor 1. Portable
   and works on both backends, but only solves the instance id, not the vertex id.
2. **Compile Slang to GLSL, then GLSL to SPIR-V with glslang `-G`**, which emits
   the OpenGL builtins. The cross-stage naming problem of section 9.4 does not
   apply, because the result still binds by number. Adds a pipeline stage.
3. **Rewrite the builtin decorations in the emitted SPIR-V.** Mechanical but
   fragile.

Option 2 is the most promising: it keeps one shader source, needs no engine
change, and the intermediate GLSL is already known to be correct.

### 9.6 Decision: stop running Slang on OpenGL

Every obstacle to running the rewritten shaders on OpenGL has been a mismatch
between what Slang emits and what OpenGL accepts - cross-stage identifier naming
in transpiled GLSL (9.4), Vulkan-only builtins in GLSL, and Vulkan-only builtins
in SPIR-V that OpenGL silently reads as zero (9.5). The shaders themselves have
been correct Vulkan throughout. Each workaround is deleted when the OpenGL
backend is.

So the Slang shaders now target Vulkan only, and the OpenGL backend goes back to
its hand-written GLSL, unchanged and frozen. It stops being something to fight
and returns to being the reference the Vulkan output is compared against.

The comparison survives the move: the capture harness screenshots whatever
backend is running, so GL against Vulkan is the same A/B as before.

What carries forward unchanged: the uniform blocks and their layout assertions,
every Slang shader written so far, the capture harness, and the RenderDoc
workflow. What is removed: the SPIR-V loading path in the OpenGL backend, the
--slangshaders toggle, and the shader registry's variant selection.

The cost is that nothing renders until a good deal of phase 4 exists, which was
the original argument for doing shaders on OpenGL first. That argument has
weakened now the shaders are written and known to be Vulkan-shaped - what would
have been guesswork no longer is.

---

## 10. Vulkan backend progress

**Status:** the backend can open a window, build a device, upload geometry and
textures, and render a depth-tested, textured mesh through a G-buffer and a
deferred resolve, validation-clean. It is driven by `vulkanprobe`, not by the
engine: no game asset has been through it, and the engine still runs entirely on
OpenGL. The sections below are in the order the work happened, and record the
traps as much as the results.

### 10.1 Standing up the device

`src/libs/graphics/vulkan/` builds as `graphicsvulkan`, gated behind
`ENABLE_VULKAN` (default OFF, so ordinary builds are untouched). Dependencies
are the ones §6 chose, all from vcpkg: volk, vulkan-headers, VMA, vk-bootstrap.
SDL3 had to be reinstalled as `sdl3[core,vulkan]` — the default port has no
Vulkan support and `SDL_CreateWindow` refuses `SDL_WINDOW_VULKAN` without it.

`VulkanDevice` is instance, surface, physical device, logical device, queues and
the VMA allocator. `VulkanSwapchain` is the part that gets rebuilt on resize.
`VulkanRenderer` implements the same `IRenderer` as the GL backend: acquire,
record, submit, present, with two frames in flight.

`captureFrame` works, so a Vulkan frame goes through the same screenshot path as
a GL one. It cuts the frame in two — submits what has been recorded, waits,
reads the staging buffer, then reopens a command buffer for `endFrame` — which
stalls hard and is why it stays a screenshot path.

Verified: 300 frames presented with the validation layers on and no messages,
and a captured frame is a single uniform colour matching the requested clear
exactly.

### 10.2 Three things that cost time, recorded so they do not cost it twice

- **VMA and volk.** With `VMA_STATIC_VULKAN_FUNCTIONS=0` *and*
  `VMA_DYNAMIC_VULKAN_FUNCTIONS=0`, VMA expects every entry point supplied by
  hand and otherwise calls through null pointers — it segfaults inside
  `vmaCreateAllocator`, before any allocation, with no diagnostic. The dynamic
  path must be on so VMA can fetch what it needs from the two getters it is
  given. The macros belong in CMake, not in the implementation file, so every
  translation unit including the header agrees with the one defining it.
- **The present semaphore is per image, not per frame in flight.** Presentation
  consumes it against a particular swapchain image and gives no signal that it
  has, so a per-frame semaphore can be re-signalled while a present is still
  pending on it. The image count need not equal the in-flight count either.
  Validation catches this immediately (`VUID-vkQueueSubmit2-semaphore-03868`).
- **Swapchain images need `TRANSFER_DST` as well as `TRANSFER_SRC`** — SRC for
  the screenshot readback, DST for `vkCmdClearColorImage`.

### 10.3 Why there is a probe application

`src/apps/vulkanprobe/` is a small host that opens a window and drives the
backend directly. The engine cannot select a backend yet: GL and Vulkan cannot
share a window, and every other subsystem still talks to the GL context, so
pointing the engine at Vulkan today means it dies in module init rather than
telling us anything about the backend.

It is scaffolding and should be deleted once the engine can switch.

### 10.4 Buffers, the uniform ring, descriptors and a first draw

`VulkanBuffer` wraps a VMA allocation in the two shapes that matter: host-visible
and permanently mapped, for data rewritten every frame; and device-local filled
once through a staging copy, for vertices and indices. `VulkanDevice` grew
`immediateSubmit` for the load-time work those copies need.

`VulkanUniformRing` is the §3.1 replacement for the OpenGL update model. One
host-visible arena per frame in flight, bump-allocated, each draw taking its own
slice addressed by a dynamic offset. Nothing is overwritten within a frame, and
an arena is only reused once its frame's fence says the GPU has finished with
it. Exhausting an arena throws rather than wrapping: wrapping would hand out
storage that draws already recorded this frame still point at, and the
corruption would read as a shader bug.

`VulkanDescriptors` builds one set per frame from the binding points in
`uniforms.h`, as `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC`. The descriptors
never change - they always cover that frame's whole arena - so only the offsets
move, and the set count does not track the draw count. Each binding's range is
its block's actual size rather than `VK_WHOLE_SIZE`, so a shader cannot read
past its slice into another draw's data.

`VulkanPipeline` builds a graphics pipeline from a Slang SPIR-V module, using
dynamic rendering so there is no `VkRenderPass` or `VkFramebuffer` to keep in
step. Viewport and scissor stay dynamic; everything else is baked, which is the
concrete reason `IContext` must not be implemented here (§1.3).

`slang/vktriangle.slang` exercises the lot: a triangle positioned from
`SV_VertexID`, coloured from `localUniforms.color` pushed through the ring that
frame. Verified by capture - the drawn colour is exactly the value pushed, over
a background of exactly the clear colour, with the validation layers silent.

**`SV_VertexID` works here.** Slang lowers it to `VertexIndex` adjusted by
`BaseVertex`, which needs the `DrawParameters` capability. That is the same
capability OpenGL's SPIR-V path silently ignored, reading the builtin as zero
and collapsing every grass instance onto one (§9.5). On Vulkan the validation
layers name it immediately and enabling
`VkPhysicalDeviceVulkan11Features::shaderDrawParameters` is the whole fix. The
shaders were correct all along; the backend was not.

### 10.5 Images, samplers, and two descriptor sets

`VulkanImage` allocates a sampled 2D image and fills it through a staging
buffer. Upload is three steps rather than one, because an image has a layout:
transition to a transfer target, copy, transition to shader-read.

**Uniform blocks and textures cannot share a descriptor set.** Both number from
zero - `globalUniforms` is binding 0 and so is `sMainTex` - so they collide as
written. Uniforms are now set 0 and textures set 1, which the shaders spell as
`[[vk::binding(n, 1)]]`. `common.slang`, `grass.slang` and `pbr_model.slang`
were updated to match; verified by disassembling the SPIR-V rather than by
reading the source.

The texture set has one binding per `TextureUnits` entry, all
`COMBINED_IMAGE_SAMPLER`, all defaulted to a 1x1 white image at init. A set may
not be bound with any descriptor left unwritten, and a shader is free to sample
a unit no material filled in, so a default is not optional.

Verified by capture: an 8x8 checkerboard sampled across the triangle, tiled
twice by the repeat address mode and filtered smoothly, over the clear colour,
validation silent.

### 10.6 The stale shader trap, again

Building a specific target - `--target vulkanprobe` - does not run
`transpile_spirv`. The probe therefore ran against a module compiled before the
texture sample was added, sampled the default white texture, and produced a
result that looked like a descriptor bug. Disassembling the module showed
`sMainTex` was not in it at all.

This is the same trap already written up in the RenderDoc skill, which cost
three debugging probes the first time. The lesson it recorded - "always build
the default target" - is easy to lose the moment building one target is faster.
When a shader change appears not to take effect, disassemble the module and look
for the thing you just added before suspecting anything else.

### 10.7 Real geometry and depth

`VulkanMesh` uploads a `Mesh` as it already describes itself - interleaved
vertex data with a stride and a set of offsets - into a device-local vertex
buffer, plus an index buffer built from its faces.

The vertex layout is not reinterpreted anywhere. `Mesh::VertexLayout` gives the
offsets, and the attribute locations are the ones the Slang shaders declare with
`[[vk::location(n)]]`, which are in turn the ones the GL path binds. All three
have to agree, so `VulkanMesh::attributeDescriptions` is the single place they
are written down. Two details carried over from the GL path: one offset
(`offTanSpace`) covers three consecutive vec3s in the order bitangent, tangent,
tangent-space normal; and bone indices are stored as floats, not integers.

`Mesh` gained `vertexData()` and `vertexLayout()` accessors. It already held
both; they were simply private.

The renderer now owns a depth image sized with the swapchain and rebuilt with
it, and `VulkanPipeline::Config` takes an optional depth format that switches
depth test and write on. Without it a cube renders inside-out, which is a
useful reminder that nothing about depth is implicit here.

Verified by capture: a rotating textured cube, 36 indices, stride 32, three
attributes, with per-face normal shading and correct occlusion. Validation
silent. The uniform arena peaks at 2816 bytes for two blocks per frame across
two frames in flight, which says the 1 MB guess is generous by three orders of
magnitude and can be revisited when there are real draw counts.

### 10.8 The G-buffer and a deferred resolve

`VulkanGBuffer` owns five colour attachments and a depth attachment, in the same
order and to the same formats as `fbOpaqueGeometry` in the OpenGL PBR pipeline -
diffuse, eye normal, lightmap, self-illumination, motion - so the two can
eventually be compared attachment by attachment. One deviation: eye normals are
RGBA8 rather than RGB8, because three-component render targets are not
universally supported and the fourth channel is free here.

`slang/vkgbuffer.slang` has both halves. The geometry pass writes the five
targets from one fragment shader; the resolve reads them back and lights once
per pixel. Normals are packed to 0..1 on the way in and unpacked on the way out,
since the target is unorm and normals are signed.

A frame is now: clear, geometry pass into the G-buffer, barrier flipping all
five attachments from colour-attachment to shader-read, resolve into the
swapchain, barrier back. Verified by capture with validation silent, and the
per-face shading in the result confirms the normal target genuinely round-trips
rather than the diffuse target being copied through.

Two things Vulkan does not do for you, both caught by validation:

- **Dynamic rendering does not transition attachments.** Images are created
  `UNDEFINED`, so the first frame begins a pass declaring a layout the images
  are not in. They need one explicit transition at creation; the per-frame cycle
  takes over after that.
- **One blend state per attachment is mandatory**, even when they are identical.
  A count mismatch against `colorAttachmentCount` is an error, not a default.

### 10.9 Next

- Wiring `MeshRegistry`, `Texture` and `Material` through, so game assets rather
  than a synthesised cube go down this path.
- A pipeline cache, since every material and pass combination is now its own
  pipeline object.
- Then the engine can begin to select a backend, and `vulkanprobe` can go.
