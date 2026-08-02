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

## State

| | |
|---|---|
| **F0, F1** | done `0cc67e42` — the merge adopted raster's transform maths, and `MergedVertex` carries object-space position for the hashed alpha test |
| **F2** | done `394bf675` — the merged buffer gained `INDEX_BUFFER` usage, and the post-merge barrier names the vertex shader and index input so a raster draw cannot race the merge compute |
| **G1** | done `db668c2f` — the per-mesh path is gone; raster modes run an empty plan and present a cleared scene |
| **G2** | done `ef6c5850` — one draw over merged geometry writes the G-buffer, in both raster modes |
| **G3–G5** | shading, then shadows. Next. |
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
material behaviour that nothing else records, and G3 and G4 put them back to
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

*Proved itself* against the pre-G1 dumps, `danm14ab`, same camera: coverage
differs by **13 pixels of 2,073,600**; depth median 0.0074 world units; eye
normals **80.3% bit-identical**, lightmap 86.6%, self-illum 97.9%. Against the
traced G-buffer, 1,607,850 pixels agree, 465,730 are raster-only — the sky room
the tracer classifies away — and **none are traced-only**.

Retro's G-buffer is **bit-identical to PBR's**, which is the point: the draw is
mode-independent, and only shading will differ.

The depth residue sits at far distance in the sky room, where the merge's
transform order differs from the old vertex shader's, and in the grass band.
Both are expected: agreement was never going to be exact, which is why this
track measures distributions rather than hashes.

## G3 — retro shading on the G-buffer

Retro's material, reading the G-buffer instead of shading forward. Retro stops
being a forward renderer.

That is worth doing before PBR for two reasons: **retro is the mode expected to
look more or less the same**, so it is the only honest visual check the rebuild
has; and it is what makes a single sky composite possible at all — V2 needed a
retro special case *only* because retro had no G-buffer.

*Proves itself:* by eye, against the G1 captures.

**Three things are legitimately missing from the frame, and none of them is a
G3 regression.** Check them off before reading anything else into a
comparison:

- **Alpha-blended foliage.** The leaf canopy is absent from the G-buffer, and
  was absent from the old one too — it always drew in the transparent pass,
  which G1 boxed. Measured on `danm14ab`: old and new raster G-buffers both
  show bare trunk and branches where the traced G-buffer has a full canopy.
  Trees look dead until transparency returns.
- **Grass and particles**, gated out of the punch-through draw by construction.
- **Shadows**, until G5.

The traced G-buffer has all three, so raster-versus-traced will disagree across
exactly those pixels for the rest of the track.

## G4 — PBR shading on the G-buffer

The PBR material, same input.

**PBR is not held to its old output.** It is visibly broken today, and matching
it would preserve the bug. It is held to looking right, and to agreeing with the
traced G-buffer on geometry.

**G4 owns three fields the merged material record does not have.**
`GpuSceneMaterial` carries no `envMapDerivedLayer`, no `waterAlpha` and no
environment cube ids, so G2 writes zero for each. The old resolve read
`envMapDerivedLayer` out of the self-illumination alpha; something has to put it
back before environment mapping and water can work at all. Growing the record is
the obvious move, and it is G4's, not a G2 omission to be discovered later.

## G5 — shadows from real geometry

Admission takes Opaque and Transparent only, so shadow-only proxies sit outside
the merge while the old shadow pass drew exactly them. Shadow from real geometry
and delete the proxies rather than plumbing them through. They exist because
four cascades and six cube faces of real geometry were expensive in 2003, which
is not a constraint now.

Shadows are judged by eye, separately from the G-buffer comparison, so a
regression cannot hide behind an intended change.

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
With G3 and G4 both shading from one G-buffer there is one place for it.

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
