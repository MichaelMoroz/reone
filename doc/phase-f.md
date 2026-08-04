# Phase F — what to do next

`cleanup-plan.md` is the record of how the engine got here. This file is the
work that remains, and nothing else.

## The goal

**Make the engine better while preserving the retro look.** Those pull in
opposite directions often enough that the rule needs stating: the two aims are
separated by *mode*, not traded off inside one.

- **Retro is preservation.** It reproduces what the original did. A deviation
  is a bug, not a taste question, and the reference engines settle it — see
  the reference section at the end of this file. Retro is also the fidelity
  anchor every other mode is judged against, which is why it goes first in
  each step and why it must stay honest.
- **PBR and path tracing are the improvement.** They are held to looking
  right, not to matching the original, and 2003's constraints are not
  binding on them.

The useful consequence when reading a reference finding: ask whether it
records **artistic intent** or **a technical limitation**. The env-map
formula, the sphere-map projection, the alpha blend modes and submission
order are intent — they *are* the look, and they bind retro absolutely.
The eight-light budget, skeleton-derived shadows, absent anti-aliasing and
unfiltered textures are limitations — retro may keep them, the other modes
should exceed them.

One case shows the distinction is not always obvious: KOTOR's 2D env maps are
GL **sphere maps**, so the eye-space projection is not a stylistic choice at
all — it is how the texels were authored, and sampling them any other way
reads the wrong pixels *in every mode*. A cube map has no such constraint,
so there world-space reflection is available to the improving modes.

Steps are named in three groups: **`G…`** the raster track, **`R…`** runtime
cost work that cut across it, and the **path-tracing substage**, which contains
the old `V…` visibility steps. The earlier `F0…F10` numbering outlived three
restructures and stopped meaning anything; commits before 2026-08-02 refer to
F0, F1 and F2, which are done and listed under **State** below.

## How steps are written here

A step that does not finish before an agent compacts gets abandoned — that
happened three times on 2026-08-02, at ninety minutes each. So steps are sized
to a context, not to a coherent-looking change, and each one:

- **is briefable in a page, with no cross-references.** The step text is the
  brief. A brief that says "read the plan" spends the agent's context
  re-deriving what is already written, and derives it wrongly after compacting.
- **proves its own work, positively.** "The hash did not move" shows nothing
  broke; it shows nothing about whether the new thing works.
- **turns on for one object before all of them** where that is possible. Same
  plumbing either way, and a wrong result is one object-shaped difference
  instead of a whole scene.
- **is committable alone**, so a failure is discarded rather than left in a
  twenty-two-file stash nobody reopens.

Before building a metric: **render the thing and look at it.** Every metric
built on 2026-08-02 either missed the real defect or misled — the sky continuity
check said nothing about buildings baked into the horizon, and the "holes"
triage ranked a correct starfield worst. Metrics check the rest of the set once
you know what you are looking at.

The instrument for looking is the free camera, and it no longer costs a trip
to the console: `5333ae11` put it on the editor menu, restoring the previous
camera on the way out, and `620eea53` moved looking onto a held right button
so the menu that turns the camera on stays clickable. That second commit also
retracted a measurement from the same session which appeared to show the free
camera never reaching the renderer at all. The captures had frozen the frame
before the module finished loading, so they compared two loading screens. A
capture taken too early proves nothing about the thing it names.

**Capture rules, everywhere:** `--dev 0`, or the frame-time readout forges a
difference; `--grassdensity 1`, because `reone.cfg` is graded away from
defaults and wins any flag not passed; and `--taajitter 0` on **both sides** of
any cross-mode comparison, or the comparison measures sampling noise (see G2).
Since `9ba51344` the raster modes are unjittered whatever that flag says, so
in practice it now only silences the traced side; it stays a rule because G9
opens the gate again. Traced output is nondeterministic —
compare distributions, never a stored number.

**Captures are deterministic per binary, not across binaries.** The capture
frame counts from process start, so anything that shifts load timing — G5
added one shader module — shifts animation phase at a fixed frame: 92k pixels
moved on `danm14ab` because the plaza droid stood differently, and one
material dedup split on `bumpMapFrame`, with admission untouched. Cross-commit
dump comparisons must expect animation-phase drift or mask animated actors.
The upload hash is stable within a binary and across modes — that equality is
the invariant; its absolute value across commits is not.

## State

| | |
|---|---|
| **F0, F1** | done `0cc67e42` — the merge adopted raster's transform maths, and `MergedVertex` carries object-space position for the hashed alpha test |
| **F2** | done `394bf675` — the merged buffer gained `INDEX_BUFFER` usage, and the post-merge barrier names the vertex shader and index input so a raster draw cannot race the merge compute |
| **G1** | done `db668c2f` — the per-mesh path is gone; raster modes run an empty plan and present a cleared scene |
| **G2** | done `ef6c5850`, coverage corrected in `298d0542` — one draw over merged geometry writes a G-buffer that agrees with the traced one |
| **G3** | done `cc9a36ac` — admission extracted and shared, sky suppressed identically, all three modes hash the upload to the same value |
| **G4** | done `d6148ee6` — matrices born in Vulkan clip, `glToVulkanClip` and the inert `IUniforms` deleted; dumps bit-identical across the change |
| **G5** | done `802ec6c8` — retro shades the G-buffer; by eye, three of four modules read as the same game. `920c1259` then took blended surfaces out of the G-buffer and the per-frame hash out of the frame; `1c703dde` added Tracy, capturable headless |
| **R1** | done `cfbb2989` — persistent registration; 1.33 → 0.16 ms measured, zero shadow mismatches including a module transition |
| **R2** | its constraint landed inside G6 `fe22cb22` — the grown material record is computed at material-set time, not per frame — leaving R2 itself as the `Registered*` deletion, mechanical cleanup once nothing reads them |
| **R3** | done `1b6559aa` — grass placement in the merge compute; grass CPU 0.007 ms and flat through a teleport, graphics slot 4.1 → 1.3 ms at density 3.57 |
| **G6** | done `fe22cb22`, material id in `be0e8dce` — PBR shades the G-buffer, the record grew 256 → 288 bytes to carry envmap, water and per-object ambient, and G5's four approximations became real data. `cd3fbec2` then made metal reflect the authored source, sharp and in eye space. Bump rides in the record and is sampled, but has never been held to a fixture |
| **G7** | done `e7a4f5b6` — shadows are a mega-draw over merged geometry and the proxies are deleted. Five corrections followed, all found in play: `67cbc312`, `0fb05120`, `05e6a8f8`, `2f00b5f5`, `306cfbc6` |
| **G8** | transparency in retro and PBR — sorted quads, premultiplied, the three alpha kinds |
| **G9** | anti-aliasing as one output stage for all three modes: FSR and FXAA |
| **sky** | **after G9.** The offline `skybake` asset becomes the single source: V0 fixes the baker, V4 suppresses from its manifest, V2 composites, V5 deletes the runtime bake. Raster shows a black sky until then, by decision |
| **PT substage** | everything traced, after the raster track finishes — see the substage section for its ordered list |

**The order of work, set 2026-08-03: finish raster first.** Done since the last
reorder: the **tracer guide fix** (`98ad7e4f` — guide surface = first
opaque-or-cutout hit, guide-miss falls back to raw at the composite; at the
vent camera, traced-only coverage 831,500 → 0 and depth MAE 2.995 → 0.0012),
**grass density as a live GPU gate** (`c1747799`), the **emitter census**
(`714bd700`), **G6** with its material-id follow-up and the metal fix
(`fe22cb22`, `be0e8dce`, `cd3fbec2`), and **G7** with the five corrections that
followed it (`e7a4f5b6`, then `67cbc312`, `0fb05120`, `05e6a8f8`, `2f00b5f5`,
`306cfbc6`).

What is left of the raster track is **G8** transparency in retro and PBR and
**G9** anti-aliasing for all three modes. Then everything traced becomes a
**path-tracing substage** — backlog 3.7's transparency restructure, the
sphere/capsule march with its density and id grids, V1 unification, ReSTIR,
SHARC, volumetrics, and the sky track.

Two consequences of that split worth stating, because they move things that
were previously on the critical path:

- **The fog grid leaves the raster track entirely.** It was specced beside
  G8; the march is path-tracing only (retro keeps the original's analytic
  fog, PBR keeps retro's treatment for now), so the grid, the id grid and
  every marcher decision below now sit in the substage.
- **G8 narrows to what retro and PBR need**: sorted premultiplied quads for
  all three alpha kinds, textured, exactly as the original drew them. The
  question of additive leaving geometry belongs to the substage, not here.

One ordering constraint survives from the older two-track framing and still
binds: **the geometry track measures against the traced G-buffer, and
unification deletes the traced primary visibility that produces it** — finish
with the instrument before removing it.

**Decided, and stated so it is not mistaken for an oversight:** the raster
track leaves **the sky black in retro and PBR**, and that is intentional
through G9. G3 suppressed the shell unconditionally in every mode, and the
sky chain — V0 fix the baker, V4 suppress from its manifest, V2 composite,
V5 delete the runtime bake — **runs after G9**. So "the raster track is
finished" means finished apart from the sky, and every by-eye acceptance
from G6 to G9 is judged against a black-sky frame.

---

# G — rebuild raster on `GpuScene`

The old raster path differs from `GpuScene` too much for incremental agreement
to be a useful target; three drafts tried and each was harder than the thing it
protected. So it goes, and what replaces it is written the way it should have
been written: one draw over merged geometry, then shading on top.

## G1 — delete the raster path, keep the shaders — done `db668c2f`

Removed the per-mesh draw walk, the pipeline plumbing behind it, and the passes
that only existed to feed it. **Every shader kept.** They encode a decade of
material behaviour that nothing else records, and G5 and G6 put them back to
work against a different input.

Also boxed for later, deliberately: **OIT, SSAO, SSR**, and the old post
chain including FXAA and sharpen. Not wrong — just not
worth carrying across a rewrite of the thing they sit on.

The "before" captures were taken first: `danm14ab`, `danm13`, `ebo_m12aa` and
`202tel`, retro and PBR, with `--dumptargets` alongside the PBR ones. **The
target dumps turned out to matter more than the screenshots** — they are what
made G2 measurable against the path it replaced, rather than against the tracer
alone. Anything that deletes a producer should dump its output first.

Two corrections the work produced:

- **Vulkan ran four walks per frame, not six.** Shadow is one walk whose
  cascades and cube faces are multiview *inside* it. The 1.873 ms and the "six
  `drawScene` walks" were an OpenGL measurement from 2026-07-28, and OpenGL is
  gone. The shape of G2 is still right; the number was never about this backend.
- **The sky bake still rides raster's per-mesh infrastructure** — `VulkanMesh`
  and its vertex input descriptions, the pipeline key's vertex input fields, the
  per-mesh resources and zero buffer, `LocalUniforms` and the uniform ring.
  Commit `00c542e2` gave it its own *shader*, which is not the same as
  independence. All of that had to survive the deletion, and V5 inherits the
  job of untangling it.

## G2 — one draw, one basic G-buffer — done `ef6c5850`

A single indexed draw over the merged buffer, pulling vertices by `SV_VertexID`,
writing position, normal, albedo and the rest of the G-buffer. No shading. No
per-mesh anything. `slang/megadraw.slang`, and a third descriptor set carrying
the merged buffers and the bindless texture tables.

**The merge had no owner outside the tracer.** `VulkanGpuScene` lived inside
`VulkanRayQuery`, which is only constructed in traced mode, so raster had never
built merged geometry at all — the buffer F2 made readable had no producer on
that path. `VulkanRenderPipeline` owns it now. Classification, texture
registration and prepare became **one shared implementation** feeding either
consumer; a second classifier would have drifted from the first, which is the
bug class this rewrite exists to remove.

**Two draws, not one.** The opaque range, then the non-opaque range gated to
punch-through materials with the hashed alpha test. Procedural quads carry a
consumer-local tag, because grass and particles are merged with
`objectPosition` zero and would hash against a constant.

*Proves itself* against the traced G-buffer, `danm14ab` frame 310, **both sides
with `--taajitter 0`**:

| | |
|---|---:|
| traced-only | **0** |
| raster-only | 464,982 — the sky shell, which G3 then removed |
| both covered | 1,608,598 |
| depth error > 1 unit | **206 px, 0.0128%** |
| depth median / meanabs | 0.000106 / **0.0038** world units |
| eye normal meanabs | **0.00237** |

Retro's G-buffer is **byte-identical to PBR's** across all six targets, which is
the point: the draw is mode-independent, and only shading differs. Worth
repeating on a creature-heavy and a particle-heavy module before leaning on the
agreement everywhere.

**Measure this with jitter off, on both sides.** Raster jitters through the
projection matrix and the tracer jitters the ray, so with jitter on every
alpha-cutout edge decides independently and the comparison is dominated by
sampling noise — it read 2.94% where the truth was 0.67%, and the mistake
survived long enough to send a step chasing a residual that was not there. The
diagnostics skill says this already; it is repeated here because it was read and
then not applied.

### The coverage rule, and why it is the tracer's

Three faults were found by holding the G-buffer to the traced one, and each was
raster applying a rule the tracer does not:

- **the opaque draw alpha-tested.** Alpha-blended leaf meshes classify as
  Opaque, so the tracer commits every texel of a leaf card in hardware. That one
  `discard` was the missing canopy.
- **the non-opaque draw demanded punch-through and rejected procedural quads**,
  which removed grass. Coverage now mirrors `commitsCoverage` and the range
  itself is the filter, exactly as the BLAS geometry range is for the tracer.
  Hashed alpha went with it — a stochastic test cannot match a deterministic one.
- **additive surfaces are not surfaces.** Traversal commits them, but the
  primary layer loop pays their emission and passes through, so the traced
  G-buffer holds what is behind. Raster was writing a solid saber blade where
  the tracer records the hand and the ground.

The mistake class in the third one is worth naming: the rule was taken from
what *traversal commits* rather than from what *fills the traced G-buffer*.
Those are different questions and only the second one matters here.

## G3 — one scene description — done `cc9a36ac`

Landed as specified: admission in `scene/render/admission`, owned by
`VulkanRenderPipeline`; `RayQueryPipeline` traced-only; all four
simplifications. The measured outcome: all three modes log upload hash
`5b6089c88c790882` on `danm14ab`; raster-only coverage fell 464,982 → 5 with
traced-only at 9; materials dedup 1,085 → 122; `MergedVertex` 160 → 144 bytes.
One decided behaviour change beyond the sky suppression itself: a failed sky
bake now falls back to the fallback cube with the shell still suppressed,
where it used to restore the shell as geometry.

`88d3efae` later removed the last input to what gets drawn that was not scene
policy at all. `updateRoomVisibility` applied the VIS graph — the leader's
room plus its adjacents — only when the camera happened to be third person;
every other camera took the all-visible branch, so first person and the free
camera already drew every room, including the walkmesh-less ones that are K1's
skybox convention. That is why switching to the free camera on Korriban made
sky appear: not a filter failing, a filter switched off by an input mode. The
branch is deleted outright rather than given a camera-appropriate key,
justified by this project's own measurement that culling buys nothing here —
removing about 9,000 frustum tests per frame moved frame time by nothing,
because the tests cost tens of nanoseconds and raster's real cost is CPU work
per draw. Four modules are byte-identical in third person across the change,
with the caveat the commit states: it was not confirmed that the culled branch
was taken in those captures, so that reads as no regression observed rather
than as proof of equivalence.

**The refactors come before the shading, and the order is forced twice over.**
First: the admission rewrite changes the classification vocabulary, the
material table and the `MergedVertex` layout, and G5/G6 shading consumes all
three — shading first means re-verifying it after the ground moves. Second:
right now is the cheapest a refactor will ever be, because the
traced-agreement instrument is armed and the G-buffer is raster's *only*
output. "Nothing moved" is checkable to a couple hundred pixels, with no
shaded image to re-judge by eye.

Raster and path tracing must consume **the same scene description**. The only
difference between the two is how it is rendered. Everything upstream of that —
which objects are admitted, how they are classified, what the material records
say, whether the sky shell is suppressed — is scene policy, and there is one
answer to it.

That is not the case today, and the failure has a name in the source:
`RayQueryPipeline::prepareRaster`, which calls the shared `prepare` with
`skyRoom = nullptr, skyBaked = false` and a comment presenting the divergence as
intent. So the sky shell is admitted for one consumer and rejected for the
other. The classifier is shared in the letter and forked in the substance, and
`RayQueryPipeline` is constructed in Retro and PBR purely to host it.

Admission belongs in its own unit that neither renderer owns. `RayQueryPipeline`
then shrinks to what its name claims — bake, TLAS, dispatch, denoise.

**Four simplifications ride along in the same step**, because they all live in
the admission code being extracted and each makes the invariant cheaper to hold:

1. **One procedural record kind.** The device side already lowered particles,
   grass and billboards into a single `GpuSceneProceduralQuad`; the scene side
   still carries three record types, three classifiers and three lowering
   paths. One `RegisteredQuads` record collapses them, and a billboard is a
   one-instance system.
2. **Deduplicate materials.** Today `materials` holds one 256-byte record per
   admitted *object* — two hundred meshes sharing ten textures upload two
   hundred materials. Hash-cons by content; `materialIds` already provides the
   per-triangle indirection, so nothing downstream changes.
3. **Drop `objectPosition` from `MergedVertex`.** Hashed alpha was its only
   consumer and the coverage correction deleted it. 16 of 160 bytes per merged
   vertex, in the C++ struct, both slang declarations and the asserts, moved
   together.
4. **One classification vocabulary.** An object's nature is currently spread
   over `RenderCategory`, `PrimitiveClass`, `surfaceType` and feature bits
   24/25/26, each consumer re-inferring from texture blending flags. Admission
   assigns one authoritative kind — opaque, cutout, lit-blended,
   additive-emissive — and everything else derives from it mechanically.

*Proves itself,* and this is the check worth having: **hash the
`GpuSceneUpload` in both modes at the same camera and frame and require
equality.** Object records, material table, bone and dangly pools, ordering.
Pixel counts are downstream evidence; this tests the invariant directly, and it
makes the whole class of divergence impossible to reintroduce quietly. The
sky's 464,982 raster-only pixels fall out of it as a consequence rather than
needing a fix of their own.

## G4 — the OpenGL legacy goes — done `d6148ee6`

Landed better than the bar asked: dumps, captures and screenshots came out
**bit-identical**, not ulp-close — the native ZO construction reproduces the
old correction exactly. Two consumers were quietly re-deriving the GL
convention and moved with it: frustum near-plane extraction and shadow-corner
unprojection. The sky-cube bake keeps its private `glm::perspective`
deliberately — it was never routed through the correction, so converting it
would have been a behaviour change smuggled into a refactor.

The backend is Vulkan-only since `df1aa375`, but the frame still speaks GL in
two places, live code both:

- **Matrices are built in GL convention and rewritten every frame.**
  `glToVulkanClip` converts projection, viewProjection, prevViewProjection and
  four shadow matrices per frame, with the flipped viewport compensating y.
  The camera should produce Vulkan clip space directly and the function should
  not exist.
- **`IUniforms` is an inert shell.** `Uniforms::init()` is empty and the
  setters write a CPU mirror that the Vulkan pipeline reads back a moment
  later — the header says so itself. After G1, `setBones`, `setDangly`,
  `setParticles` and `setWalkmesh` have zero callers, and the only callers of
  `setLocals`/`setAABB` sit in `drawdebug.cpp`, which is not in the build and
  still references GL types deleted with the backend.

*Proves itself:* it is a pure refactor, and the instrument is already armed.
Raster G-buffer dumps before and after may differ only at the ulp level —
constructing the projection natively instead of correcting it can move the
last bit of depth — so the bar is the 206-pixel figure: the traced-agreement
numbers must not move, and retro/PBR must stay byte-identical to each other.
Traced output judged by distribution, as always.

## G5 — retro shading on the G-buffer — done `802ec6c8`

Landed; three modules read as the same game by eye, and the gaps are named in
the commit: lightmap presence inferred (no bit exists), lightmapped-as-static
gate, per-object ambient/diffuse collapsed to neutral, environment reflection
omitted — all candidates for the G6 material-record growth.

**The 202tel finding, and what it turned out to be:** a lit-blended panel sat
in the G-buffer as the primary surface — exactly as the tracer records it —
and resolved opaque. This was first read as the shared coverage rule doing its
job, with raster's cutout-only correction (`920c1259`) framed as a temporary
divergence from ground truth. **The framing was backwards: the tracer's
behaviour is the bug.** Blended surfaces in the traced G-buffer feed NRD
guides whose depth, normal and motion describe the glass or the smoke while
the denoised signal is dominated by what lies behind — observed as
reprojection artifacts. The traced G-buffer was trusted as the instrument for
geometry and coverage, and for opaque and cutout it is; for blended coverage
it recorded a defect, and G2b faithfully copied it before G5b removed it
again.

That fix landed as `98ad7e4f`: the tracer's guide surface is the first
opaque-or-cutout hit — the rule raster already implements — while blended
surfaces keep contributing radiance in the layer loop. At the vent camera it
took traced-only coverage from 831,500 to 0 and depth MAE from 2.995 to
0.0012.

Retro's material, reading the G-buffer instead of shading forward. Retro stops
being a forward renderer.

That is worth doing before PBR for two reasons: **retro is the mode expected to
look more or less the same**, so it is the only honest visual check the rebuild
has; and it is what makes a single sky composite possible at all — V2 needed a
retro special case *only* because retro had no G-buffer.

*Proves itself:* by eye, against the G1 captures.

**Two things are legitimately missing from the frame, and neither is a G5
regression.** Foliage and grass are *not* on this list any more — they were,
until the coverage rule was corrected, and the correction is `298d0542`.

- **Additive emissive surfaces** — saber blades, glow decals. They contribute
  no G-buffer surface by design; see the blended pass below.
- **Shadows**, until G7 — restored by `e7a4f5b6` and the corrections after it.

The sky-shell divergence recorded here — 464,982 raster-only pixels — was
removed by G3; raster-only coverage is now 5 px.

## R1 — the scene stops rebuilding itself — done `cfbb2989`

Landed under the gate: 1.33 → **0.16 ms**. The shadow tripwire caught two real
bugs during the work — deformation refreshing before parent animation applied
(a walk-order dependence, now pinned at the render boundary for both paths)
and stale state after module transition. Acceptance ran shadow-armed with
zero mismatches, and `--commands-frame-scheduled` now exists for scripted
module transitions.

Tracy attributed the render thread's cost: `collectInto` 0.73 ms plus
admission 0.60 ms per frame, spent re-deriving a description that is ~95%
identical to the previous frame's. Both zones are one defect: registration
and admission are O(scene) when the scene barely changes.

The design: a **persistent object table** with dirty tracking. Nodes register
on creation, unregister on destroy; transform changes mark dirty; material
changes bump a per-object generation; dial changes bump a global admission
generation (the `grassGeneration` pattern, generalised). Classification and
the 256-byte material record cache per object — `bumpMapFrame` is the one
per-frame field and is patched, not rebuilt. The dedup table persists with
refcounts. Bone and dangly streams stay per-frame but collapse their
three-copy chain to one write into a persistent arena.

Two load-bearing decisions:

- **Canonical order.** Incremental add/remove cannot reproduce a per-frame
  walk order, so both paths order opaque-first-then-stable-id. One-time
  traced-distribution shift; byte-comparable forever after.
- **The full rebuild stays alive as a shadow path.** Behind a flag, every
  frame builds the upload both ways and compares hashes — the
  stale-invalidation tripwire, run through every acceptance capture,
  including a module transition so unregister/re-register is exercised.
  The G3 hash instrument is what makes this refactor checkable at all.

*Proves itself:* zero shadow mismatches across danm14ab, danm13, a module
transition and a traced run; G-buffer dumps byte-identical between the
incremental and forced-full paths *in the same binary* (no phase-drift
excuse); and the two zones fall from 1.33 ms to under 0.30, measured by the
instrument that found them.

## R2 — delete the translation layer

R1 makes the rebuild cheap. R2 asks the question R1 should have started from:
**why does admission exist at all if the scene can be in the right form in the
first place?** Classification is a pure function of the material — blending
mode, texture features, dials. A pure function of rarely-changing inputs is
not a frame phase; it runs when the input changes.

The layer is historical: it was the tracer's private policy grafted beside
raster's material system, and G3 unified the policy but kept the shape of a
per-frame pass. What remains genuinely per-frame is dynamic *data* — bone
palettes, dangly positions, particle instances, billboard basis — not
classification of anything.

End state: **nodes own slots in the persistent GPU-shaped tables.** Setting a
material computes its record and kind once, through the same shared function —
one policy, preserved — and interns it. Moving writes matrices into the slot.
Animation writes palettes into its own arena. A frame is *flush dirty ranges,
append dynamic streams*. The `Registered*` intermediates and the per-frame
assembly loop die; one schema instead of three.

The wrinkles that look like blockers and are not: calibration dials re-bake
affected records on the generation bump (set-time, not frame-time), and the
sky rule is a registration-time predicate once the room is known.

R1's substrate carries over whole — stable-id table, dirty hooks, refcounted
interner, node-owned streams, canonical order — and above all the **shadow
tripwire**, which is just as necessary when nodes write tables directly:
stale-slot bugs are the same failure class, and it caught a real
animation-timing bug within hours of existing.

*Proves itself* the same way R1 does: zero shadow mismatches across the
acceptance set including a module transition, byte-identical dumps between
paths in one binary, and the frame cost of the former collection+admission
zones reduced to the dynamic streams alone.

## R3 — grass generates on the GPU — done `1b6559aa`

Landed as specced. Placement parity to the pre-R3 baseline: 2,853 px of 2M,
all silhouettes and the now-continuous ramp. Grass CPU 0.007 ms/frame and
flat through a scheduled teleport; graphics slot 4.118 → 1.337 ms at density
3.57; the merge dispatch pays 9.9 µs. Field acceptance — the 350/250 moving
fps report — pending the reporter.

Pre-R3 baseline, measured in play: 350 fps standing, 250 fps moving. The
delta is the grass edge band — the ramp is quantised so a *still* camera rebuilds nothing, which
means a *moving* camera crosses a quantisation step every few centimetres and
keeps every edge-band patch's instance vector rebuilding. Mitigations
(coarser steps, hysteresis, per-cluster patching) shrink the class; moving
placement into the merge compute deletes it.

Grass is deterministic procedural data: an integer hash of (face, cluster,
stream) — deliberately made integer for bit-reproducibility, so it ports to
slang bit-exact. The design:

- **Per-face records upload once** (face triangle in world space, lightmap
  UVs, per-face cluster budget from area × density, material index),
  refreshed only on the density generation.
- **The merge compute expands clusters** from the ported hash for faces in
  the camera band, computing the edge ramp *continuously* — no quantisation,
  because there is no CPU rebuild to protect. Out-of-ring clusters emit
  degenerate quads.
- **Counts stay CPU-known and deterministic**: a cheap face-band scan sizes
  the ranges (faces are hundreds, not thousands); every in-band face emits
  its full budget, degenerate where scaled out. No indirect draws, no GPU
  readback, BLAS capacity unchanged in kind.
- **Both consumers inherit it for free** — same merged buffer, same BLAS
  path; traced grass gets the same continuous edge.
- **Deleted**: cluster materialisation, the cluster pool and scene-node
  children, grass instance vectors in registration, grass lowering in
  admission. Grass registers once per node with a face-table reference.

*Proves itself:* the grass CPU zones go to ~zero and stay there under camera
motion; standing fps unchanged, and moving fps recovers — the landed evidence
being the CPU-zone and graphics-slot figures, the in-play figure not yet
re-measured; the
field edge reads the same or better — continuous now — in both modes; the
density dial still works; merge GPU cost delta measured and reported.

## G6 — PBR shading on the G-buffer — done `fe22cb22`, material id in `be0e8dce`

Landed. PBR stops presenting black: the resolve G1 kept was rewired to the
current executor, and `GpuSceneMaterial` grew 256 → 288 bytes to carry
`envMap`, `envMapCube`, `waterAlpha`, `ambientColor` and `envMapDerivedLayer`,
moved across the C++ struct, its asserts and both slang declarations together.
Admission fills them through the shared classifier, with environment layers
reserved at classification and reset when module resources go, so nothing new
happens per frame. All four of G5's gaps closed: lightmap presence is a real
bit rather than a comparison against neutral white, the static light gate is a
real bit rather than has-a-lightmap, per-object ambient and diffuse are the
authored colours, and environment reflection exists in both resolves. Judged
against the pre-G1 captures as a material reference and not as a target,
danm14ab, danm13 and ebo_m12aa read coherently; 202tel was inconclusive
because near geometry occludes the bay. Eight G-buffer targets byte-identical
between PBR and retro, upload hash `a12866a1ebaac77d` in all three modes,
`SceneAdmission::prepare` unchanged at 0.132 ms.

**One part of this row's old scope was never discharged: bump.** It sits in the
material record and `megadraw.slang` samples it, but nothing has held it to
authored content. Envmap was checked, by the metal work below. Bump is still an
unchecked claim and should be given a fixture rather than assumed.

The PBR material, same input.

**PBR is not held to its old output.** It is visibly broken today, and matching
it would preserve the bug. It is held to looking right, and to agreeing with the
traced G-buffer on geometry.

**G6 owns three fields the merged material record does not have.**
`GpuSceneMaterial` carries no `envMapDerivedLayer`, no `waterAlpha` and no
environment cube ids, so G2 writes zero for each. The old resolve read
`envMapDerivedLayer` out of the self-illumination alpha; something has to put it
back before environment mapping and water can work at all. Growing the record
is G6's own work: the fields are added to `GpuSceneMaterial` and admission
(`scene/render/admission`) fills them.

**It also owns G5's four named gaps**, which all resolve here: no
lightmap-presence bit (inferred today from the neutral-white value), the
lightmapped-as-static light gate, per-object ambient and diffuse colours
collapsed to neutral, and environment reflection omitted with only the bit
kept.

**R2's constraint rides in this step** (see the R2 row in State): the grown
record is computed **at material-set time**, through the shared classifier,
not rebuilt per frame — G6 is reshaping that record anyway, so the schema and
its timing change together rather than twice. The `Registered*` deletion stays
R2's, as mechanical cleanup afterwards.

*Proves itself:* the PBR G-buffer stays byte-identical to retro's (the draw is
mode-independent; only shading may differ); an environment-mapped fixture and
a water fixture render with those materials visibly working, judged against
the pre-G1 PBR captures for material behaviour rather than for the old
output's bugs; the upload hash stays equal across all three modes, which is
what proves the record change did not fork admission.

### Metal reads duller than the original — resolved

Reported from play — metal is less reflective in **both** retro and PBR than
in the original game. The cause is visible in the code rather than a matter of
taste: the original forward shader sampled the **authored environment cube
directly** (`sampleEnvMap`, `pbr_model.slang:368`), while both current
resolves sample the **prefiltered IBL array at mip 0** — a roughness-convolved
cube at 128² (`kPrefilteredSize`). The strength term is unchanged in both,
`* (1 - diffuse.a)`, so this is not a scaling error: the reflection is taken
from a blurred, downsampled source where the original took a sharp one.

The fix direction is to sample the material's own env cube for the mirror term
and keep the prefiltered chain for what it is for — roughness-varying IBL.
G6 added `envMap` and `envMapCube` ids to the record precisely so the sharp
source is reachable; this defect was the first consumer of the cube id.

That landed as `cd3fbec2`, and it turned out to be three faults rather than
one. The resolves now select by authored material kind: an `EnvMapCube` is
sampled directly through the cube-shaped bindless table using `envMapCube`, and
a legacy 2D `EnvMap` is sampled from its own authored texture through the GL
sphere-map projection (correction 1 in the reference section), not through an
equirectangular formula and not through the prefiltered array at all. The same
projection error was in `pbr_ibl`'s convolution, whose 2D input is that same
authored sphere map. Both resolves and the kept forward shaders take the
reflection vector in **eye space**, which is what makes the original's metal
camera-locked. And the cube is sampled at **explicit LOD 0**: the first attempt
at this fix moved the droid by only 0.0004, because implicit LOD inside a
fullscreen resolve derives its derivatives from 8-bit G-buffer normals and slid
the mip straight back to blurred. Retro adds the sample after lighting as
`env * (1 - diffuse.a)`, in gamma space and with no Fresnel or roughness,
matching the retained forward shader exactly.

PBR keeps the roughness-prefiltered array for IBL specular and adds the sharp
authored mirror separately. Diffuse alpha partitions rather than duplicates
the response: `(1 - alpha)` is the authored mirror share, while `alpha` gates
the roughness-varying dielectric IBL share. The original mirror therefore
keeps its authored strength and IBL fills the response the original renderer
did not model without double-lighting the material.

Measured on the droid, retro variance goes 0.0110 to 0.0207 against 0.0123 in
the pre-G1 capture. That reads as an overshoot only if the pre-G1 capture is
the target, and it is not: it is reone's own former renderer, the one reported
as too dull, whose forward shader got plausible derivatives from real geometry
and so applied legitimate minification filtering that LOD 0 removes entirely.
By eye the droid gains reflected structure rather than gaining contrast, which
is the difference between sharper and merely brighter. **Left open
deliberately:** LOD 0 filters nothing at distance, so minified metal may alias.
Choosing a LOD from surface footprint rather than pinning it is a separate
policy, noted where the sampling happens.

### Where material data lives — the rule, settled in G6

G6 first carried per-object ambient and diffuse in **two new RGBA8
attachments**, 8 bytes per pixel to copy fields that already existed in the
record, and a shape where every future material field a resolve wanted cost
another attachment. G6b (`be0e8dce`) replaced that with a 16-bit **material
id** in the G-buffer and a lookup in the `instanceMaterials` buffer the
resolves can already reach through the mega-draw's own set 2. Material
transport falls from 8 bytes per pixel to 2 and the whole G-buffer from 32 to
26; `0xFFFF` is the uncovered sentinel, and more than 65,535 records warns and
drops the frame rather than wrapping, which matters because dedup keeps real
counts in the low hundreds. The rule that generalises it, and the reason it
survives real PBR textures:

| data | where it goes | why |
|---|---|---|
| **per-pixel**: roughness, metalness | a G-buffer channel, sampled in the mega-draw | it varies per texel; no id can carry it |
| **per-object**: ambient and diffuse tints, water alpha, env cube ids, derived env layer, curated overrides | behind the material id | constant across the object, so copying it per pixel is pure waste |
| **ambient occlusion** | **nowhere** | the lightmap *is* the baked occlusion for static geometry and the tracer traces the real thing — a third answer to a question two systems already answer |

**Textured PBR therefore costs no new attachments.** Roughness and metalness
need two bytes, and two are already free or about to be: `eyeNormal.a` is
written as literal zero today, and `selfIllum.a` carries
`envMapDerivedLayer`, which is per-object and moves behind the id. Packing
roughness beside the normal is also what the tracer already does — its guide
channel is literally `traced_normal_roughness`, NRD's own format — so the two
consumers' layouts converge rather than drift.

Two things to carry into that work when it happens: both consumers currently
**derive** roughness from diffuse alpha (`material.slang`'s
`clamp(alpha, 0.2, 1.0)` and its raster counterpart), so real textures must be
adopted in **one step for both** or the shared-material rule breaks; and the
curated per-category roughness and metalness overrides multiply into those
derived values today, so they need re-expressing against textured inputs
rather than against a derivation.

## G7 — shadows from real geometry — done `e7a4f5b6`

Admission takes Opaque and Transparent only, so shadow-only proxies sat outside
the merge while the old shadow pass drew exactly them. The shadow pass is now a
mega-draw like the G-buffer's: merged buffer, vertices pulled by `SV_VertexID`
from set 2, the opaque range then the material-gated cutout range, so a fence
casts a perforated shadow rather than a slab. The coverage rule is not
reimplemented — it moved into `lib/megadraw_geometry.slang` and both draws share
it, which makes it the second consumer of the classifier decision G8 still has
open. Cascades and cube faces are one multiview pass each. The proxies are
deleted rather than plumbed through: `RenderCategory::ShadowCaster`,
`shouldCastShadows`, the refresh fallback that collected non-renderable casters
and the per-cascade proxy frusta all went. Measured as real geometry, the pass
costs +0.05 ms retro and +0.21 ms PBR for four directional cascades, and +0.11
and +0.08 ms for six cube faces — which is the frame-cost delta this step asked
for, and the answer to why the proxies existed in 2003 and do not need to now.

PBR needed a second attempt to receive them at all. The first sampled the map
correctly, with raw shadow output bit-identical to retro's, and still came out
byte-identical to a build with no shadow maps, because PBR's illumination for
static lightmapped surfaces lives in the ambient term and bypassed both
`(1 - shadow)` multiplications. **Comparing against the previous commit rather
than against the pre-G1 reference is what exposed it:** one variable changes, so
a mode that does not move has no shadows whatever the screenshots suggest. The
fix decides how baked and dynamic occlusion combine, which the old renderer left
implicit — the lightmap carries visibility `1-B`, and with dynamic shadow `D`
and combined `C = max(B, D)` the maximum wins and baked visibility is never
squared.

Against the pre-shadow build, retro changed 1,250,985 pixels and PBR 323,136,
all darker in both, with character silhouettes and terrain occlusion visible in
the amplified deltas. Seven G-buffer targets stayed byte-identical between modes
and upload hash `a12866a1ebaac77d` held in all three, through this step and
every correction below.

**Five corrections followed, and every one came from play rather than from the
acceptance set.** That is the finding worth keeping: a shadow term can pass
byte-comparison and pixel-count gates while looking wrong from a camera the
captures never stood at.

- `67cbc312` **G7c.** There was no depth bias anywhere, not even in the pipeline
  cache's key; neighbouring shadow-term differences above 10/255 on the arch
  fall from 69.38% of its pixels to 20.29% once it exists. The cascades were
  also a function of the main camera, taking their extent from its frustum, so
  rotating re-rendered static geometry against a different light-space box and
  every edge crawled. Each slice now takes a rotation-invariant sphere extent
  quantised to 1/16 of a world unit, with the light-space origin rounded to
  whole texels; in a static 600x400 ground region the per-frame changed-pixel
  count goes from all 240,000 to about 6,890. And the incremental ambient factor
  `(C-B)/(1-B)` has its singularity exactly on the common case — as the baked
  term approaches one, which is what an unlit interior is, the room went black.
  Dynamic shadow now attenuates baked ambient by at most a quarter while direct
  light stays fully shadowable.
- `0fb05120` **G7d.** The sun pointed at the world origin. G7c had removed a
  camera dependency by aiming the light with `normalize(-position)`, which is
  only correct for a module centred on (0,0,0), and danm14ab's play area sits
  around x=320. The direction now comes from the light's authored orientation
  where it has one and otherwise from the centre of the non-background room
  bounds, constant per module and independent of the camera. The depth bias was
  meanwhile eating the shadows it was meant to clean up: constant bias is gone,
  slope drops to 1.0, and the receiver offsets along its normal by a swept
  0.0115 world units rather than a guessed constant. The shadow term on the same
  plaza material goes from 1.06x to 2.13x in both modes, agreeing to three
  decimals. The 25% ambient cap was replaced by a split factor, one for the
  direct BRDF term and one for ambient and IBL, which discharges correction 5 in
  the reference section. The strength driving that factor was taken from the
  ARE's authored `ShadowOpacity`, which is what the next entry undoes.
- `05e6a8f8` Reception was gated on `ModelUsage::Room`, so only level geometry
  was ever darkened. Every creature, door, placeable and piece of equipment was
  lit as though the sun reached it — including through the shadow it was casting
  itself, which is why characters read as pasted onto the scene rather than
  standing in it. Casting was never the problem; the shadow pass draws the whole
  merged scene. All world usages now receive, with GUI and camera models out by
  construction and sky domes and backdrops excluded at the call site, the only
  place that knows how a particular mesh resolved. The sun's filter, a 3x3 box
  one texel wide that can only produce ten discrete penumbra levels, became a
  16-tap Poisson disk over 2.5 texels rotated per receiver. The rotation is keyed
  on **world** position, not screen position: a screen-space key re-rolls every
  pixel as the camera moves, which would reintroduce the per-frame edge crawl
  `9ba51344` had just removed.
- `2f00b5f5` `ShadowOpacity` is a BYTE, and the retail game authors exactly two
  values across all 96 modules: 50 in 22 of them and 205 in the other 74.
  Dividing by 100 and clamping sent those 74 to fully black. Reading it as a byte
  fraction maps the pair to 0.196 and 0.804 instead, and judged side by side
  neither end is what either module wants — 0.196 puts danm14ab back near the
  washed-out state G7d fixed, and 0.804 is still heavy. Both groups want the same
  middle value, and two authored values that resolve to one answer are not a
  parameter. So the byte is logged and drives nothing, shadow strength is a
  constant 0.5 that says it was chosen, and `--shadowopacity` overrides it per
  run.
- `306cfbc6` The cascade divisors step 0.005 / 0.015 / 0.045, so each cascade's
  texels are three times wider than the last and a kernel fixed at 2.5 texels
  tripled the penumbra's world width at every split — the softening made the seam
  far more obvious than the old one-texel box ever did. The kernel is now held to
  a fixed width in **world** units, with the per-cascade texel radius derived
  from it by probing the light-space matrix with a one-unit tangent offset;
  probing rather than reconstructing from the divisors keeps it correct if the
  split scheme changes. Equal penumbra is not enough on its own, because
  neighbouring cascades carry different depth precision and snap their texel
  grids independently, so the last fifth of each cascade's depth range
  cross-fades into the next. The row-to-row step at the split falls 8.197 to
  0.045, and the median step across the whole ground halves.

## G8 — the blended pass, and the three alpha kinds

**Scope: retro and PBR.** In path-tracing mode additive sprites leave geometry
and are handled by the march (see the substage), so the blended draw there
covers lit-blended surfaces only — a filter on the range it draws, not a fork
of the draw. Briefed without this, the draw double-counts additive in traced
mode.

The G-buffer holds opaque surfaces. Everything else is a second draw over the
same merged buffer, after shading. There are exactly three kinds of alpha and
they are not variations of one thing:

| kind | example | where it belongs |
|---|---|---|
| **alpha punchcards** | leaf cards, fences, grilles | **opaque** — writes depth, discards on zero alpha; **open, conflicts with the shared coverage rule — see below** |
| **alpha emissive** | saber blades, glow decals | **transparent, additive** |
| **alpha lit + emissive** | particles, smoke | **transparent, alpha blended** |

**The last two are one draw, not two.** Output premultiplied colour and fix the
blend state at `ONE, ONE_MINUS_SRC_ALPHA`: additive is then simply alpha zero,
and alpha-blended is alpha equal to coverage. The material decides which it is
by the alpha it writes, so no second pipeline and no second pass are needed —
the distinction stops being a branch in the frame graph and becomes a value.

One thing to settle when this is built, because every consumer must agree on
it: punchcards are opaque here, discarding at zero alpha, whereas the shared
coverage rule today sends punch-through material through the non-opaque range
at the tracer's 0.5 threshold. Changing that is a change to the *shared* rule
and to the classifier, not to the raster draw alone — and since G7 the rule has
three consumers, not two. The G-buffer mega-draw and the shadow mega-draw both
read it out of `lib/megadraw_geometry.slang`, and the tracer's BLAS geometry
ranges are the third. Moving the threshold moves what casts a shadow, so the
punchcard fixture has to be judged with the sun on it.

### Sorting, which blending needs and one draw does not provide

Blended output is order-dependent and the scene graph does not sort — the
comment claiming distance-sorted transparent buckets sits above code that
builds them in node-iteration order, and the old path papered over it with
OIT, which is boxed. The tracer needs no order; raster does. So the ordering
is a **raster-side derived artifact, not part of the scene description** — the
shared upload stays byte-identical between modes and the equality check is
untouched.

The shape: **CPU-sort only the blended set — lit and additive-emissive
together, since they interleave — and feed the draw a per-triangle remap
buffer.** The set is small (a few hundred particle quads, faded meshes,
water), well inside CPU budget. Order is decided entirely CPU-side today —
`dstTriangleBase` is assigned by walking the object vector — so nothing on the
GPU has an opinion to fight.

The remap feeds **two reads, not one**, and missing the second is the bug to
warn about: the vertex stage pulls corner `k` of sorted slot `t` via
`indices[remap[t]*3+k]`, and the fragment stage must look up material by the
**original** triangle id, `materialIds[remap[base+prim]]`, because the
per-triangle material table is in merge order, not sorted order.

Sort keys come from the CPU side that already knows them: procedural quads
carry world positions in their records, mesh triangles get transform-applied
centroids. Skinned blended meshes would sort by their untransformed-bind
approximation, which is acceptable for a sort key and not worth CPU skinning.

Per-object sorting falls out for free in admission order; the remap only has
to exist where triangles of different objects interleave. Depth-test against
the opaque G-buffer, **depth-write off**, or blended fragments reject each
other.

*Proves itself:* a fixture carrying all three alpha kinds at once — an
alpha-blended pane, an additive glow and a punchcard — renders each correctly
in retro and PBR against the pre-G1 captures; sort correctness is shown by a
deliberately interleaved pair, two transparent objects whose triangles
alternate in depth, which is wrong without the remap and right with it; and
the upload hash stays equal across modes, proving the sort is a raster-side
artifact rather than a change to the shared description.

## G9 — the shared output stage: bloom, lens flares, anti-aliasing

Raster has had no anti-aliasing since G1 boxed FXAA and sharpen with the rest
of the old post chain; FSR exists but is wired only into the traced path.
G9 makes the frame's tail a **shared output stage every mode ends in**, and it
carries three things, not one:

- **Bloom, for every mode including path tracing.** The old renderer wrote
  hilights to a second resolve target and blurred them (`hilightsBlurPass`);
  the blur shaders survive in `postprocess.slang`. It is a display effect, so
  it belongs to all three modes rather than to raster's resolve.
- **Lens flares, for every raster mode.** They were in the retro pipeline as
  well as PBR's, drawn as billboards in the old post walk. Their category
  `LensFlare` is still filtered out at admission, so restoring them starts
  there, not in the shader.
- **Anti-aliasing**, with two methods:

- **FSR** at NativeAA — the temporal resolve, which **requires jitter on**.
- **FXAA** — spatial, single-frame, which **requires jitter off**, since a
  jittered frame with no temporal resolve just shimmers.

So the AA choice *drives* the jitter setting rather than sitting beside it as
an independent dial. `9ba51344` took the first half of that: `taajitter` is
still a global option, but `computeJitter` returns zero outside path tracing,
because jittering a grid nothing resolves is shimmer by construction. On a
frozen scene with a static camera it was moving 6-12% of pixels per frame in
both raster modes, and the reason it read as *shadows* crawling is a matrix
mismatch worth recording here since G9 owns the reversal: the offset only ever
reached `globals.projection`, while megadraw rasterises through
`globals.viewProjection`, which `uniforms.slang` documents as deliberately
unjittered so motion vectors stay clean. Depth was written through one matrix
and inverted through another, so every reconstructed world position wobbled by
the Halton offset each frame, and the shadow map lookup turned that into a
binary flip along every shadow edge. G9 is what opens the gate again, and it
should open it per method rather than per mode. One selector, deriving jitter,
is the shape — and it is also what makes the modes comparable, because a
retro and a traced capture at the same setting then differ in shading only.

**Open, and it has to be settled before the gate opens: path tracing shows no
image difference at all between jitter on and off.** `9ba51344` also corrected
megadraw to rasterise through the jittered projection so depth and its inverse
agree, but that correction is unexercised rather than verified, because no
measurement in traced mode moved a pixel either way. It was kept because it
matches the documented intent, not because a result forced it. Either FSR is
not consuming the offset or the offset is not reaching the sampling, and both
are defects in the one mode that has a temporal resolve today — so G9 cannot
treat "FSR requires jitter on" as established until traced output is shown to
respond to jitter at all.

The shaders survive from G1 (`postprocess.slang` still carries
`fxaaFragment`), and the FSR path exists in `fsrupscaler.cpp`; the work is
plumbing them into one selectable stage and deciding where sharpening sits
relative to it.

*Proves itself:* an edge-heavy fixture captured in all three modes under each
method, jitter derived rather than set, judged by eye against the pre-G1
retro captures for FXAA and against the current traced output for FSR; bloom
and flares judged against those same captures. Frame-cost delta per mode.

### What the legacy renderer had, and where each piece went

G1 kept every shader, so none of this is lost work — it is a wiring
inventory. Checked against the pre-G1 passes:

| feature | shader | fate |
|---|---|---|
| transparency / OIT | `oitBlendFragment` | **G8**, replaced by the sorted premultiplied draw |
| FXAA, sharpen | `postprocess.slang` | **G9** |
| bloom (hilights + blur) | `postprocess.slang` blurs | **G9**, now all three modes |
| lens flares | billboard path | **G9**, both raster modes; unfilter `LensFlare` at admission |
| sky | — | the sky chain, after G9 |
| **SSAO** | `pbr_ssao.slang` | **unowned — decide** |
| **SSR** | `pbr_ssr.slang` | **unowned — decide** |

The resolve currently runs a neutral AO of 1.0 and says so
(`pbr_resolve.slang`). SSAO and SSR are the two that deserve a real decision
rather than a slot: both are **screen-space approximations of occlusion and
reflection that path tracing computes properly**, so they are raster-only
catch-up, not shared features — worth restoring only if raster is meant to
stand on its own against the traced image rather than as its cheaper sibling.

---

# The path-tracing substage — after the raster track

Settled 2026-08-03 as design, revisable on measurement. Nothing here starts
until the raster track is done, which now means G8 and G9.

Raster owns primary visibility for every mode; the tracer becomes a lighting
strategy over shared surfaces. The frame:

```
raster geometry   →  opaque G-buffer            (shared, the megadraw)
raster blended    →  transparency layer(s)      (shared geometry, sorted remap draw)
PT pass 1         →  rays from opaque surfaces  (the G-buffer is the ray-origin set)
denoise           →  NRD over the opaque signal
PT resolve        →  opaque + blended + additive emission + sky as a layer
FSR (jitter on) / FXAA (jitter off)
```

**Considered and rejected: a "pass 2" tracing rays from transparency pixels.**
Tracing from the transparency layer again is slow, and its signal cannot be
denoised — transparency has no stable guides. Blended surfaces shade
analytically in the interim (the same lit blended draw both modes share), and
upgrade to sampling the radiance cache when it exists. Do not re-propose
pass 2; the replacement is the cache below.

**Guide-miss is the sky case.** With guides describing the first
opaque-or-cutout hit, pixels whose guide ray misses (smoke over baked sky)
must not route through NRD — a no-surface pixel denoises to zero. The
composite falls back to the raw signal there; sky radiance is deterministic
and never needed denoising. This is a preview of the PT resolve owning the
sky layer explicitly.

## The fog grid, the march, and additive as primitives

**The first fog grid, specced 2026-08-03:** a distorted player-centred world
grid — the simple incarnation of the end-state volume, built now rather than
after SHARC. Density source: the per-area scene fog parameters, plus — the census
(`714bd700`) settled which particles bake — **the Normal-blend billboard
smoke/dust/cloud families** (~2,250 emitter nodes across ~490 models, the
ambient always-on traversal load: vents, sandstorms, mist authored as
`fx_smoke`) **and fire/explosion (144 nodes), which bake with an emissive
channel** — the voxel carries density plus self-emission, and the march
integrates density × (cached radiance + self-emission), so fire both glows
and occludes as media. Everything else stays out of the density bake: crowd
sprites and birds (6,535 nodes — sprite *characters*, not media), and rain and
wave strips (shaped, directional). Additive families do not bake either — they
become the sphere and capsule primitives specced below. The census also surfaced that 1,124 lit
emitters carry an authored `tinted` flag the renderer has never consumed —
the tint colors the media when baking, so honoring it starts there. Lighting: **one sample per voxel per
frame with large temporal reuse**; consumption: **trilinear at render**, a
**software raymarch of density × emission** applied twice — after each
hardware ray segment (bounces included) and at the PT resolve when combining
final channels. When SHARC arrives later it feeds this same grid through the
resample stage; the grid's shape and consumers do not change.

**Media and additive are one march, and it is path-tracing only.** Retro does
exactly what the original did — sorted textured quads for everything
alpha-blended, analytic area fog, no grid and no marcher. It stays the
untouched fidelity reference, which is what makes the approximations below
judgeable: if a traced ring blobs, the authored ring is on screen one mode
over. PBR keeps retro's treatment for now; adopting the march there is a
later, separate decision.

**Ray-oriented additive sprites are spheres and capsules.** A billboard
oriented to face every ray has the same silhouette from every direction —
that *is* a sphere, and a stretched one is a capsule. So intersection is a
quadratic rather than plane-and-basis maths, the AABB is `position ± radius`
(exact, orientation-free), and the record — position, radius, colour — is
smaller than the quad it replaces. The census splits them cleanly: ~1,300
additive flare/glow/star/spark nodes are radial → spheres; ~493 motion-blur
streaks and 114 linked lightning emitters are elongated → capsules. Colour is
a load-time property (texture average × tint × alpha), which keeps bindless
texture fetches out of the march loop entirely.

**They are volumes, not surfaces**, so the march integrates emission along
the chord instead of committing a hit. That removes the hit list, the
in-register sort and all ordering care: an additive sprite is just another
emissive term in the same integral the media already computes.

**The id grid: a uniform spatial hash, rebuilt per frame, built on the CPU.**
Membership query, not field sample — no interpolation, no resample stage, no
temporal identity, so none of the objections that shaped the radiance-cache
design apply. It is boundless (a firefight across the plaza keeps its glow)
and pathologically sparse. Insertion is self-limiting: a sprite covering more
than K cells goes to a small **overflow list tested unconditionally per
segment**, so pathological content self-selects instead of blowing up the
table — no need to know the size distribution in advance. A coarse occupancy
bitmask over the same key space keeps the common empty-cell case to a bit
test rather than a probe chain. CPU construction is not a compromise: additive
sprites are particles, already lowered CPU-side per frame in admission, so the
table builds where the data is and uploads through the existing
procedural-quad path. Watch it in Tracy; if it ever shows, the same code moves
to the merge compute, whose shape it already matches.

**The loop, one implementation for primary and bounce:**

```
DDA over id cells:
  cell occupied?  gather its spheres/capsules (plus the overflow list)
  march density across the cell span at its own step rate
    per step: sum emission of primitives overlapping this step
              composite inscatter, advance transmittance
```

DDA supplies span boundaries; the density march subdivides them, so the two
structures need no aligned resolutions. **Two ray marchers is the thing to
avoid** — primary and bounce differ only in step count and jitter, not in
code. One constraint follows from 3.7: **primary must stay deterministic**,
because additive emission routes through `noiseFree` and bypasses the
denoiser — fixed steps on primary, stochastic taps only on bounces.

*Consequence:* in path-tracing mode additive sprites leave geometry entirely,
so the blended draw covers **lit-blended surfaces only** there — a filter on
what the draw covers, not a fork of it, the same shape as the G-buffer's
opaque-and-cutout
rule. Lit blended fragments still sample the march's integrated
`(inscatter, transmittance)` output at their own depth, so glass behind smoke
dims correctly.

*The accepted approximation, to be judged against retro:* shaped additive
loses its shape — a ring becomes a blob, lightning a glowing tube. For
flares, glows, sparks and bolt cores, a radial profile is what the texture
already was, so nothing is lost; for the shaped minority it is a real change
in the primary view. If it reads badly, the outs are a small textured-quad
path for just those families, or an optional radial-UV texture lookup on
primary where step counts are fixed. Decide from frames, not in advance.

*Left open on purpose:* additive emitters lighting the media back (a bolt
illuminating the smoke around it). The per-voxel lighting sample can reach
them through the same primitive list, but the original game never did it; it
is a dial, not a requirement.

**Fog × AA, the working answer:** under TAA the march would sit as a post on
the AA result to dodge reprojection; FSR complicates that in principle — but
this engine runs FSR at **NativeAA, no upscaling**, so "before FSR at render
res" and "after FSR at display res" are the same resolution, and the choice
reduces to whether fog participates in FSR's temporal accumulation. Composite
the march **after** FSR as a post using guide depth (fog is low-frequency; it
needs no AA and gains no ghosting). And the march need not run at display
resolution at all: **march at reduced resolution** — half or quarter, as
production volumetrics commonly do — and depth-aware upsample at the
composite. That decouples march cost and resolution from the AA pipeline
entirely, which dissolves the super-resolution question for good: under any
FSR mode the march res is its own dial, and the composite upsamples to
whatever the display res is. Revisit only if fog ever carries frequencies a
quarter-res march visibly loses.

## The traced quality lane, in order

Sequenced after the fog grid and the march above. Each entry exists because of the
one before it.

1. **ReSTIR DI first.** The current light loop is already one-sample RIS
   with no reuse; reservoirs plus temporal/spatial reuse is the same
   estimator matured. It attacks variance at the source for every pixel, has
   no world structure to build or invalidate, and its temporal reuse
   reprojects against exactly the stable guide surfaces the guide fix
   provides. 3.7's unit-weight paths and unified primary visibility both
   simplify it, hence the ordering.
2. **SHARC on top, long term.** Spatial-hash radiance cache: sparse
   on-demand entries where paths land, multi-resolution through the key,
   world-anchored accumulation, and the normal in the hash key structurally
   defuses most wall-leaking. Fed by the paths we already trace; enables
   bounce shortening. Its output is point-sampled and jittered — sharp,
   sparse, noisy — which forces the next stage.
3. **Accumulation and consumption are different structures.** SHARC
   accumulates; consumers need dense, smooth, band-limited data. So a
   resample stage filters SHARC into a **dense world-space radiance volume**
   — well-posed filtering, because world-anchored resampling has no
   disocclusion. The volume's shape is one of two, decided at build:
   multi-octave cascades (discrete levels, seam interpolation at
   boundaries), or a single warped non-uniform grid centred on the player,
   **snapped in ~1 m jumps and resampled at the snap** — which converts
   camera motion into discrete amortised resample events with zero
   per-frame reprojection between snaps. Rejected as the store: froxels
   (screen-space reprojection re-imports the instability the world-space
   move exists to avoid — froxels survive only as a possible view-side fog
   integrator that holds no history of its own) and uniform world grids
   (leak-safe resolution is unaffordable, coarse resolution leaks).
4. **Volumetrics** consume the grid above. Density lives there — analytic area
   fog plus the baked smoke, dust, cloud and fire families — and radiance is
   the coarse field this stage resamples into. **Prototype transmittance
   first**: self-shadowing through the column is what makes smoke read as
   dense; inscatter without it is glowing soup and fails the look test
   immediately. **Add a Dxun exterior (4xxDXN) to the K2 fixture set before
   this work starts** — it is the all-fog stress case and the
   render-and-look-at-it rule applies.

## V — raster becomes primary visibility, and the sky composites once

### V0 — fix the baker, and make the offline asset the only sky

**There are two bakers and only one of them has a consumer.**
`src/apps/skybake` is the offline tool backlog 1.14 decided on — 1,392 lines,
casting rays from inside the shell into six faces — and its committed configs
live in `override/k1` and `override/k2`. `VulkanRayQuery::bakeSkyRoom` is a
runtime GPU bake of the same idea. Grepping `src/libs` and `src/apps/engine`
for a consumer of the offline assets returns **nothing**: every sky rendered
today comes from the runtime bake. The offline tool is the intended survivor,
so V0 is what has to be true before V2, V4 and V5 can happen at all.

Two defects, one of them structural:

- **Seams on the box geometry.** Undiagnosed. 1.14 already prescribes the
  instrument: *bake a six-colour debug sky first and confirm empirically which
  world direction shows which face.* Six flat faces make both faults
  self-evident — if seams survive on flat colour the fault is face frustums or
  edge sampling; if they vanish it is content-side (shell UV seams, tiling,
  filtering). Run that before touching anything, since it doubles as 1.14's
  axis-convention proof (KOTOR is Z-up, cube faces are Y-up, and a mirrored or
  yawed sky looks plausible enough to ship).
- **Whole-room granularity swallows the props.** Every committed config entry is
  `room = <room>` / `sky = <room>`; `grep -c meshes` over both `modules.ini`
  files returns **0**. The wiring is not what is missing — `403c0802` gave the
  tool a per-mesh list (`skybaker.cpp:88`), parses `meshes =` when a config
  carries it (`skybaker.cpp:676`) and emits a draft one when it generates a
  config (`skybaker.cpp:625`). The committed configs predate that and carry
  none, so every one of them still falls back to the draft `shellMeshes()`
  heuristic. So `001ebo16` goes in as one lump —
  the star shell *plus three asteroids, a planet and a nebula* — which is
  exactly the city-skyline / planet / asteroid content that must stay
  geometry. V0 is therefore curation, not plumbing: fill `meshes =` in for the
  rooms that hold props.

*Acceptance:* the six-colour probe renders with correct face-to-direction
mapping and no seams; a real bake of a props-holding room contains the shell
**and nothing else**; and the assets remain loadable by nothing yet — V0 fixes
the producer only, so it can be judged on its own output rather than through a
renderer that does not read it.

*Coverage is the long pole, and it is content work, not code:* 56 of 117 K1
modules and 46 of 82 K2 name a sky, and every entry is still marked
`# review`.

### V1 — hybridise

`PathTracing` bypasses raster entirely today: `VulkanScenePipeline::init`
returns before allocating the G-buffer (`scenepipeline.cpp:171-183`). There are
**three** modes — `Retro`, `PBR`, `PathTracing` — not four; `RTDebug` is still
planned.

Phase-sized, but *not* a graph rewrite: `VulkanScenePipeline` is already a frame
executor with a `VulkanSceneFramePlan`, and the raster modes already select
steps from it. What is missing is that PathTracing early-returns out of the
whole shape. Removing that reaches initialisation, target ownership, barriers
and dumping, plan construction, and the tracer's output contract.

| | change | what proves it |
|---|---|---|
| **V1a** | allocate the G-buffer in traced mode | **First establish whether this is work at all.** `--dumptargets` in PathTracing already emits `g_buffer_depth.npy` at full size, entirely zero, against raster's 1.77–645. Either it is allocated and unwritten, or `dumpTargets` synthesises zeros for an absent target. The source and the dump disagree; settle it before writing code |
| **V1b** | run the geometry pass in traced mode, write the G-buffer, discard it | `g_buffer_depth` from PathTracing **byte-identical to PBR's** at the same camera. That is the whole proof that raster visibility is right in traced mode, available before anything depends on it. Traced image unchanged; only frame cost moves |
| **V1c** | the tracer takes its primary hit from the G-buffer instead of tracing camera rays | traced output changes by design — compare distributions across three runs a side and judge the images. **This deletes the traced G-buffer instrument, so it must not land while G still needs it** |

### V2 — composite the sky once

**Scope: the raster modes.** In path tracing the sky is a layer of the PT
resolve (see the frame diagram above), which composites it against everything
else at once and cannot paint over anything. What follows is retro and PBR,
where there is a discrete transparent pass to order against.

One pass, **after the opaque resolve and before transparency**. Not at the end
of the chain, which sits past the transparent pass and would paint over
particles and lens flares:

```
G-buffer → shade → SkyComposite → transparency → post → filters
```

An integration attempt put the sky in the PBR resolve *and* in postprocess — two
implementations of one idea, the same fault that got the runtime bake deleted.
With G5 and G6 both shading from one G-buffer there is one place for it.

`depth == 1.0` on the device-depth attachment is the test. Note `sGBufDepth`
**is** device depth in `[0,1]` — `pbr_resolve.slang:63-81` states it and
reconstructs position from it. An earlier draft claimed linear view-space
distance, from misreading a `--dumptargets` dump, which linearises.

*Acceptance:* a fixture with an opaque prop, an alpha-blended particle and a
lens flare. Sky off versus sky on, filters disabled. The changed-pixel set must
be exactly the far-depth set, and the particle and flare pixels must not move.

### V3 — the tracer keeps only transport

`ptSkyRadiance` on **bounce miss** stays: that is transport, and it is what
makes the sky an environment light. Its **primary-miss** call is compositing and
moves to V2.

Two things move with it, so V3 must say what replaces them: primary-miss sky
currently feeds `outputs.noiseFree`, and it supplies the cyan sky colour the
surface debug view uses.

*Acceptance:* a fixed-seed fixture whose camera sees an opaque surface and whose
first secondary ray misses. Hash the traced result across the change.

### V4 — suppress exactly the shell

The manifest is `override/*/modules.ini`. **V0 is what puts the shell meshes
in it** — today every entry names a whole room and no config carries a
`meshes =` key, so there is not yet a per-mesh list for anything to read. Once
V0 has curated one, V4 makes the renderer read that same list, so baker and
renderer share one source of truth instead of each evaluating a rule and hoping
they agree. Today the renderer does the latter: G3 suppresses by a geometric
room heuristic, which is what 1.14 wanted deleted.

Neither game marks the shell distinctly enough to infer it. K1 omits the
walkmesh from a sky room but says nothing about props inside it; TSL flags
meshes individually and flags the props too. `001ebo16` flags all thirteen of
its meshes, and that set is the star shell **plus three asteroids, a planet and
a nebula**. Suppressing by flag deletes a planet and looks like the sky works.

*Acceptance:* capture `001ebo` and `manm26ad` and confirm the asteroids, the
planet and the Ahto City rings are **still drawn**. The bar is the props, not
the sky — a sky that renders correctly while quietly removing scenery passes
every sky-shaped test.

### V5 — delete the runtime bake

`bakeSkyRoom` and its call sites, `slang/sky.slang` and its shaderpack wiring,
and the shadow-ray candidate rejection at `slang/tracing/trace.slang:167`, which
exists only to cope with sky geometry possibly still being present. Larger than
one line: the same removal reaches the sky feature bit and the classifier that
feeds it.

*Acceptance:* `rg -n 'bakeSkyRoom|clearSkyRoom|RayQuerySkyRoom|skyAvailable' src include slang`
returns no runtime-bake remnants, and `slang/sky.slang` does not exist.

---

# The reference engines, and what they say we get wrong

Surveyed 2026-08-04 from `C:\Development\odessey` — **read-only reference
checkouts, not dependencies**:

- **xoreos** — C++ Aurora/Odyssey reimplementation. Reproduces the original's
  *fixed-function GL state machine* (actual `glTexGeni`/`glBlendFunc` calls),
  so it is the authority on **how the original sampled and blended**.
- **KotOR.js** — TypeScript/Three.js, the most feature-complete. Authority on
  **light budgets, gating policy and MDL controller semantics**.
- **kvp-main** — a Vulkan wrapper over the *retail binary*, so it observes the
  real draw stream. Authority on **blend states the game actually sets**, and
  the only source for **modern PBR over these assets**.

Where they disagree, xoreos wins on GL semantics (it emulates the state
machine); KotOR.js wins on gameplay-side policy; kvp-main wins on anything
observed from the shipping game.

## Confirmed correct — do not "fix" these

- **The env-map formula.** `color += env * (1 - diffuse.a)` — additive, no
  Fresnel, no lerp, applied *after* the lightmap multiply and *not* attenuated
  by it. All three agree (xoreos `shaderbuilder.cpp:609`, KotOR.js
  `ShaderOdysseyModel.ts:423`, kvp-main sees `ONE_MINUS_DST_ALPHA/ONE` in the
  retail stream).
- **`if (alpha == 0) discard`** is exactly the retail `glAlphaFunc(GL_GREATER, 0)`.
- **Lightmap multiplies** the diffuse result — the retail stream's
  `DST_COLOR/ZERO` pass.
- **The separate emissive/hilights buffer** shape kvp-main independently
  converged on.

## Corrections, ranked

1. **The 2D `EnvMap` is a GL sphere map, not equirectangular.** We computed
   `atan2/asin`; the original is
   `m = 2·√(rx²+ry²+(rz+1)²); uv = (rx/m+0.5, ry/m+0.5)` on the **eye-space**
   reflection (xoreos `shaderbuilder.cpp:376`). We also routed 2D env maps
   through the 128² prefiltered IBL array instead of sampling the authored
   texture. This is the path most KOTOR metal uses. **Fixed in `cd3fbec2`**,
   in both resolves and in `pbr_ibl`'s convolution.
2. **The reflection vector is eye-space**, not world-space, for both cube and
   sphere paths — hence the original's camera-locked reflection. Computed
   per-vertex from the *geometric* normal; a deferred resolve can only manage
   per-pixel from the G-buffer normal, which is an accepted divergence.
   **Fixed in `cd3fbec2`**, resolves and kept forward shaders together.
3. **Implicit-LOD cube sampling in a fullscreen resolve** slides the mip,
   because the reflection's derivatives come from 8-bit G-buffer normals.
   **Fixed in `cd3fbec2`** by pinning the authored mirror to explicit LOD 0,
   which leaves minified metal unfiltered; see the metal section under G6.
4. **`ShadowOpacity` is authored per area and we throw it away** — parsed at
   `resource/parser/gff/are.cpp:369`, unused. **Tried and rejected in
   `2f00b5f5`:** it is a BYTE holding exactly two values across the retail set,
   50 in 22 modules and 205 in 74, and neither reading of that pair is what
   either group wants, so it is logged and drives nothing while shadow strength
   is a chosen 0.5. The survey was right about the field and wrong about the
   conclusion, which is the useful shape of that finding: a parsed-and-ignored
   value is worth looking at, not worth assuming is a parameter.
   `SunShadows`/`MoonShadows` are still parsed and ignored.
5. **Split the shadow term in two** — a BRDF factor and an ambient/IBL factor.
   kvp-main's `ShadowResult { factor; iblFactor; }` exists for exactly the
   double-darkening problem G7c hit, and its shipped floors let skylight fall
   to 27–36%, far below our 25% *cap*. **Done in `0fb05120`**: `getShadow`
   returns both factors and the 25% cap is gone.
6. **Self-illum is additive** — "vanilla adds `GL_EMISSION` on top of the
   texture" (kvp-main `MaterialSystem.cpp:196`). We modulate.
7. **Additive with no alpha channel uses `SRC_COLOR/ONE`**, not
   `SRC_ALPHA/ONE` (xoreos `modelnode.cpp:684`).
8. **Keep submission order for non-opaque draws** — kvp-main's replay of the
   real game reorders *only* true-opaque depth-writing geometry. **This lands
   on G8**: if the remap sort reorders transparents, expect regressions the
   original did not have.
9. **The light budget was 8 global and 3 per model** (`videoquality.2da`,
   KotOR.js `LightManager.ts:24`). We allow 32. More lights than the artists
   authored for will not look better, it will look wrong in ways that are hard
   to attribute.

## What none of them can tell us

**Original shadows.** xoreos renders none, KotOR.js built them and disabled
them, kvp-main invented modern cascades. The only surviving statement is that
the original cast creature shadows **from the skeleton, not the render mesh**
(KotOR.js `OdysseyModel3D.ts:1230`), and that it shipped both a shadows and a
*soft* shadows toggle. Our G7 cascades are a modern reconstruction with no
reference to check against.

## For the PBR texture question, when it comes

kvp-main is the only prior art for PBR over assets that author no roughness or
metalness, and its conclusion is chastening: after building an HSV material
classifier, it **clamps metalness to 0.1** — "KotOR's gray textures are
painted, not metal" — and ships `metallicSensitivity = 0.038` against a
default of 1.0, with roughness pinned to 0.07–0.24. It also omits the `1/π`
diffuse normalisation deliberately, because art authored for fixed-function
goes too dark with it. The lesson is not the numbers; it is that deriving PBR
parameters from diffuse textures mostly needs to be turned *off*.

## Done already, so it is not re-litigated

The sky's offline half is finished and committed. `skybake` renders sky shells
into cubemaps by casting rays from inside the shell — mesh count and shape stop
mattering, tiling falls out of the hit UV, orientation falls out of the ray
direction, and a missing floor is a ray that hits nothing, black by
construction. Per-game curated configs live in `override/k1` and `override/k2`
and are committed; the 79 baked assets are gitignored and regenerated from the
player's own install.

Coverage: **56 of 117** K1 modules and **46 of 82** K2 modules name a sky. Every
entry is still marked `# review` — they are drafts, and a wrong `room =`
silently suppresses level geometry.

Backlog 1.14 carries the evidence for why runtime sky classification was
abandoned, including the 117-module sweep that killed it.
