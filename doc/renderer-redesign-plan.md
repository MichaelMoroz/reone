# The structural track — modularity without CPU creep

`phase-f.md` is the feature track: what the renderer draws and how it looks.
This file is the structural track: what the renderer *is* — where scene state
lives, what the CPU does per frame, what the code that a person reads actually
says. Compiled 2026-08-04 from a four-part architecture survey (object model,
scene↔GPU boundary, Vulkan surface census, GPU representation), the Tracy
measurement of the same date, and the backlog. Steps are named **S0–S6**; the
letter is unused by the G/R/V tracks.

## The goal, and what is deliberately not the goal

**Modularity first.** Two invariants define it, and every step in this file is
judged against them:

1. **Render-side CPU is O(changes), never O(objects) or O(passes).** The
   steady-state frame is: replay this frame's change-log into GPU tables
   (proportional to what moved), then record a fixed set of dispatches and a
   handful of draws against persistent descriptors. A new render feature adds
   a GPU pass, a pipeline and a shader — never a per-object CPU loop, never
   per-frame descriptor churn. Growth in scene content lands on GPU-computed
   indirect counts.
2. **The CPU/GPU seam is a data contract, not a call contract.** Scene state
   is defined once, in Slang, and consumed by every pass. A new consumer needs
   zero scene-side C++ changes. There is deliberately *no* per-frame
   per-object hook to add code to — that hook is how every existing creep
   entry got in (flare LOS per light, settle per mesh, skin rebuild per mesh,
   particle re-registration per tick).

**Not the goal:** recovering the CPU frame. The 2026-08-04 Tracy run settled
this — the scene→GPU pipeline is already ~0.5 ms (R1 and R3 collected that
win); the remaining CPU lives in `SceneGraph::update` self (1.25–1.35 ms,
uninstrumented) and `Game::update` (0.63–0.71 ms), most of which is game
logic this track does not touch. Perf improvements fall out of the structure;
they are not its acceptance test. Also not the goal: a second graphics API.
The RHI is shaped so one *could* exist, but portability justifies nothing
here — logic decoupling does.

**The mode contract is untouched.** Retro stays the fidelity anchor, PBR and
path tracing stay the improvement, exactly as phase F states. Nothing in this
file changes what any mode renders — every step is a pure refactor by
construction, and is held to the instruments that make that checkable: the
upload-hash equality across modes, the shadow-oracle tripwire, byte-identical
raster captures, traced output by distribution.

## What the 2026-08-04 measurement settled

Steady state, `danm14ab`, headless, both modes (`raster2_zones.csv` /
`pt2_zones.csv` in the session scratchpad; two independent runs agreeing
3–8%):

| | raster retro | path tracing |
|---|---:|---:|
| frame | 3.845 ms (260 fps) | 16.86 ms (59 fps) |
| `update` slot | 2.115 | 2.305 |
| `graphics` slot | 1.680 (1.153 = present block) | 14.573 (13.901 = present block) |
| scene→GPU CPU total | **~0.50** | **~0.67** |
| `collectInto` | 0.016 | 0.017 |
| admission prepare | 0.147 | 0.239 |
| merge record+upload | 0.133 | 0.113 |
| skin streams | 0.098 (61 calls) | 0.104 |
| dangly streams | 0.043 (**789 calls**) | 0.047 |
| `SceneGraph::update` self | **1.249 — uninstrumented** | 1.349 |
| `Game::update` self | **0.633 — uninstrumented** | 0.707 |

Consequences for ordering: the biggest attributable per-frame scene costs are
the deformation streams and classification dedup, not collection; the biggest
*unattributed* cost is inside `SceneGraph::update`, which S0 must open before
S3 commits to an order; and the diagnostics skill's calibration line
(`collectInto` ≈0.73, `prepare` ≈0.60) predates R1/R3 and is stale by 46×/4×.

## Relationship to phase F

The two tracks interleave; neither blocks on the other finishing.

| structural step | earliest sensible point | why |
|---|---|---|
| S0 | now | pure instrumentation; G8/G9 benefit from it immediately |
| S1 | now | independent of the raster track; everything after it consumes it |
| S2 | after G8 lands | G8 is the last step that grows the classification/record vocabulary; invert once the schema is quiet |
| S3 | after S2 | the change-log is what the sims write into |
| S4 | after S2, BLAS half after 8.15 | residency partitions the tables S2 creates |
| S5 stage 1 | now, opportunistically | the four builders are Vulkan-internal and step on nobody |
| S5 stage 2 | after V1c | the frame shape (who owns primary visibility) must stop moving before the seam is formalized |
| S6 | trailing | deletions become possible as consumers disappear (V5 unhooks the last `VulkanMesh` user) |

Three phase-F items are absorbed rather than duplicated: **R2** (the
translation-layer deletion — S2 *is* R2, with the sync design made concrete),
backlog **3.5** (dangly into the merge kernel — S3), and backlog **4.9**
(Slang in the engine — S1, promoted from P3 to track-blocking because every
later step multiplies shaders and schema consumers). One phase-F constraint
binds hard: **7.9's RTDebug instrument must be pinned before V1c deletes the
traced primary path** — it is the only cross-consumer scene validation that
does not depend on judging an image, and S2/S4 lean on it as hard as G does.

## S0 — attribute the blind spot, arm the guard-rails

The cheapest step and the one everything else is ordered by.

- **Sub-zones** inside `SceneGraph::update` (`updateAnimations`, `refresh`,
  `updateShadowLight`, `prepareOpaqueLeafs`/`prepareTransparentLeafs`,
  flare/particle blocks) and inside `Game::update` (module, objects,
  AI/scripts, GUI, imgui begin). Also `snapshotPreviousFrame` under
  `SceneGraph::render`. Tracy macros compile out when off; there is no cost
  argument against this.
- **GPU timestamps in `IStatistic`** (backlog 2.5). Every GPU-side claim in
  S3/S4 is unscoreable without it; the backlog already calls it the blocker.
- **The scaling fixture** — the no-creep tripwire. A headless run:
  `warp testbed`, spawn N of one creature blueprint, Tracy capture; repeat at
  10×N. Assert the render-side CPU zones (collection, flush, record) are flat
  within noise while N-proportional zones are only the ones declared
  N-proportional (animation, for now). This is the `admissionShadow` idea
  applied to performance: creep is caught the frame it lands. Runs on demand,
  not in CI — but every S-step's acceptance includes one run.
- **Update the diagnostics skill's calibration line** to the current numbers,
  citing this file.

*Proves itself:* a zone table where ≥90% of `update`+`graphics` CPU is in
named leaf zones; the fixture produces its two numbers unattended; the S3
ordering question (is leaf-prep or animation the bigger half of the 1.25 ms?)
has an answer.

## S1 — Slang in the engine, one schema — landed, with one open defect

Two halves, one step, because the second is what makes the first pay.

**Outcome.** Slang links into `graphicsvulkan` and compiles `slang/` in
process; `src/apps/shaderpack` and the `compile_spirv` build step are deleted.
Modules cache to `%TEMP%/reone/slang-cache` under a hash of every `.slang`
source, so a shader edit needs no rebuild — restart or run the new
`recompileshaders` console command, which waits for device idle, recompiles,
and rebuilds the pipeline cache. A source error logs and retains the last good
module. **Measured: cold 3.8-4.2 s, warm 144 ms** (`tests.exe
--gtest_filter=SlangShaderCompiler.*`) — the warm figure is the new
per-launch cost that used to be build time, and is the number to watch if
module count grows. It started at 282 ms; `module()` was re-reading all 41
sources on every call to answer "is this current", which also ran on every
lazily created pipeline mid-game, and now only the explicit reload paths hash.

The schema half: `slang/lib/scene_schema.slang` is the single Slang
declaration of all seven scene tables, imported by `skin`, `megadraw_geometry`
and `tracing/resources`, which each dropped their copies; `lib/scene_material.slang`
is deleted. `VulkanRenderer::init` reflects the schema through
`scene_schema_reflect.slang` and aborts startup naming the offending field if a
C++ mirror disagrees — every Slang field is offset-checked, not a sample, so
swapping two adjacent `float4`s is caught where a stride comparison alone would
pass. The rule the mirrors follow: **a schema mirror carries the same name as
its Slang struct**, and only CPU-only types keep a `GpuScene` prefix. The dead
AABB debug shaders went with it (backlog 5.3).

**Open defect, and it gates calling S1 finished: both `tests.exe` and the
engine die with `STATUS_HEAP_CORRUPTION` (0xC0000374).** The engine crashes on
**every** headless capture run — `3/3` at `a6dc6ed1` and `3/3` after the fixes
below — and `tests.exe` on roughly half of full-suite runs (4/6 at
`a6dc6ed1`). It arrived with this step rather than with the review of it. The
engine crash is **teardown only**: the module loads, the frame renders, the
screenshot is written and the frame-slot line is logged, and the process then
faults on the way out — the signature the diagnostics skill names for an
object owning a resource that outlives the device. The suspects are the
`SlangShaderCompiler` member's lifetime against `VulkanRenderer::deinit`
ordering, and Slang's own DLLs. Run the Debug build's checked VMA first, as
that skill prescribes. The test crash never reproduces with
`--gtest_filter=SlangShaderCompiler.*` alone (0/5) or with those tests excluded
(0/3); it needs both, and the process always dies entering the *first* Slang
test after other suites have run. Adding only the neighbouring image-decoder
suites reproduces it 2/2, which points at heap damage done earlier and merely
*detected* by Slang's first large allocation, rather than at the compiler
itself. Linking `graphicsvulkan` into `tests` is what newly put the two in one
process. A separate real fault was found and fixed while chasing it - `mad.lib`
and `slang.lib` both resolved to vcpkg's *debug* import library in Release
builds (`LNK4098 MSVCRTD conflicts`), now bound per configuration - but that was
not the cause: the crash rate is unchanged after it, in both binaries.

What *is* established about the fixes: six headless raster captures of
`danm14ab` at frame 310, three either side of them, hash identical — so the
renames, the stricter guard and the build changes are output-neutral, which is
the bar a pure refactor has to clear.

**Deliberately still open**, so it is not mistaken for finished: the uniform
blocks are *not* on this path — `uniformlayout.generated.h` is still produced
offline by `uniformgen`/`slangc` and committed, so there are two schema
mechanisms until that is folded in. and the naming rule is only half-applied
in the C++ mirrors.

**Runtime compilation** (backlog 4.9's wants, unchanged): link Slang, compile
at startup and on demand, cache compiled SPIR-V keyed on source hash so warm
startup pays nothing, errors to the console, keep the last good module per
pipeline so a typo does not take the frame down, a force-recompile command.
The build-time shaderpack step and its stale-module trap both cease to exist.

**One schema.** `MergedVertex` is declared four times today (1 C++, 3 Slang);
`InstanceMaterial` three times; the sync mechanism is a comment. The scene
tables move into one Slang module (`slang/lib/scene_schema.slang`) that every
shader imports, and the C++ mirrors are *verified against Slang reflection* —
extending exactly what `uniformgen` already does for the uniform blocks, now
available at runtime because the compiler is in the process. A wrong mirror
fails at startup naming the field, not at 2 a.m. naming nothing. Backlog 0.3's
storage-buffer-stride hazard gets its tripwire the same way.

**Dead shader inventory rides along:** enumerate shaderpack entries against
live pipeline creation, delete the orphans (5.3 names two; the census will
find more).

*Proves itself:* raster captures byte-identical across the change (it is a
toolchain move, not a shader change); a deliberately mis-sized C++ mirror
aborts startup with the field named; cold and warm startup cost measured and
stated; `rg -c 'struct MergedVertex|struct SceneObject|InstanceMaterial'`
finds each once in Slang and once in C++.

## S2 — finish the inversion: nodes own GPU slots (this is R2)

R2's end-state, quoted because it is already right: *"nodes own slots in the
persistent GPU-shaped tables… A frame is flush dirty ranges, append dynamic
streams."* What this step adds is the concrete machinery:

- **Persistent device tables** — objects, interned materials, bone arena,
  dangly arena, procedural sources — with slot allocation at registration
  (stable `SceneNodeId` index is the natural key; R1's substrate carries over
  whole). Backlog 6.2 (`_nodes` never releases) becomes load-bearing here and
  is fixed as part of this step, not deferred around.
- **Sync model, decided:** per-frame-in-flight table copies plus a
  **change-log**: mutations append `(slot, range, payload)` to a CPU log;
  each frame replays the log segments the in-flight copy has not yet seen,
  then truncates behind the oldest frame. Chosen over single-copy versioned
  slots because it keeps writes sequential, makes the frame's CPU cost
  literally proportional to the log length, and gives the shadow oracle a
  natural observation point (hash the log, or rebuild-and-compare a full
  frame behind the flag — R1's tripwire, retained verbatim).
- **Classification and the material record are set-time** (G6/R2's constraint,
  already partially landed): setting a material computes record + kind once
  through the shared classifier and interns it. `dirtyAdmission` generations
  survive for dial changes — a re-bake on bump, set-time not frame-time.
- **The borrowed pointers die.** `RegisteredSkin/Dangly/Saber`'s raw pointers
  into node-owned vectors — the boundary's standing lifetime hazard — are
  replaced by owners writing into their arena slots through the log. Same for
  the grass face-table pointer.
- **What gets deleted:** the `Registered*`/`ObjectRecord` intermediates, the
  per-frame `prepare`/upload-vector assembly, `GpuSceneAdmission` as a frame
  phase (its policy lives on in the set-time classifier; a thin flush
  remains), and the second scene-side read path (`RayQueryPipeline` reading
  `objects()` for the sky bake) — V5 deletes the bake, S2 must not recreate
  the pattern.

*Proves itself:* R2's own gates, unchanged — zero shadow-oracle mismatches
across the acceptance set including a module transition; G-buffer dumps
byte-identical between incremental and forced-full paths in one binary;
upload-hash equality across all three modes holds. Plus: the scaling fixture
shows collection/flush flat in N, and the former collect+prepare zones reduce
to the dynamic streams alone.

## S3 — the remaining per-frame CPU work becomes events or GPU

Ordered by the measured table, revisable by S0's attribution. Each item
deletes a per-frame-per-thing CPU loop — the creep checklist, retired:

- **Skin palettes become event-driven.** 61 meshes × 128 bones rebuild
  unconditionally today (0.098 ms). Palettes are computed only when a mesh's
  animation actually advanced, written through the change-log. Animation
  evaluation itself **stays on the CPU** — game logic reads bone transforms
  (attachments, hardpoints), so moving it would duplicate state; this
  half-answers backlog 7.4's compute-skinning question: skinning is already
  GPU (the merge), palette *derivation* stays CPU, palette *scheduling* stops
  being per-frame.
- **Dangly moves into the merge kernel** (backlog 3.5, absorbed): the spring
  is solved where its output is consumed, deleting 789 calls/frame and the
  double-buffered upload. Determinism note: fixed 1/60 step and per-vertex
  state in the arena keep capture runs reproducible.
- **Particles simulate on the GPU.** Emitters upload spawn-parameter records
  on change; a compute pass integrates particle state in a persistent arena
  and emits quads into the procedural range. Determinism by the R3 precedent —
  integer hash of `(emitter, particle, frame)` for stochastic decisions, no
  shared-generator ordering dependence (the bug class 7.x already caught
  once). This answers 7.4's granularity question: the emitter is the record,
  particles are GPU state. The per-tick `addParticles` re-registration dies.
- **`settleMeshTransform` dies.** The prev-transform latch becomes part of
  the transform write itself (the slot holds current and previous; the flush
  rotates them), deleting a per-mesh-per-frame walk.
- **Flare LOS becomes a GPU visibility query** — one ray per flare light
  against the TLAS/merged scene, feeding the flare's alpha. Coordinate with
  G9, which owns un-filtering `LensFlare` at admission; land whichever comes
  first, but the CPU LOS walk does not survive both.

*Proves itself:* each named zone at ~zero in Tracy with the GPU-side cost
measured by S0's timestamps; raster captures stay byte-identical for the
skin/settle/flare items (pure scheduling moves); dangly and particles are
judged on the isolation fixtures (`warp testbed grass|smoke`) plus
distribution comparison in traced mode; the scaling fixture stays flat.

## S4 — residency: stop rewriting the static world

The merge currently rewrites every vertex of the scene every frame — static
geometry included — because `prevPosition` lives per-vertex and everything is
published `Dynamic`. The scaffolding for better (`GpuSceneResidencyClass`,
`Region`) already exists, unused.

- **Static/dynamic partition** of the merged buffer and the material table
  (backlog 9.2's remainder, 8.9's static half). The static region is written
  at module load and never touched; `prevPosition = position` there by
  construction. The merge dispatch covers dynamic ranges only.
- **Backlog 9.1 first:** decide what "static" is provable from — the admission
  proof, not the authored `staticObject` hint the code already distrusts.
- **The per-element binary search goes** (backlog 8.12): with regions, ranges
  dispatch per-object-run, or a precomputed per-vertex object id replaces the
  search. Either kills the O(log N) per thread.
- **BLAS strategy is explicitly unchanged** until backlog 8.15 measures the
  AMD build path: the single full rebuild stands (it is GPU time, and small);
  compaction (8.14) lands here; the static/dynamic BLAS split stays the
  documented escape hatch if AMD's 7.4× Vulkan build gap is real on current
  drivers.

*Proves itself:* per-frame upload traffic ≈ dynamic set only (measured, not
asserted); merge GPU cost drops in proportion to the static fraction
(danm14ab: 45k of 86k triangles static); upload-hash equality and the shadow
oracle hold; a module transition rebuilds the static region exactly once.

## S5 — the RHI: logic files read as logic

The audit's numbers: 11,715 Vulkan-touching lines, of which ~4,300 are
wrapper tier and ~1,930 are ceremony inlined into the seven high-level files —
five hand-written image-transition helpers, ~368 descriptor-boilerplate
lines, ~182 dynamic-rendering preambles, and passes that spend 41 lines
preparing one `vkCmdDraw(cmd, 3, 1, 0, 0)`. The RHI's job is that the seven
files stop containing any of it. Its *internal* complexity is unconstrained
by policy — it is the one part of the tree licensed to be ugly.

**Stage 1 — four builders, Vulkan-only, start whenever.** No design risk,
independent of every other track:

1. a layout-tracking image type — generalize what `VulkanGBuffer` already
   does; the five transition helpers and ~65 inline barrier sites collapse
   into it;
2. a `RenderPassScope` RAII type over `VkRenderingInfo` + viewport + scissor;
3. a pipeline builder covering compute and RT — the gap `VulkanPipelineCache`
   leaves, which is why `rayquery.cpp`, `gpuscene.cpp` and `nrddenoiser.cpp`
   each hand-roll layouts, pools and pipelines;
4. a descriptor-write builder.

Measured expectation: ~1,100 lines out for ~350 in.

**Stage 2 — the seam, after V1c.** Once raster owns primary visibility in
every mode the frame shape stops moving, and the wrapper tier plus the
builders formalize into an interface layer: device, swapchain, queues,
buffers, images, pipelines, submission. The gate is mechanical:

    rg 'vk[A-Z]|Vk[A-Z]|vma[A-Z]' src/libs/graphics --glob '!vulkan/**'
    rg 'vk[A-Z]|Vk[A-Z]|vma[A-Z]' src/libs/graphics/vulkan/*.cpp   # only RHI files may match

`IRenderer` sheds its Vulkan leak (`begin2DRendering` exists only to scope
dynamic rendering — the RHI owns that scope). Slang runtime (S1) supplies the
shader half of the seam, so pipeline creation takes source + schema, not
SPIR-V blobs + hand-declared layouts.

*Proves itself:* per stage, captures byte-identical (these are pure
refactors); the grep gate empty; the seven core files' line counts recorded
before/after against the audit's ~5,000 → ~3,300 estimate — a number to
report, not a bar to force.

## S6 — deletions the track leaves behind

Trailing cleanup, each unblocked by an earlier step; none worth its own
session until its blocker lands:

- the legacy per-mesh path: `VulkanMesh`, per-object `LocalUniforms`,
  `BoneUniforms`/`DanglyUniforms` blocks and their uniform-ring wiring —
  last consumer is the runtime sky bake, so this trails **V5**;
- the per-draw texture-set path (`acquireTextureSet`) once 2D rides the RHI;
- the `Registered*` types and `scene::GpuScene`'s frame-phase surface (S2's
  mechanical remainder — R2 already names it);
- `resetFrame()` and other confirmed no-ops.

*Proves itself:* the greps return nothing, `--target tests` passes, and the
acceptance capture set is unchanged.

## Decisions taken in this plan — review these

1. **Slang runtime is promoted P3 → track-blocking** (S1 before S2+). The
   schema single-source is the reason; the iteration loop is the bonus.
2. **Sync model: per-FIF table copies + change-log replay**, not versioned
   slots in one copy.
3. **Animation evaluation stays CPU; palettes become event-driven outputs**
   (half of backlog 7.4, decided).
4. **Particle simulation moves to GPU with integer-hash determinism**; the
   emitter is the registered record (the other half of 7.4, decided).
5. **BLAS single-full-rebuild stands** until 8.15's AMD measurement; the
   split is the documented escape, not the default.
6. **RHI in two stages**, seam formalized only after V1c; stage 1 may start
   immediately.
7. **S2 waits for G8** — the last vocabulary-growing step — rather than
   racing it.

## Open questions, owned by a step

- What the 1.25 ms inside `SceneGraph::update` actually is → **S0**; may
  reorder S3, and decides whether leaf-prep survives the inversion or becomes
  GPU-driven draw generation (a possible S4 extension, not assumed).
- Whether G8's CPU transparency sort ever outgrows its budget → the scaling
  fixture watches it; G8 already names the Enderton fallback.
- Whether the RHI seam should also carry the 2D renderer or leave it as a
  direct client → decide in S5 stage 2 from what `renderer2d.cpp` looks like
  after stage 1 (it is only 7% Vulkan tokens today).
