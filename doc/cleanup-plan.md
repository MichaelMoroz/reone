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
- **Phase F is real work, justified by hardware rather than elegance.** Raster
  consuming `GpuScene` matters most exactly where the machine is weakest: a
  mega-draw over merged geometry against 1048 per-mesh draws. That is also the
  configuration with the least headroom to waste.
- **But it is an empirical question, not an assumption.** A per-frame compute
  merge on a GPU with weak compute may cost more than the draws it saves.
  Phase F must be measured on the low end, and "raster keeps per-mesh draws on
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

**One renderer, four render modes, and one path by which scene geometry becomes
GPU data.** Corrected 2026-08-01: an earlier revision drew PathTracing and the
Rasterizer as sibling modules, which the hybrid decision
(`vulkan-rt-backend.md` §11.2) makes wrong. Raster owns primary visibility in
every mode, so path tracing is a *mode* of the one renderer rather than a peer
of it.

```
SceneGraph → GpuScene → Rasterizer → RenderMode ┬→ Retro ──────────────┐
                                                ├→ PBR ────────────────┤
                                                ├→ PathTrace → Denoise ┤→ AA (FXAA | FSR)
                                                └→ RTDebug ────────────┘

GpuScene — the ONE compute scene-mesh creation path
    admission -> one compute dispatch -> merged world-space geometry
    stable primitive -> object/material identity, residency, lifetime
```

`RTDebug` is the ray-traced G-buffer kept as a validation instrument
(backlog 7.9), and it is the only surviving piece of the traced primary path.
BLAS/TLAS, the sky environment, NRD and FSR do not disappear — they become
what the PathTrace mode owns, rather than a second renderer's private
machinery.

**And one condition on all three: no Vulkan API outside `graphics/vulkan`.**
This was an unstated assumption until 2026-08-01 and the tree is a long way from
it — 575 `Vk`/`vk` references live in `libs/scene`, four public headers under
`include/reone/scene/` include `volk.h`, and `scene/render/` is in practice a
second Vulkan renderer hosted in the scene library. The measurement and the rule
and the split are under Phase E, which now runs before the raster work rather
than after it, so that F never writes new Vulkan into a library it is about to
leave.

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
  -> consumers                tracer today, raster after Phase F
```

So what Phase F actually deletes is **raster drawing directly from per-mesh
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

Each has its own verification bar, stated with it, because they are not the same
bar and pretending otherwise is how a refactor stops being checkable.

The table is the index, so it lists every phase-sized piece of work in
**execution order** — including the two that were carrying real weight without a
letter. `RenderRegistry`'s removal runs between D and E and keeps no letter,
because A to E are already in commit messages and renumbering them would break
every reference for the sake of tidiness.

| phase | work | bar | state |
|---|---|---|---|
| **A** | remove OpenGL | pixel-identical vs the Vulkan baseline | **done** `3445fdc1`…`df1aa375` |
| **B0** | one source buffer, offsets not addresses | pixel-identical | **done** `a24b1bd3` |
| **B** | extract `GpuScene`, tracer as only consumer | pixel-identical | **done** `6fed967b` |
| **C** | residency in the contract, not the implementation | pixel-identical | **done** `badd9c5f` |
| **D** | admit everything, and see it correctly | traced image changes deliberately, inspected per class; raster untouched except where stated | **done** — grass, dangly, saber, particles admitted; blended transparency **superseded by hybrid**, see below |
| *(registry)* | delete `RenderRegistry` whole — it is branch-only; `SceneGraph` admits directly | **full raster image hash-identical, shadows included**; traced within noise | **done** `8d37449d` |
| **E** | Vulkan containment: no Vulkan API outside `graphics/vulkan` | pixel-identical — it is a relocation, so raster hash-identical and traced within noise | **done** `6dd2965b`, `6a8d280d` |
| **F** | raster consumes `GpuScene` and becomes primary visibility for both renderers, ending in the mega-draw (F5) | **G-buffer byte-identical** — under hybrid this is the contract with the tracer, not a safety check. Shadows change deliberately and are judged by eye; F5 must additionally be **measurably faster** or it is reverted | next |

### What happens next, in order

Written down 2026-08-01 because the correctness work in D pulled a long way
from the phase list and the way back should not have to be rediscovered.

**1. ~~Close Phase D.~~ Closed.** Grass, dangly, saber and particles are
admitted and seen correctly. Grass shadows work — the blades were sub-pixel at
distance, not absent, which took a purpose-built test module to establish and
cost an afternoon of arguing with a screenshot first.

The one defect left open — smoke rendering as a black band — is **superseded
rather than fixed**, by the hybrid decision (`vulkan-rt-backend.md` §11.2).
Its truncating half cannot happen once raster owns primary visibility, because
the background behind a puff *is* the G-buffer and was never at risk. Its other
half, smoke self-shadowing to black under single scattering, is a real lighting
problem that belongs with participating media, not with admission. Everything
built alongside it — the free camera, the TLAS content counts, the traced
G-buffer dump, the testbed module — outlives the defect and is used by every
phase after this one.

**2. ~~Measure the registry copy.~~ It was already measured, on 2026-07-28, and
this plan asked for it again for a year's worth of paragraphs.** The numbers are
in `renderer-registration-plan.md:929` and they change the argument — see
"What the registry actually costs" below. The short version: registration is
**0.445 ms/frame** and the six `drawScene` walks are **1.873 ms/frame**, so the
copy this plan kept pointing at is the *smaller* half by four times. Nothing is
gated on a new measurement.

**3. Delete `RenderRegistry`, and fold three things into that one change**
because they all touch the same code and doing them separately means doing
admission three times:

  - **the removal itself** — `SceneGraph` admits into `GpuScene` directly,
    `drawScene`'s selection becomes ranges, the kill switch and curated
    materials move to stable-id tables, the panel is renamed Objects
  - **the `GpuScene` split, which unblocks Phase E.** It cannot simply move to
    `graphics/vulkan`: `update()` takes a `RenderRegistry &`, and graphics
    knowing about scene types inverts the library dependency. Removing the
    registry is exactly what unblocks it — Vulkan-typed storage goes down into
    `graphics/vulkan`, registry-reading admission stays above. Splitting it here
    rather than in E means admission is written once, and E inherits one fewer
    file to untangle.
  - **backlog 7.7, the isolation fixtures that render nothing.** Not
    housekeeping: per-class isolation is what would have caught the grass
    transpose, and it cannot run today. `--commands-frame` looks like the fix,
    since the cause is `warp` completing its module load after the commands
    file has run.

**4. Phase E, the Vulkan containment sweep**, which this plan had never named:
575 `Vk`/`vk` references in `libs/scene` and four public headers including
`volk.h`. It runs **before** the raster work, reversing an earlier revision —
otherwise F writes its new buffer binding, descriptors and barriers into a
library it is about to leave, and every line of it moves twice.

**5. Phase F, raster consumes `GpuScene`**, against a byte-identical G-buffer
bar. Three of its five original breakages turned out to be current tracer bugs
and were fixed in D. What remains is raster's own lowering over a scene that is
already correct — but the hard bar promotes two things the earlier draft
deferred into prerequisites, and the phase opens with a probe rather than a
change, because raster and the merge compute world position and normals by
different expressions and one of those is a different vector rather than a
different rounding. **The mega-draw is its last substep, F5**, not a phase of
its own: it only means something once F0-F4 have proven a merged draw
byte-identical, and it is gated on a measurement that may cancel it.

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
past that, nobody knows, including whether GL was ever correct. The blocker was
that the toolkit took no arguments, so the failing draw could be reached only by
a person clicking — **fixed** (`a89c6eeb`, `44e11344`): it opens a model and
frames it from the command line, so RenderDoc and the capture harness can both
reach the draw. The defect itself is still open and now diagnosable.



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

## Phase D — everything the tracer should see, seen correctly

Grass and particles first, since admitting them is what forced everything else
in this phase. The second review argued they should stay out and this plan
briefly agreed. That was wrong, and the reason is worth naming because it is the
same mistake twice: **the review treated current behaviour as a constraint when
it was a known bug.**

The decisive fact is that the tracer matches only `RegisteredMesh` — three
`std::get_if<RegisteredMesh>` sites in `rayquery.cpp`, and no handling of
`RegisteredGrass`, `RegisteredParticles` or `RegisteredBillboard` anywhere
(`registry.h:148`, `:162`, `:175`, `:186`). So when this was written, **grass and
particles were invisible to the path tracer**: they cast no shadow, appeared in
no reflection, occluded nothing, and a ray passed straight through a hillside of
grass. That is a correctness gap, not a design choice.

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
- **Particles are smaller than this section first estimated.** The pool is
  `kMaxParticles = 64` per emitter (`graphics/types.h:39`), not a scene budget of
  1000, and danm14ab measures **232 live quads** at frame 1200 with the vents
  filled — 464 triangles. With grass the frame goes 86k → ~90k. The 1000 figure
  was carried in from nowhere and is deleted rather than corrected.

  The divergence it implied is closed. Raster used to draw only the first 64 in
  one call while the registry could pass more; it now issues uniform-sized
  batches over the complete list (`registry.cpp:603-608`), so both renderers see
  the same particles. 64 is the uniform-block capacity, not a scene limit — the
  distinction the earlier text missed.
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

Phase F then accepts that raster keeps a non-merged path for
whatever stays out.

### Coverage is transmission

Admitting a class and seeing it correctly turned out to be different jobs, and
transparency is where they came apart. This is the model the tracer now runs,
written down because it was decided in code and three of its constants disagree
with each other.

**A blended surface is glass with an IOR of 1.0.** Not a composite and not a
special case in the integrator: a material that lets rays pass straight through,
weighted by coverage. Shade it, weight the result by alpha, continue the ray
with `1 - alpha`, and skip it entirely below an epsilon. That is what makes
smoke *lit* rather than pasted on, it is what the tracer already does for any
transmitting surface, and it needs no ordering, no blend state and no second
pass. The alternative — a rasterised composite over the traced image — cannot be
in the BLAS at all, which settles backlog 1.7 the same way admission settled the
rest.

Coverage arrives in three flavours and traversal has to tell them apart
(`slang/tracing/trace.slang`):

| kind | traversal | why |
|---|---|---|
| cutout, `kPtMaskPunchThrough` | binary, commits at 0.5 | a leaf either is there or is not |
| blended, `kPtMaskBlendedCoverage` | commits at any nonzero coverage, transmits the remainder | smoke, and anything raster would have alpha-blended |
| additive, `kPtSurfaceUnlitTransparent` | always commits, passes through, casts no shadow | sabers and glow are emitters, not occluders |

Two details cost real time and neither is guessable from the shader. **Premultiplied
textures carry no usable alpha channel** — coverage lives in the luma and has to be
recovered exactly as `slang/particles.slang:64-71` already does for raster; a
tracer reading `.a` on one of those samples zero and the surface disappears.
And **the classification happens at admission**, in `classifyParticles`
(`rayquery.cpp`): additive blending takes the unlit path, everything else is
given blended coverage. Lose that else branch and every particle commits
unconditionally as a black block — which happened once, from a careless
`git checkout --`, and read convincingly as a shading bug.

**The open question is how many transmitting hits a ray may cross, and the three
answers do not agree:**

| path | limit |
|---|---|
| additive pass-through | `kMaxPassThroughSteps = 16` |
| blended transmission, primary and bounce | `kMaxTransmissionSteps = 12` |
| shadow ray | **uncapped** — every candidate, until transmittance < 1e-3 |

A camera ray through the Dantooine plume stops after twelve layers. A shadow ray
from the same point walks all 232, multiplying `(1 - alpha)` at each one, and
reports the surface fully occluded. So the smoke is lit by rays that see a
twelfth of the volume and shadowed by rays that see the whole of it — the
leading explanation for the black band, and consistent with the measurement:
`diff_factor` 1.0 and a valid `view_z` in that band, with traced diffuse at
0.00003 against 0.05318 on the character beside it.

Underneath the asymmetry sits a question the caps cannot answer. Single
scattering with exact attenuation and nothing scattering back in gives black by
construction; real smoke reads bright because of multiple scattering this
integrator does not have. Making the three limits agree is necessary. It may not
be sufficient, and if it is not, the answer is a scattering approximation rather
than a larger number.

Two consequences live in the backlog rather than here, and both are worse than
they look: blended hits must not write the denoiser guides (**1.12**), because a
thin quad has neither the depth nor the motion of the surface behind it, so NRD
reprojects from the wrong place; and the non-finite values in **1.13** compound
with that, since a NaN written into a guide spreads across the frame instead of
staying in its pixel.

## `RenderRegistry` goes away, and that reshapes what follows

Decided 2026-07-31, and it supersedes the phase list below.

**The scope rule, decided 2026-08-01: anything called a registry that is not on
`master` is removed.** `scene/registry.{h,cpp}` is branch-only — created by
`c536ac72` on `path-tracing`, 1,047 lines, absent from `master` — so it goes
whole rather than being pared down. `graphics/meshregistry` and
`graphics/textureregistry` are master's and are untouched;
`graphics/shaderregistry` already went with OpenGL.

That rule settles five consumers this section had not accounted for, which an
audit turned up before any code was written:

| consumer | where it goes |
|---|---|
| `WalkmeshSceneNode` and `TriggerSceneNode` registering `RegisteredMesh` under `RenderCategory::Debug` (`node/walkmesh.cpp:71`, `node/trigger.cpp:114`) | **deleted with the registry.** Phase A's note that walkmeshes "belong to the room, door and placeable game APIs" is about the walkmesh *data*, which is untouched — the debug *render* path is branch-only and carries no game state |
| `RegisteredAABB`, `RegisteredDebug` (`registry.cpp:270`, `:275`) | deleted with it, as this plan already assumed but had not verified were gone |
| `traversalCount` (`registry.h:365`, logged at `pipeline/vulkan.cpp:1666`) | a statistic about the registry; dies with the thing it counts |
| `IRenderPipeline` taking `RenderRegistry &` (`render/pipeline.h`) | the parameter drops from the interface and both implementations |
| **`RenderRegistry::TraceClass`** | **the one that must survive.** A nested type of the doomed class, read by the tracer (`rayquery.cpp`) and the editor (~10 sites), and *persisted* through `loadTraceClasses` as an integer. It moves to the tracer beside curated materials, and the persisted values must keep resolving or be migrated deliberately |

**The bar.** Nothing in this change alters a draw: raster keeps drawing the same
meshes with the same matrices, only the records come from `GpuScene` instead of
the registry. So no arithmetic changes and the bar is the strongest in this
document — **the full raster image hash-identical, shadows included**, and the
traced image within noise. Anything less means something moved that should not
have.

`RenderRegistry` was always temporary — a per-frame copy of the scene made for
rendering, which is precisely the job `GpuScene` now does. Keeping both means
building the same snapshot twice a frame: nodes push `RegisteredMesh` records
carrying a copied `Material` and copied bone vectors, and admission then reads
them to build `SceneObject`. So **nodes should admit into `GpuScene` directly**
and the registry should disappear rather than be demoted.

The objection to that was that the registry is not only a copy: `drawScene`
(`registry.cpp:519`) owns raster's frustum and distance policy, pass and
category filtering, ordering, and the debug kill switch. **A mega-draw dissolves
most of it.** One draw over the merged buffer has no per-object selection to
make; the tracer already bypasses `drawScene` entirely and says so
(`rayquery.cpp:968`). What survives is smaller than a registry:

- **pass partitioning** — shadow, opaque, transparent are *ranges*, which is
  what `Region` and Phase C's residency classes already describe
- **transparency ordering** — a sort over ranges, not per-draw selection
- **the debug kill switch** — an admission filter, one flag

**Culling is not what is being given up.** This project already measured it:
caching culling removed roughly 9000 frustum tests per frame and changed frame
time by *nothing*, because an AABB-frustum test costs tens of nanoseconds. At
86k triangles the GPU does not need the help. **CPU overhead is raster's actual
problem** — per-draw material binding, descriptor churn, and the registry copy
itself — and that is what one draw removes.

And this is not a bet on the mega-draw winning. Phase B0 already put source
geometry in one buffer addressed by offsets, which the plan noted is "what a
raster mega-draw wants anyway — a shared buffer bound once with per-draw
offsets". So if the merge does not pay on weak hardware, the fallback is N draws
over **the same `GpuScene` records**, not a resurrected registry. Raster staying
permanent for old hardware needs per-object *records*, which `GpuScene` has; it
does not need the selection machinery.

```
SceneGraph  --nodes admit-->  GpuScene   (records + merged buffers + regions)
                                 |
                    +------------+------------+
                    |                         |
              tracer: BLAS/TLAS        raster: one draw over ranges
                                       (or N draws over the same records)
```

Sequencing rests on one measurement, which is step 2 of "What happens next"
above and is not restated here. Phase C was the other prerequisite and is done —
ranges only mean something once residency is in the contract.

### What the registry actually costs — measured 2026-07-28, not estimated

Folded in from `renderer-registration-plan.md:905-1049` on 2026-08-01, because
this plan spent four revisions calling for a measurement that already existed
and reasoning from the wrong half of it. OpenGL, `danm14ab`, `--pbr 1`, wall
clock differenced between 300- and 900-frame runs with a warm-up discarded.

| | ms/frame |
|---|---:|
| `a7b2bf0f`, immediately before the registry | 4.51 |
| `c536ac72`, the registry | 5.62 — **+1.11 ms, +25%** |
| `72fb544e`, after material flattening | 5.01 — recovered 0.61 ms |

And inside the Graphics slot at that HEAD:

| phase | ms/frame |
|---|---:|
| snapshot registration (`renderScene`) | 0.445 |
| **the six `drawScene` walks** | **1.873** |
| `_objects.clear()` destruction | 0.020 |
| `resetFrame` bookkeeping | 0.002 |

**The draw walk is four times the registration, and this plan had it backwards.**
Every version of this document argued the *copy* was the thing worth deleting —
"roughly a thousand objects a frame, against a 2.3 ms update slot". The copy is
0.445 ms. The six walks over `_objects`, one per pass, are 1.873 ms. So the
prize is deleting `drawScene`, not deleting the copy, and that is an argument
for ranges over per-object selection rather than an argument about snapshot
construction.

`RegisteredObject` is **488 bytes**, so 1508 entries are ~0.70 MiB built and
destroyed per frame, and the variant makes a three-field billboard pay the
largest alternative's width.

**Ruled out, with numbers.** These are recorded because negative results are
what stop a guess recurring:

- the 653 dangly position vectors: **0.065 ms** — real, and an order of
  magnitude too small
- the dead `drawnPasses` clear: **0.002 ms**
- **caching the per-root cull test: no effect at all.** The pre-registry code
  culled once per root per frame; the registry tests per entry per pass, ~9000
  against ~200. Removing those calls moved frame time by nothing, because an
  AABB-frustum test costs tens of nanoseconds. *Reasoning from a ratio of call
  counts is how that hour was lost.*

### Which copies are real, and which this plan invented

Also from the registration plan (`:988-1034`), and it corrects a second thing
this document repeats:

- **The `Material` copy is real and dominates** — but not because of geometry.
  `Material::textures` is an `unordered_map`, so `registerRender` builds one
  hash table on the stack and the entry copy-initialises a second
  (`node/mesh.cpp:254-268`, `registry.cpp:118`). Two per entry, ~1500 entries.
  **The fix is flattening `textures` into fixed slot indices, which is the
  bindless change the mega-draw (F5) wants anyway — not a change to when the registry is
  filled.**
- **Dangly positions are moved, not copied.** `RegisteredDeformation` is taken
  by value and `std::move`d, and the call site passes a prvalue. There is
  nothing here to save, despite 653 of them.
- **Skinned bones do copy** — `RegisteredSkin {_bones, _prevBones}`, two
  vectors, two allocations per skinned node per frame. The entry owns its bones
  rather than referencing them; a span fixes it once stable ids make a node safe
  to reference for a frame.
- **`drawScene` re-copies instance arrays on every pass that selects them** —
  grass into `visible` and again into 256-cluster batches, particles into
  `visible`. That is in the *draw* walk, and it scales with the denser grass
  Phase D just landed. Wants a span plus a `(first, count)` pair.

### Structural facts the removal has to handle

Four things in the registration plan that this document never carried, and each
is a decision the removal cannot avoid:

- **A shadow-casting mesh registers twice.** Once from `registerRender` with its
  real material, once from `registerShadow` with a `DirLightShadow` /
  `PointLightShadow` material (`node/mesh.cpp:345-362`) — *a pass wearing a
  material's clothes*. Entry count is inflated by every caster, and anything
  keyed off entries must decide which of the two is real. One object should be
  one entry, with the pass selecting a shader.
- **Skinned meshes never cast shadows at all.** `shouldCastShadows`
  (`node/mesh.cpp:201-213`) excludes skin meshes on creatures, so no character
  is shadowed. What *does* silently lose its deformation is a dangly or saber
  caster. Fixing it is not admission work: `slang/shadow.slang:25-33` has a
  single vertex stage taking `POSITION` and `localUniforms.model`, so deforming
  shadows need new entry points and pipeline keys. **Phase E4 shadows from real
  geometry inherits this.**
- **Culling is now per light for the frustum, and still per view for distance.**
  `49496d2f` fixed the half that mattered: `graph.cpp:448-455` builds a frustum
  per cascade and per cube face from `_shadowLightSpace[i]` and passes them as
  `VisibilityPolicy::shadowFrusta`, so a caster outside the *view* frustum is no
  longer dropped from the shadow map. Draw-distance culling was not converted —
  `gpuscene.cpp:66-71` still measures against `visibility.drawDistanceCamera`,
  which is the view camera even in a shadow pass. The remaining half must not be
  silently reproduced when selection becomes ranges.
- **`SceneGraph::_nodes` never shrinks.** It holds a `shared_ptr` to every node
  ever created, is never erased from and never read; `clear()` leaves it
  untouched. Nothing in a scene is destroyed until the `SceneGraph` dies. Under
  a snapshot that is a memory bug on its own schedule — under retained
  registration it would be a hard prerequisite. It is also why "nodes admit
  directly" must not quietly become a retained protocol.

**And the snapshot stays a snapshot.** The registration plan argues this at
length (`:147-231`) and it is the load-bearing conclusion: retained
registration conflates stable identity, persistent backend caches, and a
registration protocol. Ray tracing needs the first two, which Phase C delivered;
the third buys nothing and costs an invalidation contract the scene graph cannot
currently honour. Backend caches are **validated, not notified** — an entry
carries a content version, the cache rebuilds on mismatch and releases after N
absent frames.

### Composition, for sizing anything here

`danm14ab`, frame 310, from `formatRegistryCounts`:

| | registered | drawn opaque | drawn transparent |
|---|---:|---:|---:|
| rigid | 744 | 237 | 38 |
| skinned | 61 | 13 | 0 |
| dangly | **653** | 7 | **544** |
| saber | 4 | 0 | 4 |
| emitters / particles | 41 / 55 | — | — |
| grass | 1 node / 1482 clusters | 1 | — |

**544 of the 586 transparent draws are dangly**, so the transparent pass is very
nearly nothing but foliage, and dangly outnumbers skinned ten to one.

### Where the registry's other jobs go

The registry panel and the per-object overrides look like blockers and are not.
The code already half-agrees:

| | lives where afterwards |
|---|---|
| display names, hierarchy | **SceneGraph, already** — the panel resolves `graph.nameText(entry.nameIds.model)`; the registry only carries the ids |
| what exists this frame, and where | `GpuScene` records; `RegistryCounts` becomes counts over admission |
| per-object debug state — disable, overrides | a side table keyed by **stable id**, consulted at admission |
| per-category material overrides | `PtCategoryOverride[9]` in graphics options (`options.h:154-170`), applied during the tracer's material lowering — never registry state, unaffected |

### The panel is far more coupled than that table admits

Audited 2026-08-01. `editor.cpp` carries **64** registry references and the
table above accounts for about four of them. Six couplings that the removal has
to answer, one of which contradicts the table:

- **`drawnPasses` exists only because `drawScene` writes it.** Five sites
  (`registry.cpp:545`…`:644`), cleared in `resetFrame`, read by the panel as
  pass slots and by the "Hide fully culled" filter. **Ranges produce no
  per-object drawn state, and a mega-draw produces none at all.** The table
  claims "keep the admitted/drawn distinction; it is still real once raster
  draws ranges" — that is asserted without a mechanism, and the mechanism is
  exactly what is being deleted. Either the panel loses the distinction, or
  something reconstructs it deliberately. **Decide before deleting, not after.**
- **`graphics::Material::curatedIndex`** (`material.h:93`) is an `int` indexing
  the *scene* layer's curated storage, resolved through
  `RenderRegistry::curatedIndex` with exact and `model/*` wildcard keys. A
  graphics type holding an index into a scene-owned table is a layering
  inversion that predates all of this and comes due now.
- **`Editor::_materialEdit` is a `RenderRegistry::CuratedMaterial` by value**
  (`editor.h:99`), a live working copy held across frames, and `editor.h`
  includes `scene/registry.h`. Editor state is typed on the doomed class — the
  same problem as `TraceClass` and it needs the same answer.
- **`drawMaterialEditor(scene::RenderRegistry &)`** takes the class by
  reference; the signature changes with it.
- **`registry.skyRoom()`** is read by the panel (`editor.cpp:274`) to mark the
  sky room. The corrected-shape section moves sky classification out, but never
  notes the panel is a consumer.
- **The panel deliberately reads the previous frame's completed snapshot**
  (`editor.cpp:1099`), and it iterates `registry.objects()` as raw variants,
  rendering per-alternative detail — material name, pass slots, cull root, sky
  flag, curated class, per-entry enable. It needs an equivalent enumeration over
  `GpuScene` records with the same completed-frame semantics, not a count.

**The kill switch is the one that proves the point.** Its own comment says it is
keyed by `SceneNodeId` index "so it survives the per-frame re-registration" —
which is an admission that the state belongs to the object's identity, not to
the snapshot, and sits in the registry only because that is where admission
happens today. Moving it to a stable-id table is a correction, not a
workaround.

**So Phase C's stable identity is load-bearing for the tooling, not only for the
tracer.** If the audit concludes identity cannot be made stable across a module
transition, this is where it bites first: the kill switch already requires an
identity that outlives re-registration, so the requirement predates the
refactor.

**Curated materials are the third resident, and the largest.**
`RenderRegistry::CuratedMaterial` (`registry.h:272-294`) holds the per-node
`TraceClass`, albedo multiplier, roughness and metallic modes with their
parameters and weights, and emission mode — keyed by *model and node name*
(`curatedFor`, `setCurated`), persisted through `loadTraceClasses`, and edited
by `Editor::drawMaterialEditor(RenderRegistry &)`. Name-keyed, not
snapshot-keyed, so like the kill switch it is identity state boarding in the
wrong house. It moves beside the kill switch; the tracer keeps consuming it
during material lowering, which is where it is already applied.

### The tool gets renamed with it

"Registry" is the name of a thing that will not exist. **Decided 2026-08-01: it
becomes the Scene viewer.** An earlier revision proposed "Objects", on the
grounds that `isObjectEnabled`/`setObjectEnabled` already use that word — but
once `SceneGraph` admits directly, what the panel shows *is* the scene, and
naming it after the objects it happens to list describes the old snapshot rather
than the new source. The renames that follow:

| now | after |
|---|---|
| `ImGui::Begin("Registry")`, the menu item | **"Scene"** |
| `Editor::drawRegistry`, `_showRegistry`, `_registryFilter`, `_registryHideFullyCulled` | `drawSceneViewer`, `_showSceneViewer`, `_sceneViewerFilter`, … |
| `_registryScene` — which scene graph the panel is looking at | `_sceneViewerGraph`, because "scene" now means the panel and the old name would read as its own selector |
| `RegistryCounts`, `formatRegistryCounts` | admission counts |
| `registeredCounts()`, `drawnCounts()`, `drawnCountsByPass()` | see the coupling audit above — the admitted/drawn distinction has no mechanism once `drawScene` goes, and that has to be decided rather than renamed |

The rename also changes what the panel *is*, not just what it is called. Reading
"the previous frame's completed render snapshot" was a property of the registry;
a scene viewer reads the scene, which is live. Whether the panel keeps the
one-frame lag deliberately or stops needing it is part of the same decision.

Practical note: renaming an ImGui window changes its `imgui.ini` key, so the
saved layout, size and dock position reset to the code defaults. That reads as
the panel breaking. Delete `build/bin/imgui.ini` and check what a first run
actually shows before believing a layout regression.

## Phase E — Vulkan containment

**No Vulkan API outside `graphics/vulkan`**, as stated in the end state.

**It goes before F, and an earlier revision had that backwards.** The argument
for putting it last was that a pure relocation's pixel-identical bar is only
meaningful over code that has stopped changing, and that relocating
`rayquery.cpp` while the raster work was still editing it would destroy both
phases' verification. That is a real hazard and it is the smaller one. The
larger one is that F writes *new* Vulkan — buffer binding, descriptors, a
widened barrier — and if containment has not happened yet, every line of it
lands in `libs/scene` and has to be moved again. **Do not write new code into a
building you are about to evacuate.** Relocating first also means F edits code
that is already where it belongs, so the two never touch the same file in the
same phase.

### The measurement

| file | `Vk`/`vk` references |
|---|---:|
| `scene/render/pipeline/rayquery.cpp` | 243 |
| `scene/render/pipeline/vulkan.cpp` | 191 |
| `scene/gpuscene.cpp` | 73 |
| `scene/render/pipeline/nrddenoiser.cpp` | 59 |
| `scene/render/pass/vulkan.cpp` | 7 |
| `scene/render/pipeline/fsrupscaler.cpp` | 2 |

575 references inside `libs/scene`, against 4,652 lines of actual
`graphics/vulkan`. Four public headers under `include/reone/scene/` include
`volk.h`, so the leak is in the *interface*, not only the implementation.
`scene/render/` is in practice a second Vulkan renderer living in the scene
library, and `GpuScene`'s 73 references are 13% of the problem.

### It is not a file move, and that is the whole difficulty

The obvious version — drag `scene/render/*` into `graphics/vulkan` — **inverts
the library dependency**. Those files read `RenderRegistry`, `SceneNode`,
`Material` and the scene graph; graphics knowing about scene types is precisely
what the layering forbids. So each file splits the same way `GpuScene` already
has to:

```
scene/            reads scene types, decides what to draw and with what
  |  (a plain, Vulkan-free interface)
  v
graphics/vulkan/  owns buffers, descriptors, pipelines, barriers, commands
```

The split is the work; the move is the easy part. Sized by where the references
sit:

| file | refs | shape of the split |
|---|---:|---|
| `render/pipeline/rayquery.cpp` | 243 | the largest. Trace resources, AS build and dispatch go down; admission, classification and curated material lowering stay up |
| `render/pipeline/vulkan.cpp` | 191 | frame ordering and pass structure stay up; images, render passes and barriers go down |
| `gpuscene.cpp` | 73 | already identified — Vulkan-typed storage down, registry-reading admission up. Unblocked by the registry removal, which is why that runs first |
| `render/pipeline/nrddenoiser.cpp` | 59 | NRD wrapper; almost entirely a graphics concern already |
| `render/pass/vulkan.cpp` | 7 | nearly clean |
| `render/pipeline/fsrupscaler.cpp` | 2 | nearly clean |

**The four public headers are the real test.** `include/reone/scene/gpuscene.h`,
`render/pass/vulkan.h`, `render/pipeline/rayquery.h` and
`render/pipeline/vulkan.h` include `volk.h`, so the leak is in the interface and
every translation unit that touches scene rendering inherits it. The phase is
done when no header under `include/reone/scene/` includes a Vulkan header —
that is a grep, which makes it the one bar here that cannot be argued with.

### Order, and what makes it safe

Smallest first, so the mechanism is proven on files where a mistake is cheap:
`fsrupscaler`, `pass/vulkan`, `nrddenoiser`, then `gpuscene`, then
`pipeline/vulkan`, then `rayquery`. One file per commit, pixel-identical each
time. Anything else makes a failure impossible to localise, and this is a phase
where a subtle barrier or lifetime mistake will present as an intermittent
corruption rather than a clean break — see the hazard note about VMA
allocations outliving the device.

Fold in here rather than earlier: `glToVulkanClip` (`pipeline/vulkan.cpp:1319-1334`)
and the negative-height viewport (`:627-634`), both Vulkan-native cleanups that
change matrices, so each is its own commit with its own capture check.

## Phase F — raster consumes `GpuScene`

Rewritten 2026-08-01, then reconciled with the hybrid decision the same day.
The bar hardened to a byte-identical G-buffer, which turns several things the
earlier draft deferred into prerequisites; Phase E has since put the Vulkan
surface where this phase's new code belongs, so that constraint is satisfied
rather than pending.

Of the five breakages the first draft listed, four are gone. Three were tracer
correctness bugs fixed in Phase D — dangly and saber frozen at base pose
(`c1469287`) chief among them, a bug that had stood since the tracer existed and
took one commit once someone noticed the displaced positions were already being
handed to it. The fourth, "merged shadow draws lose frustum rejection", stopped
mattering when caching culling removed ~9000 frustum tests per frame and moved
frame time by *nothing*. What remains is raster's own lowering over a scene that
is already correct.

### What hybrid changed about this phase

Reconciled 2026-08-01. This phase was written before the hybrid decision
(`vulkan-rt-backend.md` §11.2) and read as an optimisation justified by CPU
overhead on weak hardware. It is not that any more.

**Raster owns primary visibility for both renderers.** Camera rays are not
traced; the tracer does transport starting from raster's G-buffer. So this phase
is not optional and does not depend on the mega-draw paying off — one geometry
path is a requirement, and if the merge turns out not to pay on weak hardware
the fallback is N draws over the same `GpuScene` records, not a second path.

**And the byte-identical bar stops being a safety check.** It was "prove the
relocation changed nothing". Under hybrid the G-buffer raster produces *is the
tracer's input*, so the bar is the contract between the two renderers: every
traced frame is only as correct as the G-buffer under it.

Three pieces of work follow from hybrid that the steps below do not mention, and
they belong at the end of this phase rather than in it:

- **the traced primary path is retired**, except the debug shader that emits a
  ray-traced G-buffer for validation — the `RTDebug` mode, backlog 7.9. Delete
  the visibility walk without pinning that down first and the ability to check
  the merged scene against raster goes with it.
- **coverage-as-transmission goes with it**, in its primary-ray half only.
  Shading a blended surface and continuing with `1 - alpha` describes a camera
  ray walking a stack of quads, and there is no camera ray. Shadow rays and
  secondary bounces still cross smoke and keep the model.
- **transmissive surfaces have to be re-homed**, and how they are lit once
  raster composites them is the open question hybrid creates. It is not answered
  here.

### The bar, and why it has two halves

**The rasterised G-buffer must be byte-identical before and after.**
`np.array_equal` on the `--dumptargets` `.npy` files: `g_buffer_diffuse`,
`g_buffer_eye_normal`, `g_buffer_lightmap`, `g_buffer_self_illum`,
`g_buffer_depth`, and motion. Raster is bit-exact by construction once `--dev 0`
suppresses the frame-time readout, so there is no tolerance to negotiate here —
a differing pixel is a real difference and the change is wrong.

**Shadows are the deliberate exception.** Proxies go away and shadowing moves to
real geometry, so shadow maps and everything lit through them change on purpose.
Shadows are judged by eye, the G-buffer by hash. Keeping the two bars apart is
what makes the phase checkable; one combined "looks right" bar would hide a
G-buffer regression behind an intended shadow change.

### F0 — is byte-identical reachable at all? Answer before building anything

Three places where raster and the merge compute the same quantity differently.
Two are possible rounding differences; **one is a different vector**. Probe all
three in the existing vertex stage — compute both forms, `asuint` XOR them,
write a flag to a target — and count nonzero pixels on danm14ab before writing
any of the switchover.

| quantity | raster | the merge | kind of difference |
|---|---|---|---|
| world position | `mul(localUniforms.model, objectPos)` (`pbr_model.slang:59`) | three `dot(row, float4(p,1))` (`skin.slang:86-90`) | summation order and FMA contraction — possibly bits |
| **normal** | **inverse transpose**, `mul(n, (float3x3)modelInv)` (`lib/geometry.slang:64-66`) | **plain `M * n`**, `transformDir` (`skin.slang:287-289`) | **a different vector under any non-uniform scale or shear** |
| normalisation | `normalize()` | `safeNormalize`, `v * rsqrt(dot(v,v))` (`skin.slang:99-103`) | `rsqrt` may lower to an approximate instruction |

Plus one shape difference: raster's tangent frame is computed **only** when the
normal-map or bump-map feature bit is set and is `float3(0)` otherwise
(`pbr_model.slang:64-70`), while the merge fills it unconditionally. Wherever
those disagree, `g_buffer_eye_normal` differs on every normal-mapped surface.

**When a probe is nonzero, the merge moves — not the bar.** The tracer has no
bit-exactness requirement and the merge is the newer code. For the normal that
means `SceneObject` gains the inverse (or its 3x3 transpose) and the merge
adopts the inverse transpose. If a probe cannot be driven to zero, that is a
finding worth stopping on, not something to work around by loosening the
comparison.

**A tracer correctness question falls out of the normal row, and it is not a
Phase F question.** If `M * n` is wrong for non-uniformly-scaled objects, then
the path tracer has been shading those objects with wrong normals for as long as
the merge has existed. Measure whether any admitted object actually has
non-uniform scale before claiming it either way: if every transform is a rigid
motion plus uniform scale the two conventions agree and this is only about bits.
Either way it belongs to Phase D correctness, recorded here because Phase F is
what exposed it.

### F1 — `MergedVertex` must carry object-space position

`opaqueFragment` — the G-buffer writer itself — hashes object-space position for
the hashed alpha test (`pbr_model.slang:280`). `MergedVertex` has no such field.
The earlier draft called this "a raster lowering problem for Phase E proper" (as
the raster phase was then lettered) and
left it late; under a byte-identical bar it blocks the first step, because any
dithered surface differs immediately.

144 → 160 bytes. `skin.slang` declares the struct independently of the C++ header
and the `static_assert`s are the only ABI guard, so both sides and every assert
move together.

### F2 — getting merged geometry into a raster draw

The merged buffers carry `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` only
(`gpuscene.cpp:243-244`), and the post-merge barrier names AS-build and
ray-tracing reads. Both need widening for a raster consumer.

**Prefer programmable vertex pulling** — bind the merged buffer as a
`StructuredBuffer<MergedVertex>` and index by `SV_VertexID` — over adding
`VERTEX`/`INDEX` usage and going through vertex input. No binding descriptions,
no attribute descriptions, no format plumbing in the pipeline key, one
declaration of the vertex layout instead of two, and the mega-draw becomes
trivial later.

### F3 — the static opaque set, and nothing else yet

World-space vertices, `model` = identity, every other part of `LocalUniforms`,
the material path and texture binding unchanged. This step changes **where
vertices come from and nothing else**, which is exactly what makes a
byte-identical result meaningful.

**Do not let merged raster draws cover grass and particles.** Phase D admitted
them for the tracer, while raster draws them through `executeDrawGrass` and
`executeDrawParticles` with their own shaders — cover them here and they render
twice.

**Skinned geometry is a separate later step.** Raster skins in the vertex shader
from a bone palette; the merge skins in compute. Those must also be shown
bit-identical, and the same rule applies when they are not. Attempting static
and skinned together makes a failure impossible to localise.

Motion vectors carry the same question: the VS builds `prevClipPos` as
`prevViewProjection * (prevModel * prevObjectPos)` (`pbr_model.slang:75-76`)
while the merge already holds `prevPosition` in world space.

### F4 — shadows from real geometry

Only once the G-buffer is byte-identical. Admission takes Opaque and Transparent
only (`rayquery.cpp:517`, `:1112`), so shadow-only proxies sit outside the merge
while raster's shadow pass draws exactly them. Shadow from real geometry and
delete the proxies rather than plumbing them through. Proxies exist because four
cascades and six cube faces of real geometry were expensive on 2003 hardware,
which is no longer a constraint.

This is where the image changes on purpose.

### F5 — the mega-draw, if it pays

The payoff the rest of this document is justified by, and until now the only
part of it never planned. The opening section argues raster consuming `GpuScene`
"matters most exactly where the machine is weakest: a mega-draw over merged
geometry against 1048 per-mesh draws", the registry section argues a mega-draw
dissolves `drawScene`, and every earlier draft excluded it — so it was the motivation
for three sections and the subject of none.

**The phase does not hinge on this substep.** Under hybrid, raster is primary
visibility whether or not one draw beats many; F5 only decides how those draws
are issued. **It is the last substep, not a phase of its own.** F0-F4 put
merged geometry into a raster draw and prove it byte-identical; only then is
there anything to collapse. Bindless arrives from Phase E having put the
tracer's descriptor-indexing machinery somewhere shareable.

#### The gate: measure before building

**This project has already been wrong about exactly this.** Culling moved from
~200 frustum tests per frame to ~9000; caching it removed the calls and changed
frame time by *nothing*. A ratio of call counts is not evidence. So the gate is
two measurements, not an argument:

- step 2 of the ordered list — is the per-object `Material` and bone copy a
  visible slice of the 2.3 ms update slot?
- what does 1048 draws actually cost in CPU time? Per-draw descriptor writes,
  uniform ring pushes and pipeline binds are the claim; time them.

If the answer is that per-draw CPU cost is small, **this phase should not
happen**, and "raster keeps per-object draws over merged ranges" is a correct
outcome rather than a failure. The low-end note in the opening section says the
same thing from the other direction: a per-frame compute merge on weak compute
may cost more than the draws it saves.

#### What one draw actually requires

**1. Per-primitive materials, which means bindless — and raster has none.**
Today each draw binds its own texture set (`acquireTextureSet`, per draw) and
pushes its own `LocalUniforms`. One draw covering many materials means the
fragment stage looks the material up per primitive and indexes textures by id.
The tracer already does this, and **it is the only thing in the tree that does**:
`VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT` with a variable descriptor count
appears at `rayquery.cpp:138-143` and nowhere else. Raster adopting it is the
bulk of this phase, and it is another argument for F first — the machinery has
to live somewhere shareable rather than inside the tracer's pipeline.

**2. `InstanceMaterial` is not the table raster needs.** The corrected-shape
section already settled this: it carries trace-only surface types and curated
trace operations, and lacks `color`, `ambientColor`, the env-map slots, fog and
blend/cull state that raster reads from the original `Material`. So either
raster gets its own per-primitive table or the shared one widens. **Decide
explicitly** — sharing it by default is how the first draft of this plan went
wrong.

**3. Feature bits stop being uniform across a draw.** `pbr_model.slang` says so
in its own header comment: *"Material features remain runtime branches — they
are uniform across a draw."* That stops being true. They become per-primitive
loads, and whether the resulting divergence costs more than the draws saved is
an empirical question belonging to the gate above.

**4. Pipeline state cannot vary within a draw, and this is the part that
actually determines the draw count.** Blend and cull are pipeline state, not
shader state. But with merged geometry and vertex pulling, most of the current
key collapses: the vertex bindings and attributes vanish (one layout), and the
vertex entry vanishes with them (everything is already transformed, so `static`,
`skinned`, `dangly` and `saber` stop being separate stages). For the opaque
G-buffer pass the fragment entry is fixed and blend is fixed, so **only cull
varies** — `material.faceCulling.value_or(FaceCullMode::Back)`.

That is the concrete claim to test: **1048 draws becomes one per cull mode, a
handful.** Not literally one, and the section that says "one draw" everywhere
should be read as "a handful" from here.

**5. Transparents do not collapse the same way.** Blend mode varies per material
there, so they partition by (blend, cull) and keep the ordering sort over ranges.
The mega-draw is an opaque-pass claim; transparents get whatever falls out.

#### Bar

Pixel-identical for the opaque G-buffer: same geometry, same materials, only the
binding model changed. Shadows are already settled by then.

**And a second bar this phase alone has: frame time must actually improve.**
Every other phase in this document is verified by things *not* changing. This
one exists solely to make something faster, so a version that is
pixel-identical and no quicker has failed and should be reverted rather than
kept for tidiness.

#### What it deletes

Per-draw texture sets, per-draw `LocalUniforms` pushes, and — once skinned
geometry consumes the merged stream — the per-draw bone palette upload
(`pass/vulkan.cpp:274-297`) together with the CPU palette build
(`node/mesh.cpp:327-354`). That last deletion is the one the target section
promised and is the clearest signal the phase worked.

### Not in this phase

- **`offMaterial`**, which still has no `SceneObject` field. Walkmesh debug
  geometry is its only consumer and deleting that remains the better answer than
  widening the vertex.
- **The 575-reference containment sweep** — that is Phase E, and it has already happened by the time this phase starts.

`pipeline/vulkan.cpp` remains the awkward one: it owns frame ordering and returns
before every raster pass in path-tracing mode (`:1337-1373`), so a core consumed
by both needs a precise update point and per-consumer barriers. Its
`glToVulkanClip` rewrite (`:1319-1334`) and the negative-height viewport
(`:627-634`) are real Vulkan cleanups that belong with Phase E.

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

**Raster is bit-exact; path tracing is not.** These need different bars, and
conflating them produced most of this refactor's false alarms.

**Raster: byte-identical, full stop.** The apparent 0.02% of differing pixels
was entirely the frame-time readout drawn into the corner of the captured image
("234.4 FPS 4.27 ms" against "241.3 FPS 4.14 ms"). Exclude it and three runs
each of danm14ab, ebo_m12aa and danm13, in both PBR and retro, hash the same —
six groups, no exceptions. Wall-clock readouts are now suppressed under
`isCaptureRun`, alongside the fixed timestep and the seeded generator, so the
bar for any raster change is **hash equality, and anything else is a
regression.**

**Path tracing: genuinely nondeterministic**, 8–64% of pixels between identical
runs, because acceleration-structure build order is not reproducible and the
bounce loop's accumulation is order-dependent by design. No masking fixes that,
and it is why the rest of this section exists.

So the bar becomes **the change must be indistinguishable from run-to-run
noise** — but getting that right took four false failures, and the method
matters more than the bar:

1. **A single run-pair is not a noise floor.** It varies 1.6× on raster and
   8.4→16.3% on tracing. Estimating the floor from one pair reported failure
   three times for changes that altered nothing.
2. **A stored baseline is one sample of a wide distribution.** Comparing runs
   against it is bounded by wherever that single draw landed. In B0 every new
   run fell on the same side of the stored image, which looked like a
   systematic regression at 1.6% probability by chance — and wasn't.
3. So compare **distribution against distribution**: capture N times before the
   change and N times after, and ask whether the cross-boundary spread exceeds
   the within-group spread. B0's apparent 0.009 luminance shift sat inside the
   0.017 scatter of the *unmodified* build measured against itself.

Mean luminance is still the metric that would catch a real change, but only
against a distribution. Stored baselines remain useful as a smoke test — a
gross regression will show — and are not evidence at this precision.

Step 4 cannot be compared at all, and should be measured rather than compared.

## Not in scope

**For the pixel-identical phases — A, B0, B, C, E, and Phase F's raster half —
no behaviour changes, no features, no performance work.** If something looks
wrong mid-refactor, write it down rather than fixing it: a refactor that also
changes behaviour cannot be verified by comparing images, and the whole
verification scheme above depends on that separation holding.

**Phase D is the deliberate exception, and it is not a loophole.** Admitting a
class the tracer could not see changes the traced image by definition, so its
bar is different in kind: each class lands in its own commit, is inspected
against the traced image, and leaves the raster baseline hash-identical. That
last clause is what keeps D checkable — a phase that changed both renderers at
once would be verifiable by nothing.
