# Phase F — what to do next

`cleanup-plan.md` is the record of how the engine got here. This file is the
work that remains, and nothing else.

Steps are named `G1…` on the geometry track and `V1…` on the visibility track,
because the old `F0…F10` numbering outlived three restructures and stopped
meaning anything. Commits before 2026-08-02 refer to F0, F1 and F2; those are
done and are listed under **State** below.

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

**Capture rules, everywhere:** `--dev 0`, or the frame-time readout forges a
difference, and `--grassdensity 1`, because `reone.cfg` is graded away from
defaults and wins any flag not passed. Traced output is nondeterministic —
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
| **R1** | in flight — persistent registration: the scene description stops rebuilding every frame. Tracy-measured target: collectInto 0.73 + admission 0.60 = 1.33 ms/frame → under 0.30 |
| **R2** | delete the translation layer: nodes own GPU-shaped records, classification moves to material-set time, the Registered* intermediates die |
| **G6–G8** | PBR shading, shadows, the blended pass. After R1/R2. |
| **V1–V5** | the visibility track and the sky. After G. |

## Two tracks, and why geometry goes first

Phase F named two independent things: raster **consuming** `GpuScene`, and
raster **becoming primary visibility** for every mode. Neither needs the other.

The ordering is forced, and not by preference. **The geometry track measures
against the traced G-buffer, and V1 deletes the traced primary visibility that
produces it.** Finish with the instrument before removing it.

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

Also boxed for later, deliberately: **OIT, SSAO, SSR**. Not wrong — just not
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
| raster-only | 464,982 — the sky shell, see below |
| both covered | 1,608,598 |
| depth error > 1 unit | **206 px, 0.0128%** |
| depth median / meanabs | 0.000106 / **0.0038** world units |
| eye normal meanabs | **0.00237** |

Retro's G-buffer is **byte-identical to PBR's** across all six targets, which is
the point: the draw is mode-independent, and only shading differs.

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

So the fix direction is now settled rather than open: **the tracer's guide
surface should be the first opaque-or-cutout hit — the rule raster already
implements** — while blended surfaces keep contributing radiance in the layer
loop. That realigns the two consumers, restores the coverage agreement, and
removes the reprojection artifacts in one move. It is the same change backlog
3.7 wants for other reasons (stochastic coverage, flat emission loop), so it
lands there, on the tracer side; raster is already correct.

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
- **Shadows**, until G7.

The sky is a third difference of a different kind: raster still admits the sky
shell as geometry and the tracer does not, which is the 464,982 raster-only
pixels. That is a defect rather than an accepted difference — G3 removes it.

## R1 — the scene stops rebuilding itself

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

## G6 — PBR shading on the G-buffer

The PBR material, same input.

**PBR is not held to its old output.** It is visibly broken today, and matching
it would preserve the bug. It is held to looking right, and to agreeing with the
traced G-buffer on geometry.

**G6 owns three fields the merged material record does not have.**
`GpuSceneMaterial` carries no `envMapDerivedLayer`, no `waterAlpha` and no
environment cube ids, so G2 writes zero for each. The old resolve read
`envMapDerivedLayer` out of the self-illumination alpha; something has to put it
back before environment mapping and water can work at all. Growing the record is
the obvious move — G3 is rebuilding that record anyway, so G6 states what it
needs and G3 leaves room for it.

## G7 — shadows from real geometry

Admission takes Opaque and Transparent only, so shadow-only proxies sit outside
the merge while the old shadow pass drew exactly them. Shadow from real geometry
and delete the proxies rather than plumbing them through. They exist because
four cascades and six cube faces of real geometry were expensive in 2003, which
is not a constraint now.

Shadows are judged by eye, separately from the G-buffer comparison, so a
regression cannot hide behind an intended change.

## G8 — the blended pass, and the three alpha kinds

The G-buffer holds opaque surfaces. Everything else is a second draw over the
same merged buffer, after shading. There are exactly three kinds of alpha and
they are not variations of one thing:

| kind | example | where it belongs |
|---|---|---|
| **alpha punchcards** | leaf cards, fences, grilles | **opaque** — writes depth, discards on zero alpha |
| **alpha emissive** | saber blades, glow decals | **transparent, additive** |
| **alpha lit + emissive** | particles, smoke | **transparent, alpha blended** |

**The last two are one draw, not two.** Output premultiplied colour and fix the
blend state at `ONE, ONE_MINUS_SRC_ALPHA`: additive is then simply alpha zero,
and alpha-blended is alpha equal to coverage. The material decides which it is
by the alpha it writes, so no second pipeline and no second pass are needed —
the distinction stops being a branch in the frame graph and becomes a value.

One thing to settle when this is built, because both consumers must agree on
it: punchcards are opaque here, discarding at zero alpha, whereas the shared
coverage rule today sends punch-through material through the non-opaque range
at the tracer's 0.5 threshold. Changing that is a change to the *shared* rule
and to the classifier, not to the raster draw alone.

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

## Why the traced G-buffer is trustworthy

Measured on `danm14ab`, 2026-08-02:

- traced and raster agree on **1,650,826 pixels**
- **35** are traced-only — alpha-tested foliage, where raster's diffuse alpha is
  zero and the tracer still hits geometry
- the other **422,739** are the sky, which is the decided change rather than a
  discrepancy: raster draws a room, the tracer classifies it

So the tracer is not missing whole categories. Worth repeating on a
creature-heavy and a particle-heavy module before leaning on it everywhere.

---

# V — raster becomes primary visibility, and the sky composites once

## V1 — hybridise

`PathTracing` bypasses raster entirely today: `VulkanScenePipeline::init`
returns before allocating the G-buffer (`scenepipeline.cpp:191-207`). There are
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

## V2 — composite the sky once

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
**is** device depth in `[0,1]` — `pbr_resolve.slang:62-78` states it and
reconstructs position from it. An earlier draft claimed linear view-space
distance, from misreading a `--dumptargets` dump, which linearises.

*Acceptance:* a fixture with an opaque prop, an alpha-blended particle and a
lens flare. Sky off versus sky on, filters disabled. The changed-pixel set must
be exactly the far-depth set, and the particle and flare pixels must not move.

## V3 — the tracer keeps only transport

`ptSkyRadiance` on **bounce miss** stays: that is transport, and it is what
makes the sky an environment light. Its **primary-miss** call is compositing and
moves to V2.

Two things move with it, so V3 must say what replaces them: primary-miss sky
currently feeds `outputs.noiseFree`, and it supplies the cyan sky colour the
surface debug view uses.

*Acceptance:* a fixed-seed fixture whose camera sees an opaque surface and whose
first secondary ray misses. Hash the traced result across the change.

## V4 — suppress exactly the shell

The manifest exists: `override/*/modules.ini` carries `meshes =` naming the
shell mesh by mesh, and `skybake` bakes exactly that list (`403c0802`). V4 makes
the renderer read the same list, so baker and renderer share one source of truth
instead of each evaluating a rule and hoping they agree.

Neither game marks the shell distinctly enough to infer it. K1 omits the
walkmesh from a sky room but says nothing about props inside it; TSL flags
meshes individually and flags the props too. `001ebo16` flags all thirteen of
its meshes, and that set is the star shell **plus three asteroids, a planet and
a nebula**. Suppressing by flag deletes a planet and looks like the sky works.

*Acceptance:* capture `001ebo` and `manm26ad` and confirm the asteroids, the
planet and the Ahto City rings are **still drawn**. The bar is the props, not
the sky — a sky that renders correctly while quietly removing scenery passes
every sky-shaped test.

## V5 — delete the runtime bake

`bakeSkyRoom` and its call sites, `slang/sky.slang` and its shaderpack wiring,
and the shadow-ray candidate rejection at `slang/tracing/trace.slang:134`, which
exists only to cope with sky geometry possibly still being present. Larger than
one line: the same removal reaches the sky feature bit and the classifier that
feeds it.

*Acceptance:* `rg -n 'bakeSkyRoom|clearSkyRoom|RayQuerySkyRoom|skyAvailable' src include slang`
returns no runtime-bake remnants, and `slang/sky.slang` does not exist.

---

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
