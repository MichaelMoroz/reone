# Vulkan, ray tracing and path tracing — the plan

Goal: run reone entirely on Vulkan, reach parity with the OpenGL renderer, then
go past it — hardware ray tracing, real-time path tracing, FSR upscaling, and a
material model that can feed a physically based BSDF.

**Status, 2026-07-26.** The engine runs on Vulkan as far as the main menu:
`engine --backend vulkan` renders it correctly, matching the OpenGL frame except
for the 3D model behind the panel. The backend has a device, swapchain, memory,
descriptors, a pipeline cache, resource upload, a complete 2D renderer, and a
G-buffer with a deferred resolve. The shipping model and grass shaders have been
shown to render through it. What does not exist is the scene pipeline — so
nothing in the game world draws on Vulkan yet.

This document is the whole road, not just the next step. Part I is what stands
today, Part II is parity, Part III is what parity is *for*, and Part IV is
reference material: conventions that must hold, and traps already paid for once.

---

# Part I — Where things stand

## 1. What runs today

`src/libs/graphics/vulkan/` (14 translation units):

| Piece | What it does |
| --- | --- |
| `device` | instance, surface, physical/logical device, queues, VMA allocator, immediate submit |
| `swapchain` | the images presented from, rebuilt on resize |
| `renderer` | `IRenderer`: acquire, record, submit, present, two frames in flight, readback |
| `renderer2d` | `I2DRenderer`: sprites, rects, text, full-target images, blend and scissor |
| `buffer` / `image` | VMA allocations in the shapes that matter, with layout transitions |
| `uniformring` | the per-frame bump allocator that replaces GL's whole-block overwrite |
| `descriptors` | uniform set (dynamic offsets) and texture set (per-frame, per-texture) |
| `pipeline` / `pipelinecache` | pipelines built on first use, keyed on their full state |
| `mesh` | `Mesh` uploaded to device-local vertex and index buffers |
| `gbuffer` | five colour attachments plus depth, matching the GL layout |
| `resources` | engine `Texture` and `Mesh` uploaded once each, keyed by address |

Shaders in `slang/`: `uniforms` (the ten blocks), `pbr_model` (four geometry
entry points plus the opaque fragment), `grass`, `walkmesh`, `vk2d` (the 2D
vocabulary), `common`, and six shared modules under `slang/lib/`.

Verified by capture with the validation layers silent: the main menu; a textured,
depth-tested mesh; a five-attachment G-buffer and deferred resolve; all four
`pbr_model` geometry variants; 256 instanced grass clusters.

**Not built:** the scene pipeline and every pass in it. That is the whole of
Part II.

## 2. The architecture as built

### 2.1 Backend selection

`graphics/backend.h` holds the choice process-wide. Deliberately not threaded
through constructors: the two backends cannot share a window, so it is settled
once before anything graphical exists, and the objects that need to ask —
`Texture`, `Mesh` — are built deep inside resource providers that have no other
reason to know a backend exists.

`--backend vulkan` sets it before the window is created. `Window` then asks for
`SDL_WINDOW_VULKAN` and creates no GL context.

### 2.2 The two seams

Everything outside the graphics library talks to the backend through exactly two
interfaces, each with two implementations:

- **`IRenderer`** owns the frame: `beginFrame` / drawing / `endFrame`, plus
  `drawSceneOutput` and `captureFrame`.
- **`I2DRenderer`** owns everything drawn in screen space.

This is what makes a second backend possible at all, and it paid off as intended:
adding `Vulkan2DRenderer` required no caller changes anywhere.

**`IContext` is not one of the seams and must never be implemented in Vulkan.**
It is an OpenGL state machine wearing an interface — `pushDepthMask`,
`popPolygonMode`, `bindTexture(unit)`. Implementing it means a per-draw
pipeline-state hash emulating a state machine, which is the classic failed port.
State belongs in `VulkanPipelineCache::Key`.

### 2.3 GL services under Vulkan

`Context`, `MeshRegistry`, `TextureRegistry` and `Uniforms` are constructed but
never initialised. They still hand out references through `GraphicsServices`, but
nothing on the Vulkan path may call them, and anything that does faults
immediately rather than silently drawing nothing.

That is the right trade while the backend is incomplete: it is exactly how five
leftover GL calls on the 2D path were found. Once parity is reached they should
become null implementations or disappear with the GL backend.

`GraphicsModule` cannot construct the Vulkan renderers — `graphicsvulkan` links
against `graphics`, not the reverse — so the engine, which links both, builds
them and injects them with `setRenderers`.

### 2.4 Two things the scene pipeline had to work around

**Render passes cannot nest.** The scene pipeline begins passes of its own, so
it has to record before the frame's 2D scope opens. `Game::renderSceneOffscreen`
splits producing the scene image from compositing it; only the composite belongs
inside the 2D pass. The same constraint is why the 3D sub-scene behind menu
panels is still skipped - `Control::render` runs inside the GUI pass.

**A render target crosses the seam as a `Texture &`.** That is what
`IRenderer::drawSceneOutput` takes, and it has no pixels to upload.
`VulkanResources::registerExternal` associates the handle with the image, so the
identity survives without changing the interface or giving `Texture` a
backend-specific field.

### 2.5 Deliberately skipped under Vulkan

Each has its reason recorded at the site: GLSL compilation (`Shaders::init`),
movie playback (`Movie::render`), the ImGui editor (built on the GL backend), and
the 3D sub-scene behind menu panels (`Control::render`).

---

# Part II — Parity

The target is the OpenGL PBR pipeline: everything the game draws today, drawn the
same way. This is the bulk of the remaining work, and nothing in Part III can
start without most of it.

## 3. The scene pipeline

`src/libs/scene/render/pipeline/pbr.cpp` is roughly 500 lines of orchestration
over six named passes: `DirLightShadowsPass`, `PointLightShadows`,
`OpaqueGeometry`, `TransparentGeometry`, `PostProcessing` and `Debug`. None
exists in Vulkan.

### 3.1 What to build

A `VulkanRenderPipeline` implementing the same interface the GL pipelines do, so
`SceneGraph::render` does not care which is behind it. It needs:

1. **Render targets.** The GL pipeline holds around a dozen: G-buffer, ping/pong
   at full and half resolution, SSAO, SSR, shadow maps, output. `VulkanImage`
   covers colour attachments and depth already; what is missing is a target set
   that owns them, sizes them together and rebuilds on resize —
   `VulkanGBuffer` generalised.
2. **A pass abstraction.** Dynamic rendering means no `VkRenderPass` objects, so
   a pass is a small record: attachments, load/store ops, pipeline key, and the
   barrier needed before it. Worth making explicit rather than open-coding
   `vkCmdBeginRendering` a dozen times, because the barriers between passes are
   where the bugs live.
3. **Barrier discipline.** Every target written as an attachment and then sampled
   changes layout twice a frame. `VulkanGBuffer` tracks its own layout because
   callers got it wrong; the generalised version must do the same.

### 3.2 Order of work

Shadow maps last, not first. They are the only pass needing layered rendering and
a different projection per layer, and everything else can be verified without
them.

1. ~~Opaque geometry into the G-buffer.~~ **Done.** `VulkanRenderPipeline` and
   `VulkanRenderPass` drive the G-buffer from the scene graph, and a warped-into
   module renders: terrain, props, and skinned characters with their materials.
   Known wrong: lightmapped surfaces come out black and the sky is blown out,
   both of which are the placeholder resolve rather than the geometry pass;
   grass, particles, billboards and AABBs warn once and skip.
2. The deferred resolve with real lighting (§4.1). **This is now the blocker for
   everything looking right**, not a refinement.
3. Transparency and OIT.
4. SSAO and SSR.
5. Post-processing: bloom, FXAA, sharpen, combine. On Vulkan the last two
   are replaced by FSR (§12.2) rather than ported.
6. Shadows, directional then point.
7. Debug: AABBs, walkmeshes, the draw-debug overlay.

Each step is independently verifiable against the GL frame with the existing
capture harness, which is the reason for this order.

## 4. Materials and lighting

### 4.1 Lighting

The resolve currently lights from one fixed direction. `glsl/f_pbr_combine.glsl`
is the real thing and has to come across: light lists, shadow lookups, the PBR
BRDF, fog, and the geometry feature bits packed into the lightmap alpha.

`lib/surface.slang` already describes a surface independently of how it was
sampled, which is the right shape — the resolve consumes `SurfaceParams` and does
not care whether a G-buffer or a ray hit produced it. Keep that.

### 4.2 Materials reaching the backend

`Material` (`include/reone/graphics/material.h`) is a texture-unit map plus a
handful of colours and flags. It never reaches the Vulkan backend today.

Two things are needed, and they should be designed together because the second is
what §9 wants:

- A **material → uniform mapping**, so `LocalUniforms.featureMask` and the colour
  fields are filled from `Material` rather than by hand.
- A **material buffer**, GPU-resident and index-addressed, rather than a
  descriptor set bound per material. The raster path indexes into it; the path
  tracer will index into the same buffer from a hit record. Designing it once,
  now, avoids doing it twice.

## 5. The remaining geometry kinds

The model variants (static, skinned, dangly, saber) and grass are done in the
sense that their pipelines build and draw. Still needed: **particles**,
**billboards**, **walkmeshes** and the **AABB** debug geometry. Each is an entry
point already written or portable from GLSL, plus a pipeline variant the cache
will produce.

`walkmesh.slang` exists and is registered nowhere.

## 6. The rest of the frame

- **Movie playback.** Uploads an ffmpeg frame per tick. The resource cache
  uploads once and keeps; this needs a streaming path — a per-frame staging
  buffer and an image re-written rather than re-created.
- **The cursor** already goes through `I2DRenderer` and should work once the game
  reaches gameplay.
- **The ImGui editor** needs the Vulkan ImGui backend. Low priority but cheap,
  and it is the render-target viewer that makes the G-buffer inspectable.
- **The profiler and console** render through GL directly and need converting to
  `I2DRenderer` — mostly text and rects.

## 7. Retiring OpenGL

**Decision: freeze GL as maintenance-only, then retire it.** Two live backends
taxes every interface decision.

- GL receives no new features. It only ever needs to do what the toolkit does —
  model viewing.
- The toolkit stays on `wxGLCanvas` for now: wxWidgets 3.3 has no Vulkan canvas,
  and writing `VkSurfaceKHR`-from-`HWND` glue before any RT payoff is the wrong
  order of work.
- **Retirement path:** the toolkit is a model viewer and needs no swapchain.
  Render Vulkan offscreen, read back, blit into a plain `wxPanel`. No
  platform-specific surface code at all, and once it works the GL backend and its
  71 GLSL shaders can be deleted outright.

Deferred cleanup still outstanding: the SPIR-V loading path in the GL backend,
the `--slangshaders` toggle, and `ShaderRegistry` variant selection. All dead
since §14.4 and worth removing before they confuse someone.

---

# Part III — Beyond parity

## 8. Material PBR-ification

A prerequisite for path tracing, and worth stating plainly: **no KotOR asset
carries metallic or roughness.** Odyssey materials are diffuse, lightmap, envmap
and bumpmap, plus ambient, diffuse and self-illumination colours. A path tracer
needs a real BSDF parameterisation, so one has to be synthesised.

### 8.1 What the assets do tell us

- **Envmap presence** is the strongest signal available: a surface with an
  environment map is meant to look reflective, and envmap intensity maps
  reasonably onto low roughness.
- **Specular colour in the TXI** exists on some textures.
- **Bump and normal maps** give detail independent of roughness, but their
  presence correlates with materials the artists treated as detailed.
- **Diffuse luminance and saturation** are weak but usable priors: near-neutral
  dark surfaces are more often metal-ish, saturated bright ones dielectric.

### 8.2 Approach

A **heuristic mapping layer**, not a guess baked into assets:

1. A default dielectric BSDF — roughness around 0.7, metallic 0, specular 0.04.
2. Envmap-bearing materials get lower roughness, scaled by envmap intensity.
3. An override table keyed by texture or material name, for the cases the
   heuristic gets visibly wrong. Ship it with the engine; it is small, and it is
   the honest way to handle a game whose art has no PBR intent.
4. The lightmap becomes a *baked irradiance* input rather than a multiplier once
   real GI exists (§16.2).

The mapping belongs in the material buffer of §4.2, computed at load time, so the
raster and traced paths see identical parameters.

### 8.3 Ordering

After raster parity, before path tracing. Earlier means changing the raster look
while still using it as the parity reference; later means the path tracer has
nothing sensible to trace.

## 9. The scene has to be GPU-resident

A path tracer has no draws, so there is nothing to bind per draw. A ray may hit
any surface, which means the whole scene must be addressable before tracing
starts:

- one global vertex/index buffer with meshes suballocated into it;
- one material buffer, indexed by instance (§4.2);
- **bindless textures** — a descriptor-indexed array sampled by an index taken
  from the hit record;
- TLAS instances carrying that material index.

This replaces the per-frame texture descriptor sets of §16.3, which exist only
because a set bound to a recording command buffer cannot be rewritten. Descriptor
indexing removes the problem rather than working around it.

`SceneGraph::refresh()` already flattens the node tree into typed arrays every
frame, which is the natural hook for building TLAS instances. **One caveat:**
those arrays are rebuilt and reordered per frame, so a BLAS instance cache needs
a stable per-instance identity that does not exist yet.

### 9.1 Anything the vertex shader synthesises must become real geometry

Rasterisation lets the vertex stage invent geometry. A ray tracer cannot see
that: only what is in a BLAS exists.

| Kind | Today | Needed |
| --- | --- | --- |
| Skinned meshes | bones applied in the vertex shader | compute skinning into a buffer, rebuild BLAS per frame |
| Dangly meshes | positions from a uniform block | same, or CPU-written into the vertex buffer |
| Sabers | quads built from a displacement | real quads |
| Grass | billboards from cluster positions | real quads, or leave rasterised |
| Particles | billboards | leave rasterised and composite |

Compute skinning is the largest item here and is shared with §5 — the raster path
can use it too, which removes the vertex-shader skinning path entirely.

## 10. Hardware ray tracing

### 10.1 Extensions, and the choice within them

Required: `VK_KHR_acceleration_structure`, `VK_KHR_deferred_host_operations`,
buffer device address, descriptor indexing. Then one of:

- **`VK_KHR_ray_query`** — tracing from inside ordinary fragment or compute
  shaders. Simpler: no shader binding table, no new pipeline type. Right for
  hybrid effects.
- **`VK_KHR_ray_tracing_pipeline`** — ray generation, closest-hit, any-hit and
  miss shaders with a shader binding table. Required for a real path tracer,
  where recursion and per-material hit shaders matter.

**Do both, in that order.** Ray query first, because it needs no new pipeline
machinery and gives a visible result early. The full pipeline when path tracing
starts.

### 10.2 Acceleration structures

- **BLAS per mesh**, built once for static geometry, rebuilt or refitted per
  frame for skinned and dangly.
- **TLAS per frame** from the flattened scene arrays, carrying a material index
  per instance.
- Refit rather than rebuild where topology is unchanged — the common case for
  skinned meshes.
- Compaction for static BLASes is worth it, and cheap once the build path works.

### 10.3 Hybrid effects, in order of payoff

1. **Ray-traced shadows**, replacing the shadow map passes entirely. Biggest
   visual win per unit of work, and it deletes two passes.
2. **Ray-traced ambient occlusion**, replacing SSAO.
3. **Ray-traced reflections**, replacing SSR — the one that most obviously beats
   its screen-space predecessor, which cannot reflect what is off-screen.

Each is independently shippable and each removes an existing pass, so the
pipeline gets simpler as it gets better.

### 10.4 Alpha-tested geometry needs any-hit shaders

Foliage and grass are alpha-tested (`lib/hashedalpha.slang`). Under
rasterisation the fragment shader discards; under tracing an any-hit shader must
do the equivalent, and it must be cheap, because it runs per candidate hit.

## 11. Path tracing

The endpoint: `RendererType::PathTraced` alongside the raster pipelines, sharing
the scene, materials and acceleration structures.

### 11.1 Shape

- A **ray generation** shader per pixel, using the camera jitter that already
  exists for TAA.
- **Closest-hit** shaders filling `SurfaceParams` from the material buffer and
  the hit record — the same struct the deferred resolve consumes, which is why
  `lib/surface.slang` is worth keeping as the single surface description.
- **Any-hit** for alpha test (§10.4).
- **Miss** returning skybox or fog.
- Multiple bounces with Russian-roulette termination.
- **Next-event estimation** against the scene light list. KotOR interiors are lit
  by many small point lights, and pure path tracing without NEE will be unusably
  noisy in exactly the scenes people care about.

### 11.2 What it replaces, and what it keeps

Keeps the raster path for GUI, text, particles and movies; none of those has any
business being traced. The 2D renderer is already backend-clean, so this costs
nothing.

### 11.3 Realistic expectations

At 1080p on a modern GPU the budget is one to two samples per pixel per frame.
Everything then depends on the denoiser, which is why §12 is not optional.

## 12. Denoising and FSR

### 12.1 Denoising

One to two samples per pixel is unusable raw. Options:

- **NRD** (NVIDIA Real-Time Denoisers) — mature, vendor-neutral in practice,
  designed for exactly this input. The pragmatic default.
- **Hand-rolled SVGF** — more work, fully understood, a reasonable fallback if
  NRD's licensing or integration proves awkward.
- **ReSTIR** for direct lighting is a larger change but the right answer for
  many-light interiors, and KotOR interiors are exactly that.

Defer the choice until §10.3 produces real traced input. Choosing now would be
choosing without data.

### 12.2 FSR

**FidelityFX FSR, native Vulkan backend.** Upscaling matters more here than in
most projects: it is what buys the sample budget back.

What FSR needs, and how much exists already:

| Input | Status |
| --- | --- |
| Colour | yes |
| Depth | yes |
| **Motion vectors** | **yes** — RG16F G-buffer attachment, written by all four opaque shaders |
| **Camera jitter** | **yes** — Halton (2,3), behind `--taajitter`, off by default |
| Exposure | trivial |

Motion vectors and jitter were built in phase 1 precisely so this would be wiring
rather than a project. Two things to settle when it lands:

- **Jitter has been off by default** because nothing consumed it. FSR is that
  consumer, and turning it on will change every screenshot comparison — do it
  deliberately, not incidentally.
- **Motion vector conventions** (§16.1) are still unverified. FSR is the first
  real consumer and will expose any sign or scale error immediately.

**Temporal FSR, not spatial, and it replaces FXAA rather than joining it.**
Decided deliberately: FSR 1 is EASU plus RCAS, a spatial upscaler that does not
anti-alias at all - at native scale it is little more than a sharpen, so the
aliasing FXAA exists to hide would remain. FSR 2/3 accumulates over frames and
therefore *is* the anti-aliasing, which is also what makes it the right fit for
a path tracer later. So the OpenGL pipeline's FXAA and sharpen stages have no
Vulkan counterparts at all: FSR subsumes both, RCAS doing the sharpening.

**Take the SDK; do not port the shaders.** FSR 1 would be two shaders and worth
porting to keep the Slang-only convention. FSR 2 is a dozen-odd compute passes
with history buffers, lock management, reactive masks and exposure handling,
and hand-porting it would be substantially more work than integrating AMD's,
which already ships a Vulkan backend. It is not in vcpkg, so it arrives as the
first vendored build dependency.

Integration risks specific to this backend, none of them settled:

- **volk owns the Vulkan entry points here.** The SDK's Vulkan backend resolves
  its own, and the two have to be reconciled - the same class of problem as the
  VMA segfault in §16, which needed `VMA_DYNAMIC_VULKAN_FUNCTIONS`.
- **Render resolution is currently the swapchain resolution.** Upscaling means
  decoupling the scene pipeline's target size from the window, which nothing
  needs today.
- **The 2D layer must not be upscaled.** This one falls out well: the scene
  already crosses to the compositor as a texture, so FSR slots exactly at that
  seam and the GUI draws over the result at display resolution.

Verify the current FSR licence text before committing. It has been MIT, which is
compatible with GPL-3, but these terms change.

---

# Part IV — Reference

## 13. Dependencies

| Purpose | Choice | Status |
| --- | --- | --- |
| Loader | volk | in use |
| Headers | vulkan-headers | in use |
| Allocation | VMA | in use |
| Init | vk-bootstrap | in use |
| Window | `sdl3[core,vulkan]` | in use — the default port refuses `SDL_WINDOW_VULKAN` |
| Shaders | shader-slang | in use |
| Upscaler | FidelityFX FSR 2/3, SDK vendored | not started |
| Denoiser | NRD, or hand-rolled SVGF/ReSTIR | not chosen |

Gated behind `ENABLE_VULKAN`, default OFF, so ordinary builds are unaffected.

### 13.1 Licensing

reone is GPL-3, clean-room, explicitly non-commercial. FSR is permissively
licensed and links without conflict — *verify the current text before
committing*. No dynamic-plugin indirection is needed; a thin internal
`IUpscaler` seam is still worth having so upscaling can be switched off at
runtime and the denoiser swapped, but it is an ordinary interface compiled in.

## 14. Shader architecture

### 14.1 Share code, not entry points

One module per program family, entry points per variant, shared code in
`slang/lib/`. Slang emits one SPIR-V blob per module with every entry point in
it, so shared code compiles once and a second entry point costs nothing.

### 14.2 Specialise geometry, branch on material

Geometry kind is a pipeline: static, skinned, dangly and saber are separate
vertex entry points. Material variation is a branch on `featureMask` inside one
fragment shader. Geometry differences change the vertex contract; material
differences do not.

### 14.3 One surface description, two consumers

`lib/surface.slang` describes a shaded surface independently of how it was
sampled. The deferred resolve fills it from a G-buffer; the path tracer will fill
it from a hit record. Everything downstream is shared. This is the single most
important structural decision for making §11 tractable.

### 14.4 Why Slang no longer targets OpenGL

Slang's SPIR-V uses Vulkan builtins — `InstanceIndex`, `VertexIndex`,
`BaseInstance`, needing the `DrawParameters` capability. OpenGL's SPIR-V path
reads them as **zero, silently**: 256 grass instances collapsed onto one and the
field vanished. Finding it took a RenderDoc investigation.

On Vulkan the same shader works, and enabling
`VkPhysicalDeviceVulkan11Features::shaderDrawParameters` is the whole fix. The
shaders were correct all along; the backend was not. That is the clearest single
justification for the move.

## 15. Conventions that must hold

Load-bearing. Breaking one produces a plausible-looking wrong image.

- **Descriptor sets.** Uniforms are set 0, textures set 1. Both number from zero,
  so they cannot share a set — `[[vk::binding(n, 1)]]` for textures.
- **Uniform block bindings** are pinned in `uniforms.slang` and mirrored by
  `uniformlayout.generated.h`, which asserts every offset and size at compile
  time. That header caught a real std140 bug; regenerate it, never edit it.
- **Vertex attribute locations** are declared in three places that must agree:
  `Mesh::VertexLayout`, the Slang `[[vk::location(n)]]`, and
  `VulkanMesh::attributeDescriptions`. Two quirks carried from GL: `offTanSpace`
  covers three consecutive vec3s as bitangent, tangent, tangent-space normal; and
  bone indices are stored as floats.
- **Clip space.** Vulkan depth is 0..1 — use the `_ZO` GLM variants, never
  `GLM_FORCE_DEPTH_ZERO_TO_ONE`, which would change the GL backend too. Clip
  space y points *down*, so a screen-space ortho passes `(0, w, 0, h)`, not the
  OpenGL `(0, w, h, 0)`.
- **Texture v.** The same bytes are read bottom-up by OpenGL and top-down by
  Vulkan, and the quad mesh pairs position (0,0) with uv (0,1). 2D shaders flip v.
- **Screenshots must match the window.** `captureFrame` reverses rows, because a
  Vulkan image copy is top-down where `glReadPixels` is bottom-up.
- **`captureFrame` before `endFrame`.** A presented swapchain image has undefined
  contents.

## 16. Traps already paid for

Recorded so they are not paid for twice. Every one cost real time.

### 16.1 Motion vectors — done, conventions unverified

`GlobalUniforms` carries unjittered current and previous view-projection plus the
jitter offset; `LocalUniforms` a previous model matrix; `BoneUniforms` a previous
bone set. `SceneNode` latches its previous absolute transform once per frame. The
G-buffer has an RG16F motion target written by all four opaque shaders.

**Nothing consumes it yet**, so the sign and scale conventions are unverified.
FSR (§12.2) is the first real consumer and will expose any error.

### 16.2 Lighting model — treat all albedo maps as albedo

Baked lightmaps are treated as albedo for now, even where that is physically
wrong. Revisit when real GI exists (§8.2).

### 16.3 The bugs

- **VMA needs its dynamic function path** with volk. With both static and dynamic
  function macros off it calls through null pointers inside `vmaCreateAllocator`
  — before any allocation, with no diagnostic. The macros belong in CMake so every
  translation unit agrees with the one defining the implementation.
- **The present semaphore is per swapchain image, not per frame in flight.**
  Presentation consumes it against an image and never signals that it has.
- **Dynamic rendering does not transition attachments.** Images created
  `UNDEFINED` need one explicit transition before the first pass declares a
  layout they are not in.
- **One blend state per attachment is mandatory**, even when identical.
- **Image layout belongs to the object.** `VulkanGBuffer` tracks its own, because
  whether the attachments are readable depends on what the previous frame did and
  the caller cannot know.
- **A descriptor set bound to a recording command buffer cannot be rewritten.**
  Pointing one shared texture set at a different image per draw invalidates the
  buffer — 81 validation errors from one `vkUpdateDescriptorSets`. Descriptor
  indexing (§9) is the real fix.
- **A descriptor's image view type must match the shader's declaration.**
  `Sampler2DArray` and `SamplerCube` units need array and cube views; a 2D view is
  an error, not a coercion.
- **A vertex input the pipeline does not supply is an error**, not a default.
- **KotOR textures are DXT** and upload as BC1/BC3 unchanged.
- **Empty uniform blocks hide failures.** Three of the four model variants draw
  fine with zeroed blocks; `danglyVertex` takes its position wholly from
  `DanglyUniforms` and silently vanishes. A variant rendering nothing is not
  necessarily a broken pipeline.
- **Building a named target skips `transpile_spirv`.** This has cost two separate
  investigations. When a shader edit appears not to take, disassemble the module
  (`spirv-dis x.spv | grep Decorate`) before suspecting anything else.
- **A verification path can hide the bug it should catch.** Ten teardown
  validation errors were invisible because every run used `--capture`, whose
  readback waits on the queue and left the device idle by accident.
- **An "intermittent" crash may not be.** `imguiHandle` ran on every SDL event
  with no ImGui context under Vulkan; it faulted only when an event arrived before
  the first frame, which looked like a race.

### 16.4 The comparison harness

Screenshot comparison is only meaningful with care:

- `--captureframe` counts frames, not seconds, and capture runs use a fixed 1/60
  timestep. Two runs at the same frame are bit-identical **for GUI frames**.
- **Gameplay frames are not deterministic.** About a third of the image differs
  between two runs of the *same* build, and the variance is *bimodal* — pairs
  either agree within 1% or differ across 33%. A single A/B pair endorses whatever
  you hoped for about half the time. Ruled out already: RNG divergence (draw
  counts identical), frame timing, SSAO/SSR, an off-by-one frame, and threaded
  loading. Unresolved; see §17.
- Match settings, not just builds — a second build tree without `reone.cfg` runs a
  different resolution *and* a different pipeline.
- The mouse cursor is in the capture.
- Diff by region against the same-build noise floor, not by whole-frame
  percentage. That is what exposed a real minimap regression hiding inside
  animation noise.

## 17. Open questions

- **Scene nondeterminism** (§16.4). Must be closed before GL-and-Vulkan parity can
  be checked automatically, which is the entire point of the harness. The
  bimodality is the strongest clue: something settles into one of two states
  early. Worth checking whether the number of `update` calls before the capture
  frame is constant, and whether any pass samples a target that is never cleared.
- **Denoiser choice** (§12.1) — defer until §10.3 produces traced input.
- **Compute skinning ownership** — the raster path could use it too, which would
  delete the vertex-shader skinning path. Decide when §9.1 is built.
- **Whether grass and particles are ever traced**, or stay rasterised and
  composited. Leaning composited; revisit if the seams show.
- **`IStatistic` has no GPU timing.** Needed before any performance claim about
  the Vulkan backend can be taken seriously.
