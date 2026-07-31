# Cleanup plan: a GPU scene core, then remove OpenGL

Written 2026-07-30. Revised after an adversarial review that corrected most of
the first draft's central claim; the review is preserved in the commit history
of this file and its findings are folded in below.

## What this is for — the question that governs the rest

Every earlier revision optimised the refactor without asking where the engine is
going. That is backwards, because the answer decides whether whole phases should
exist — and it changed two of them.

`doc/vulkan-rt-backend.md:3` states the goal: *"run reone entirely on Vulkan,
reach parity with the OpenGL renderer, then go past it — hardware ray tracing,
real-time path tracing, FSR upscaling, and a material model that can feed a
physically based BSDF."* Parity was the **milestone**. Path tracing is the
**destination**. And `renderer-registration-plan.md:507` already describes
raster concepts as *"raster survivals"* that *"retire as light intensities rise
and transport carries the frame."*

**Raster is not the next GL, and the reason matters.** It stays permanently,
for two things that are easy to conflate and are actually different axes:

|  | modern hardware | no ray tracing |
|---|---|---|
| **path-traced look** | the destination | unavailable |
| **original look (Retro)** | raster, or a retro mode in the tracer | raster |

**Retro is orthogonal to hardware.** Someone on a 5090 may choose the original
look; that is an art-direction choice, not a fallback. So raster must render
both looks *and* must work with no ray tracing at all. It is load-bearing, not
transitional, and GL's fate does not apply to it.

The consequences run the other way from a deletion:

- **`GpuScene` must not require ray tracing.** The merge is a plain compute
  dispatch and the BLAS/TLAS sit on top of its output, so the representation is
  already RT-free — but that now has to stay true deliberately rather than by
  accident. The tracer layers acceleration structures over the scene; it does
  not define it.
- **Phase E is real work, justified by hardware rather than elegance.** Raster
  consuming `GpuScene` matters most exactly where the machine is weakest: a
  mega-draw over merged geometry against 1048 per-mesh draws. That is also the
  configuration with the least headroom to waste.
- **But it is an empirical question, not an assumption.** A per-frame compute
  merge on a GPU with weak compute may cost more than the draws it saves.
  Phase E must be measured on the low end, and "raster keeps per-mesh draws on
  hardware where the merge does not pay" is an acceptable answer.
- **Admission has to serve both consumers after all.** The particle budget and
  the grass classes are shared, so the raster/tracer reconciliation stands.
- **Unifying PBR and Retro is not work.** They already share a module and
  `vulkan.cpp` already branches on `_options.pbr`. Nothing to do — but Retro
  must remain reachable on the traced path too, which is an argument for it
  being a shading configuration rather than a pipeline.

## What changed from the first draft, and why

The first draft asserted that four things the tracer builds — merged geometry,
the opaque/non-opaque partition, per-triangle material ids, and the bone pool —
were "equally useful to raster and to tracing, and only live in the tracer for
historical reasons." **Three of those four are wrong**, and the fourth is only
true in the future tense.

| claim | verdict |
|---|---|
| merged world-space geometry | plausible future shared output, **not consumable today** — raster builds pipelines from each mesh's own vertex layout (`pass/vulkan.cpp:167-170`) and draws object-space vertices through `LocalUniforms.model` (`slang/pbr_model.slang:56-79`) |
| opaque/non-opaque partition | **not the same split.** Trace partitions on `surfaceType`, sky and punch-through (`rayquery.cpp:1361-1371`); raster partitions on `RenderCategory` and `Material::blending` (`vulkan.cpp:658-661`). They overlap and are not equivalent |
| per-triangle material id | useful future *identity*, but no raster shader reads such a table; raster binds textures and fills `LocalUniforms` per draw (`pass/vulkan.cpp:172-199`) |
| `InstanceMaterial` table | **wrong to share.** Trace-only surface types and curated trace operations. Raster reads the original `Material`, including `color`, `ambientColor`, env-map slots, fog and blend/cull state that `InstanceMaterial` does not carry |
| bone pool | **wrong.** Raster uploads ≤24 `glm::mat4` per draw into the uniform ring (`pass/vulkan.cpp:274-297`). And the merged result is already world-space skinned, so a raster draw of it needs no palette at all |

Also corrected: `pass/pbr.cpp` and `pass/retro.cpp` are **not** backend-agnostic.
They have no raw `gl*` calls but drive `Context`, `ShaderProgram`, `Mesh::draw`
and GL `Uniforms`; Vulkan uses a different executor entirely
(`pass/vulkan.cpp`). The first draft offered "these survive untouched" as an
invariant to check the work against. It was a false invariant.

And the GL blast radius is far larger than the "3 call sites" the first draft
claimed. That number counted literal `currentBackend()` calls and missed the
branches: `engine.cpp:199-268`, `window.cpp:31-91`, `di/module.cpp:68-91`,
`provider/shaders.cpp:87-98`, `texture.cpp:119`, `mesh.cpp:36`,
`uniforms.cpp:34`, `movie.cpp:90-105`, `graph.cpp:647-654`,
`editor.cpp:940-943`.

## The target, stated as an end state

Three modules, and **one path by which scene geometry becomes GPU data**:

```
GpuScene — the ONE compute scene-mesh creation path
    registry -> admission -> one compute dispatch -> merged world-space geometry
    stable primitive -> object/material identity, residency, lifetime

  PathTracing module        BLAS/TLAS, trace, sky environment, NRD, FSR
  Rasterizer module         ONE module, configured as PBR or Retro
```

**"One path" means one canonical output, not one upload.** An earlier revision
of this section said the merge was an alternative to per-mesh `VulkanMesh`
uploads and that collapsing to one path would make them stop existing. That is
factually wrong: the merge *reads* those uploads — `SceneObject` carries
`srcVertexAddress` and `srcIndexAddress` pointing at them. Source geometry has
to reach the GPU before anything can merge it. The honest shape is two levels:

```
CPU Mesh asset
  -> source mesh cache        device-addressable per-mesh data, uploaded once
  -> GpuScene merge           per frame -> canonical world-space triangle stream
  -> consumers                tracer today, raster after Phase E
```

So what Phase E actually deletes is **raster drawing directly from per-mesh
buffers**, not the uploads themselves. The per-mesh path stops being a draw
path and becomes a source cache, which is a smaller and more accurate claim.

The duplicated raster skinning does still go: `node/mesh.cpp:327-354` builds
palettes every frame and `pass/vulkan.cpp:274-297` uploads them per draw, while
the merge has already produced world-space skinned vertices. That deletion is
real, and it is a consequence of raster consuming the merged stream — not of
the uploads disappearing.

**PBR and Retro are not two pipelines.** They are two configurations of one
rasterizer over the same geometry. `pipeline/vulkan.cpp` already branches
internally on `_options.pbr` (`:592-595`, `:666-710`), so the second pipeline
is largely historical.

### What is missing before this can match the baseline

`GpuScene` cannot be the single path until it admits everything that is drawn.
Admission currently considers only `RegisteredMesh` (`rayquery.cpp:1159-1162`)
while the registry also holds billboards, particles, grass, AABBs and debug
entries (`registry.h:162-213`). So the known gaps are:

**Grass and particles must be admitted** — they are invisible to the tracer
today, which is a correctness gap. See Phase D. AABB, debug and walkmesh
entries stay out and should be deleted rather than excluded. Transparents are
already admitted; only the consumer's ordering and blend state differ, making
them a lowering gap.

The sequencing principle still holds, and it is what makes any of this
checkable: **whatever the merged stream is going to carry must be carrying it
before raster reads it.** Add a class while the tracer is the only consumer and
the change is deliberate and inspectable against the traced image alone; add it
after raster has switched and a difference could be the new consumer or the
missing class, with no way to separate them. That is the same trap as comparing
two numbers that differ by less than their spread.

## The corrected shape

**`GpuScene` owns merged world-space geometry, stable primitive identity, and
the lifetime of both. Consumers own their own lowering.** That is narrower than
the first draft and it is the part that survives scrutiny.

```
GpuScene  (Vulkan-only, one consumer to begin with)
  admission      which registry objects are merged, given an explicit
                 classification supplied by the caller
  residency      ResidencyClass + stable id + revision in the API NOW;
                 all-dynamic rebuild-every-frame implementation for now
  build          ONE compute dispatch -> world-space geometry
  publish        canonical primitive stream, primitive -> object/material id,
                 buffer views WITH declared consumer usage and barriers

consumers, owning their own lowering:
  RayQueryPipeline   RayMaterialBuilder (TraceClass, curated overrides,
                     InstanceMaterial), trace partition views, BLAS/TLAS,
                     SkyEnvironment, NRD, FSR
  raster             its own draw semantics — a later, designed feature
```

Three boundary decisions the review forced:

- **Sky-room classification moves out; the bake stays in.** The first draft
  left both in the tracer, which was internally inconsistent: detection decides
  which meshes are *admitted* (`rayquery.cpp:1143-1158`), and admission is core.
  A `SkyRoomClassifier` produces the decision and passes it explicitly to
  `GpuScene::update`; the cubemap allocation, six-face bake and sampling stay
  in a trace-side `SkyEnvironment`. The consumer requests replacement, the core
  admits or excludes accordingly — the consumer never reaches back into
  admission after the fact.
- **Curated overrides and `TraceClass` stay tracer-side.** They are documented
  manual trace classification and have no raster counterpart. The core keeps
  source material *identity*; `RayMaterialBuilder` constructs `InstanceMaterial`.
- **The partition needs an explicit reason or it belongs to the tracer.**
  `geometryIndex` means "may be committed as opaque in hardware" versus "must
  run candidate semantics" — that is ray policy. Either the core publishes one
  canonical stream and the tracer derives its own ranges, or the tracer supplies
  a named `TraceIntersectionClass` the core merely lays out. A core that emits
  the partition without knowing why has hidden ray policy behind a neutral name.

Two more things the review surfaced that the draft missed entirely:

- **Buffer usage and synchronization are part of the contract.** The geometry
  buffer is allocated without vertex/index usage (`rayquery.cpp:261-270`) and
  the merge barrier names only AS-build and ray-tracing reads (`:1515-1528`).
  A core cannot issue one trace-shaped barrier and call the buffer generally
  consumable; publication must declare consumer capability.
- **The sky bake is a second raster implementation hidden inside the tracer.**
  It selects `pbr_model/staticVertex`, original vertex layouts and original mesh
  draws (`rayquery.cpp:880-912`). Decide explicitly whether it stays an
  original-mesh capture pass or gets a core view, or the tangle reappears.

## Order of work

Five phases, A to E. Each has its own verification bar, stated with it, because
they are not the same bar and pretending otherwise is how a refactor stops
being checkable.

| phase | work | bar |
|---|---|---|
| **A** | remove OpenGL | pixel-identical vs the Vulkan baseline |
| **B** | extract `GpuScene`, tracer as only consumer | pixel-identical |
| **C** | residency in the contract, not the implementation | pixel-identical |
| **D** | extend admission: grass, particles, the rest | traced image changes deliberately, inspected per class; raster baseline untouched |
| **E** | raster switches to `GpuScene` | pixel-identical vs the raster baseline |

## Phase A — remove OpenGL *(done: `3445fdc1`, `6a681168`, `f14d7dae`, `df1aa375`)*

**184 files, 13,420 lines deleted against 346 added.** Four commits, not the
three planned: porting the toolkit turned up a fourth. Presenting a scene
needed forty lines of Vulkan scaffolding that only `engine.cpp` knew how to
write, so a second host could not present at all; that moved into the renderer
as `begin2DRendering`/`end2DRendering`/`presentSceneOutput`, and the engine now
calls the same code it used to inline.

Two estimates in this section were wrong in the same direction. The toolkit
port was sized **L** and took one commit, because every seam already existed.
The deletion was sized at 3537 lines and came to 10,043, because the counts
here were taken from an older tree. Both errors came from reading the plan
instead of the code.

One thing did not survive contact: **walkmesh geometry is still here.** It is
owned by the room, door and placeable game APIs, so deleting it is a game-model
refactor rather than the debug-geometry cleanup this phase assumed. That
matters downstream — it was named as the highest-leverage deletion precisely
because it carries the one attribute `MergedVertex` structurally cannot, so
Phase D inherits the problem rather than finding it solved.

**And the toolkit preview renders exploded geometry on some models** (backlog
5.6). It is not the port, not the selector removal, and not retro-on-Vulkan;
past that, nobody knows, including whether GL was ever correct. The real
blocker is that the toolkit takes no arguments, so the failing draw cannot be
reached by RenderDoc or any harness — only by a person clicking. Fix that
first.



GL removal goes first. The review argued against this — the extraction is
entirely inside the Vulkan tracer and gains nothing from GL being gone, so
this puts the central architectural change behind a large mechanical diff.
That cost is real and accepted: a tree with one backend is easier to reason
about while doing the harder work, and the deletion only gets more awkward the
longer the abstraction keeps two shapes.

The review's substantive objection is not about order, it is that "remove GL"
cannot mean one thing. **Vulkan raster still depends on the GL-shaped types**:
`pipeline/vulkan.cpp:1319-1335` reads the CPU mirror of `Uniforms` because the
GL uniforms object is inert, and `VulkanRenderPass` still receives that
`Uniforms` reference (`pass/vulkan.cpp:67-112`). Deleting the GL
implementations underneath `Context`, `Uniforms`, `MeshRegistry` and
`TextureRegistry` before replacing their Vulkan-facing role leaves nothing that
renders. So Phase A splits into three steps, and A1 is honest that it does not
finish the job:

**A1. Vulkan baseline.** Capture Vulkan PBR, retro and path-tracing. GL is
still the default backend, so an uncategorised capture proves nothing: the only
valid invariant is Vulkan-before against Vulkan-after. *(Done — nine captures
at commit `90319515` in `scratchpad/vkbase/`.)*

**A2. Delete, do not hollow.** The job is simplification, so the measure is what
   *disappears*, not whether GL is unreachable. Most of the GL-shaped types have
   no Vulkan twin because Vulkan never uses them — DI skips `Context` under
   Vulkan (`di/module.cpp:68-91`) and shader provisioning skips all GLSL
   compilation (`provider/shaders.cpp:87-98`). They are not abstractions to
   collapse later; they are dead weight now:

   | delete outright | lines | why it can go |
   |---|---:|---|
   | `graphics/context.cpp` | 381 | not constructed under Vulkan |
   | `graphics/shaderprogram.cpp` + `shader.cpp` | ~200 | GLSL only |
   | `graphics/framebuffer.cpp` + `renderbuffer.cpp` | 165 | Vulkan uses its own images and render passes |
   | `graphics/uniformbuffer.cpp` | 50 | Vulkan uses its own ring |
   | `pipeline/pbr.cpp`, `pipeline/retro.cpp` | 852 | GL pipelines |
   | `pass/pbr.cpp`, `pass/retro.cpp` | 634 | GL executors |
   | `renderer/gl2d.cpp`, `extern/glad` | — | GL |

   Plus the selector: `GraphicsBackend`, `currentBackend()`,
   `setCurrentBackend()`, `graphics/backend.cpp`, and the branches in window,
   DI, shader provisioning, texture, mesh, uniforms, movie and scene graph.

   **And the UI and app layer, which the earlier drafts underweighted.** There
   are 55 backend/GL references across ten files under `src/apps`, and they are
   not incidental — the engine selects and tears down a *different ImGui
   renderer* per backend:

   | file | refs | what goes |
   |---|---:|---|
   | `apps/engine/engine.cpp` | 21 | backend selection, window setup, ImGui renderer init/shutdown (`:199-268`) |
   | `apps/launcher/frame.cpp` | 19 | the backend dropdown and its config round-trip (`:120-145`, `:417-457`, `:496-617`) |
   | `apps/engine/editor.cpp` | 4 | the displayed backend (`:940-943`) |
   | `apps/launcher/frame.h` | 3 | `backend` config field (`:44-45`) |
   | `apps/engine/optionsparser.cpp` | 2 | the `--backend` option (`:56`, `:169`) |
   | `apps/engine/engine.h` | 2 | |
   | `apps/engine/options.h` | 1 | `backend {"gl"}` default (`:104-105`) |
   | `apps/toolkit/app.cpp` | 1 | |
   | `apps/launcher/CMakeLists.txt`, `apps/shaderpack/CMakeLists.txt` | 1 each | build wiring |

   The launcher must not keep a disabled or single-entry dropdown — the option
   goes, not just its second choice. The ImGui path becomes Vulkan-only rather
   than selected.

   **Three GL consumers the earlier drafts missed entirely**, found by the
   second review, and one of them changes the size of this phase:

   - **The wx toolkit drives the full GL pipeline through `wxGLCanvas`**
     (`toolkit/view/resource/modelpanel.cpp:43`, `:125-129`). Backlog 5.5 sizes
     porting it as **L**. **Decided: port it, and port it first**, before any
     GL deletion — the preview is one of the toolkit's eight viewers, and the
     other seven (GFF editor, 2DA tables, script decompiler, texture and audio
     preview, text) plus the batch tools are pure data work that never touches
     graphics. Deleting the renderer to spare a port would take the one viewer
     that cannot be replaced by reading the file.

     It is smaller than the L suggests, because every seam already exists:
     `VulkanRenderer` is constructed from an `SDL_Window*` and nothing else
     (`apps/engine/engine.cpp:305-311`), `GraphicsModule` already documents its
     window as "null in hosts that own presentation themselves, such as the
     wxWidgets toolkit" (`graphics/di/module.h:39-46`), `setRenderers` is the
     injection point, and swapchain recreate on resize is already handled
     (`vulkan/renderer.cpp:210-219`, `:425-429`). The missing piece is only an
     `SDL_Window` for a wx panel, which SDL3 builds from a native handle via
     `SDL_CreateWindowWithProperties` and `SDL_PROP_WINDOW_CREATE_WIN32_HWND_POINTER`.
     So: `wxGLCanvas` becomes a plain `wxPanel`, the toolkit constructs a
     `VulkanRenderer` over its handle and injects it, and `InitGL`/`SwapBuffers`
     go. Presenting into a child window keeps the render path identical to the
     engine's — no offscreen readback, no second code path to maintain.
   - **The profiler calls `context.useProgram`** at
     `apps/engine/profiler.cpp:121` and `:135`, outside any backend branch.
     Deleting `Context` breaks it.
   - **`Editor::renderRTPreview`** is a GL path the Vulkan pipeline already
     supersedes.

   All three are deletion candidates rather than porting candidates, along with
   the AABB path, `RegisteredDebug`/`drawdebug.cpp`, and `pipeline.cpp` itself.
   Highest leverage of the lot: **walkmesh debug geometry**, whose removal
   deletes the one attribute `MergedVertex` structurally cannot carry.

   **Only three types genuinely survive**, and only because they carry
   CPU-side asset data Vulkan uploads from, not because they abstract a
   backend: `Texture` (image data and features), `Mesh` (geometry and vertex
   layout, alongside the existing `VulkanMesh`), and `Uniforms` (the CPU mirror
   `pipeline/vulkan.cpp:1319-1335` already reads). Their GL halves go with
   everything else.

   Pixel-identical against the Vulkan baseline.

**A3. Finish the three survivors.** Strip each to the asset/CPU role it
actually plays and delete the backend-neutral shape. This is also where
`glToVulkanClip` (`vulkan.cpp:1319-1334`) and the negative-height viewport
(`:627-634`) get resolved by building projections Vulkan-native — that one
changes matrices, so it is its own commit with its own capture check.

## Phase B0 — one source buffer, offsets instead of device addresses

The buffer sprawl and the raw GPU pointers are the same problem. `SceneObject`
carries `uint64_t srcVertexAddress` and `srcIndexAddress` **only because every
mesh owns a separate buffer** — with one buffer there is nothing to point at,
just an offset.

Concatenate all source mesh data into two shared buffers at load time, and
convert indices to 32-bit while doing it:

```
before                                after
  per-mesh VulkanMesh buffer, xN        sourceVertices  StructuredBuffer<float>
  uint64_t srcVertexAddress             uint  srcVertexOffset
  uint64_t srcIndexAddress              uint  srcIndexOffset
  ConstBufferPointer<uint,4> loads      sourceVertices[base + i]
  loadIndex16 + dword masking           sourceIndices[base + i]
```

What this deletes outright:

- both `uint64_t` fields from `SceneObject`, and the 8-byte alignment they force
- `ptLoadFloat`, `ptLoadFloat2`, `ptLoadFloat3`, `ptLoadIndex16` and the
  raw-address fetch block in `tracing/resources.slang`
- `loadFloat` / `load2` / `load3` / `loadIndex16` in `skin.slang`, including
  `loadIndex16`'s unaligned dword read and byte masking
- the `SPV_EXT_physical_storage_buffer` capability from the compile line
- `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` and the `STORAGE_BUFFER` usage
  added to every per-mesh buffer purely so the merge could read it
  (`graphics/vulkan/mesh.cpp:104-109`)

**No shader touches a GPU address afterwards.** The BLAS build still needs
device addresses, but that is C++ handing Vulkan a `VkDeviceOrHostAddressConstKHR`
— it never enters a shader.

The buffer count becomes small and fixed, and every one is a plain
`StructuredBuffer`:

| buffer | contents |
|---|---|
| source | all mesh vertex floats, all 32-bit indices |
| scene | `SceneObject[]`, then the bone pool |
| geometry | merged vertices, indices, per-triangle material id |
| material | `InstanceMaterial[]` |

This also settles what the "source mesh cache" from the corrected end state
actually *is*: one buffer, not a collection. And it is what a raster mega-draw
wants anyway — a shared buffer bound once with per-draw offsets.

Do this **before** Phase B, because it changes `SceneObject`'s layout and the
merge kernel's entire fetch path, and both get harder to change once `GpuScene`
publishes them. Verification is pixel-identical: it is a addressing change, not
a behaviour change.

## Phase B — extract `GpuScene`, tracer as its only consumer

Data layouts, frame-lifetime buffers, the merge pipeline, publication views.
Ray material lowering, sky replacement and the trace partition stay explicit on
the ray side. Raster stays on its present per-draw path — this is an ownership
change, not a sharing claim, and it must be pixel-identical.

## Phase C — residency in the contract, not yet in the implementation

`ResidencyClass`, stable id, revision, and a defined invalidation path, while
retaining rebuild-every-frame. Retrofitting this after public views exist would
change offsets, primitive ids, material indices and AS lifetime.

`Material::staticObject` is set only for room nodes and deliberately skips the
`modelName + "a"` subtree (`area.cpp:472-488`), and the registration plan warns
it is not an immutability proof — so the audit is part of this phase, not an
assumption.

## Phase D — admit grass and particles, because the BLAS needs them

The second review argued these should all stay out and this plan briefly
agreed. That was wrong, and the reason is worth naming because it is the same
mistake twice: **the review treated current behaviour as a constraint when it
is a known bug.**

The decisive fact is that the tracer matches only `RegisteredMesh` — three
`std::get_if<RegisteredMesh>` sites in `rayquery.cpp`, and no handling of
`RegisteredGrass`, `RegisteredParticles` or `RegisteredBillboard` anywhere
(`registry.h:148`, `:162`, `:175`, `:186`). So today **grass and particles are
invisible to the path tracer**: they cast no shadow, appear in no reflection,
occlude nothing, and a ray passes straight through a hillside of grass. That is
a correctness gap, not a design choice.

They have to be in the BLAS, and the merged scene *is* the BLAS input. There is
no other way in — so they must be admitted.

**One merged scene. Everything in it.** The third review argued that grass and
particles are camera-dependent by construction — `grassVertex` derives its axes
from the camera and expands the quad in the vertex shader
(`slang/grass.slang:40-74`), particles likewise (`node/emitter.cpp:265-306`,
`slang/particles.slang:29-35`) — and that a pose correct for the primary camera
is not correct for a reflection or shadow ray arriving from elsewhere.

True, and not a reason to do anything differently. The orientation error on a
grass blade seen from a bounce ray is small; the error from **grass not
existing in the acceleration structure at all** is total. Cluster selection
being camera-position-dependent is likewise a small effect at 32 m. Both are
bounded approximations worth accepting to get one representation, and neither
justifies a second geometry path or a per-class lowering.

So: build the quads with the axes available at merge time, admit them, and
record the approximation. If it ever visibly matters — grass swimming in a
mirror, say — revisit it *then*, with the artefact in hand rather than in
principle.

### Scale, since this was the other open question

- **Grass is cheap.** The grass mesh is a two-face quad
  (`graphics/meshregistry.cpp:49-59`), so a full 2048-cluster pool
  (`node/grass.cpp:42`) is **4,096 triangles** — 86k becomes ~90k, about +4.8%.
  Raster does not cap at 256; it frustum-culls and batches in 256s
  (`registry.cpp:605-626`), where 256 is only the uniform-block limit.
- **Particles: budget 1000.** At a quad each that is 2,000 triangles, so with
  grass the frame goes 86k → ~92k, about **+7%**. Not a scale problem.

  This forces a second decision. Raster silently draws only the first **64** in
  one call (`pass/vulkan.cpp:420-442`) while the registry can pass more
  (`registry.cpp:584-603`). If the tracer admits 1000 and raster keeps drawing
  64, the two renderers disagree about what is in the scene — and since this
  plan verifies by comparing traced and raster captures, that divergence
  quietly breaks the check itself. **Raise the raster bound to match.** Whatever
  number is chosen, one number, both consumers.
- **The 0.35 ms figure is not a measurement of this renderer.** It is an
  external single-operation linear extrapolation, and the backlog says so
  itself while naming GPU timings as the missing gate. It omits the merge
  expansion, the source/scene/material uploads, the barriers, and any cost from
  a different non-opaque mix.

With those settled, each objection becomes a work item rather than a veto:

- **Backlog 1.6** (deterministic grass placement) and **1.7** (particles as
  geometry or composite) both resolve here, and both resolve the same way:
  geometry, in the merged scene. 1.7 in particular is settled by the fact that
  a rasterised composite cannot be in the BLAS at all.
- **Transparents** are already admitted; only raster's ordering and blend state
  differ. A lowering gap in the consumer, not an admission gap.

What genuinely stays out should be **deleted rather than excluded**: AABB and
debug entries are already `warnOnce` stubs, and walkmesh/trigger geometry is
debug visualisation. That last one earns a note — it carries `offMaterial`, the
one attribute `MergedVertex` structurally cannot express, so deleting it
removes the only reason to widen the vertex format.

Order within the phase: grass first, since it forces the 1.6 determinism fix
and is the largest visible gain in the traced image, then particles and
billboards. One class per commit, tracer still the only consumer, each
inspected against the traced image while the raster baseline stays untouched.

Phase E then accepts that raster keeps a non-merged path for
whatever stays out.

## Phase E — raster consumes `GpuScene` where it can

Not a switchover. The second review lists five concrete breakages, and each is
a prerequisite rather than a detail:

1. **Shadow-only proxies are excluded from the merge by category**
   (`rayquery.cpp:1169-1172`) and are exactly what the shadow pass draws.
2. **The merge is unculled by design** (`:1048-1051`). Merged shadow draws would
   lose frustum rejection across four cascades and six cube faces — a large
   regression, not a wash.
3. **Dangly and saber are frozen at base pose.** The merge does bone skinning
   only, so raster consuming it would freeze 653 canopies on danm14ab.
4. **Hashed alpha test hashes object-space position**
   (`pbr_model.slang:280, 335`). World-space vertices make foliage dither swim
   under motion.
5. **`offMaterial` has no field in `SceneObject`**, which walkmesh geometry
   needs.

Motion vectors are the one thing merged geometry straightforwardly improves.

The honest scope is therefore: raster consumes the merged stream for the static
and skinned opaque set where all five are resolved, and keeps its own path for
shadows, dangly, saber, grass, particles and walkmesh until each is separately
answered. **Claiming a full switchover here would be claiming a rewrite.**

`pipeline/vulkan.cpp` is not a later isolated payoff: it owns frame ordering and
returns before every raster pass in path-tracing mode (`:1337-1373`). A core
consumed by both needs a precise update point and per-consumer barriers. Its
`glToVulkanClip` rewrite (`:1319-1334`) and the negative-height viewport
(`:627-634`) are real Vulkan cleanups but cannot be bundled into a
no-pixel-change GL deletion.

## Hazards

- **The shared struct header is not a single source of truth.** `skin.slang`
  independently declares `SceneObject`, `MergedVertex` and `Matrix3x4`
  (`slang/skin.slang:8-53`); the C++ `static_assert`s are the only ABI guard.
  A header must not imply otherwise unless layout generation or reflection is
  added with it.
- **Raster skinning stays duplicated.** `MeshSceneNode` builds palettes per
  frame (`node/mesh.cpp:327-354`) and the pass uploads them per draw, while the
  merge re-skins into world space. Deleting the duplicate requires step 4.
- **Config migration.** `backend` defaults to `"gl"` and the launcher persists
  it. Removing the option needs a decision: ignore legacy `backend=`, drop it
  on rewrite, and make Vulkan a build requirement rather than keeping
  `#ifdef R_ENABLE_VULKAN` fallbacks.

## Verification

Harness rules, all load-bearing:

- Build ONCE per configuration, then run every module against that build.
- ALWAYS pass `--capture` with `--captureframe`. `--captureframe` alone never
  exits, leaving an engine at 100% GPU that corrupts every later measurement.
- Capture the same module twice and use the second.
- Confirm no `engine.exe` survives before measuring anything.
- Two numbers differing by less than the spread of either are not a result;
  noise runs to about 12% on one module.

**"Pixel-identical" is not available, and assuming it was cost this phase its
first false alarm.** Captures are no longer reproducible run to run, measured
here on `danm14ab` at frame 310 with two runs of one unmodified binary:

| | differing pixels | mean abs error | mean luminance |
|---|---:|---:|---|
| retro raster | 0.0245% | 0.022 | 77.314 vs 77.317 |
| path tracing | 13.40% | 0.118 | 111.724 both |

The images are the same image — mean luminance agrees to three decimals and
the mean absolute error is a fiftieth of a grey level — but individual pixels
move, by up to 219 in a channel. This is backlog **7.1**, already open at P1 —
not something this phase introduced. What is new is the measurement: 7.1
described the effect as bimodal and intermittent, and these figures show it is
present on every run, small in magnitude, and different between raster and
tracing. It also contradicts the diagnostics skill, which still documents
raster as byte-identical and tracing as bounded at 0.02%; that text is wrong
and should be corrected before it misleads another comparison.

So the bar becomes **the change must be indistinguishable from run-to-run
noise**: capture the baseline module twice, and require the
baseline-versus-candidate difference to be no larger than the
baseline-versus-baseline difference, on all three of differing-pixel share,
mean absolute error and mean luminance. A candidate that lands inside that
spread has not changed the image. Comparing a single run against a single
stored baseline reports failure at 12% for a build that changed nothing.

Step 4 cannot be compared at all, and should be measured rather than compared.

## Not in scope

No behaviour changes, no features, no performance work. If something looks
wrong mid-refactor, write it down rather than fixing it: a refactor that also
changes behaviour cannot be verified by comparing images.
