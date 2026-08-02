# Phase F — what to do next

`cleanup-plan.md` is the record of how the engine got here. This file is the
work that remains, and nothing else.

## How steps are written here

A step that does not finish before an agent compacts gets abandoned — that
happened three times on 2026-08-02, at ninety minutes each. So steps are sized
to a context, not to a coherent-looking change, and each one:

- **is briefable in a page, with no cross-references.** The step text is the
  brief. A brief that says "read the plan" spends the agent's context
  re-deriving what is already written down, and it derives it wrongly after
  compacting.
- **proves its own work, positively.** "The hash did not move" shows nothing
  broke; it shows nothing about whether the new thing works. Three steps
  verified only by absence and the switch fails with no way to tell which one
  lied.
- **turns on for one object before all of them.** Same plumbing either way, and
  a wrong result is one object-shaped difference instead of a whole scene.
- **is committable alone**, so a failure is discarded rather than left in a
  twenty-two-file stash nobody reopens.

And before building a metric: **render the thing and look at it.** Every metric
built on 2026-08-02 either missed the real defect or misled — the sky
continuity check said nothing about buildings baked into the horizon, and the
"holes" triage ranked a correct starfield worst. Metrics are for checking the
rest of the set once you know what you are looking at.

**Capture rules, everywhere:** `--dev 0` or the frame-time readout forges a
difference, and `--grassdensity 1` because `reone.cfg` is graded away from
defaults and wins any flag not passed. Traced output is nondeterministic —
compare distributions, never a stored number.

## State

| | |
|---|---|
| **F0, F1** | done `0cc67e42` — the merge adopted raster's transform maths, and `MergedVertex` carries object-space position for the hashed alpha test |
| **F2** | done `394bf675` — the merged buffer gained `INDEX_BUFFER` usage, and the post-merge barrier names the vertex shader and index input so a raster draw cannot race the merge compute |
| **F3–F5** | *F-geo*, below. Next. |
| **F6–F10** | *F-vis*, below. After F-geo. |

## Two tracks, and why F-geo goes first

Phase F names two independent things: raster **consuming** `GpuScene`, and
raster **becoming primary visibility** for every mode. Neither needs the other.
F5 does not need hybrid — the plan's own words are that it "only decides how
those draws are issued".

The ordering is forced, though, and not by preference. **F-geo measures against
the traced G-buffer, and F6c deletes the traced primary visibility that produces
it.** So F-geo must be finished with the instrument before F-vis removes it.

---

# F-geo — rebuild raster on `GpuScene`

## F3 — delete the old raster path, then recreate it

Incremental migration was drafted three ways — static-opaque-first, then
category-by-category with both paths live — and each was harder than the thing
it protected. The old raster path differs from `GpuScene` too much for
incremental agreement to be a useful target. So it goes first, and what replaces
it does things the way the tracer already does them.

**Keep**

- **Shadow handling.** It works, and it is orthogonal to where vertices come
  from.
- **Base materials.** Retro keeps a retro material; PBR keeps its own PBR-like
  one.

**Change**

- **Both modes fill and shade from one G-buffer.** Retro stops being forward
  rendered. This is the change that pays for itself twice: the sky composite in
  F7 needed a retro special case *only* because retro had no G-buffer, and that
  special case now never gets written.
- **Transparency is plain alpha blending.** Opaque raster for everything else.
- **FXAA and sharpen move to the end of the chain, where FSR sits.** Three
  interchangeable filters over a finished image, instead of two of them wired
  into the middle.

**Drop, into a box to reopen later:** OIT, SSAO, SSR. Not because they are
wrong — because carrying them across a rewrite of the thing they sit on costs
more than rebuilding them afterwards on a path that has settled.

### What correct means, with the old path gone

There is no old image to match. Two references replace it:

- **The traced G-buffer**, for geometry and coverage. `--ptdebugview` already
  exposes thirteen views — normals, albedo, roughness, metallic, lightmap,
  viewZ, motion, categories — each replacing shading at the primary hit. It
  reads the same `GpuScene` records the rebuilt raster path will, so a
  disagreement is a defect rather than a difference of convention.
- **Retro, by eye**, against captures taken before the deletion. Retro is the
  one mode expected to look more or less the same.

**PBR is not a reference.** It is visibly broken today, and holding a rebuild to
it would preserve the bug.

**Take the "before" captures first**, across several modules, and keep them.
Once the old path is deleted it cannot be regenerated, and a comparison nobody
took in advance is a comparison nobody can take.

### Why the traced G-buffer is trustworthy

Measured on `danm14ab`, 2026-08-02:

- traced and raster agree on **1,650,826 pixels**
- **35** pixels are traced-only — they look like alpha-tested foliage, where
  raster's diffuse alpha is zero and the tracer still hits geometry
- the other **422,739** are the sky, which is the decided change rather than a
  discrepancy: raster draws a room, the tracer classifies it

So the tracer is not missing whole categories. Worth repeating the check on a
creature-heavy and a particle-heavy module before leaning on it everywhere.

## F4 — shadows from real geometry

Admission takes Opaque and Transparent only, so shadow-only proxies sit outside
the merge while raster's shadow pass draws exactly them. Shadow from real
geometry and delete the proxies rather than plumbing them through. They exist
because four cascades and six cube faces of real geometry were expensive in
2003, which is not a constraint now.

Shadows are judged by eye. Keeping that separate from the G-buffer comparison is
what stops a regression hiding behind an intended change.

## F5 — the mega-draw, if it pays

The payoff the whole track is justified by: one draw over merged geometry
instead of ~1048 per-mesh draws.

**Gated on measurement, not on the argument.** This project has already been
wrong about exactly this — culling went from ~200 frustum tests per frame to
~9000, caching it removed the calls, and frame time changed by nothing. The
numbers that matter were taken 2026-07-28 and should not be re-derived:
registration costs **0.445 ms/frame**, the six `drawScene` walks cost
**1.873 ms**. The walks are what F5 collects; the registry removal moved them
rather than removing them.

If it does not measure faster, it is reverted.

---

# F-vis — raster becomes primary visibility, and the sky composites once

## F6 — hybridise

`PathTracing` bypasses raster entirely today: `VulkanScenePipeline::init`
returns before allocating the G-buffer (`scenepipeline.cpp:191-207`). There are
**three** modes — `Retro`, `PBR`, `PathTracing` — not four; `RTDebug` is still
planned.

This is phase-sized. It is *not* a graph rewrite: `VulkanScenePipeline` is
already a frame executor with a `VulkanSceneFramePlan`, and PBR and Retro
already select steps from it. What is missing is that PathTracing early-returns
out of the whole shape. Removing that touches initialisation, target ownership,
barriers and dumping, plan construction, and the tracer's output contract.

| | change | what proves it |
|---|---|---|
| **F6a** | allocate the G-buffer in traced mode | **First establish whether this is work at all.** `--dumptargets` in PathTracing already emits `g_buffer_depth.npy` at full size, entirely zero, against raster's 1.77–645. Either it is allocated and unwritten, or `dumpTargets` synthesises zeros for an absent target. The source and the dump disagree; settle it before writing code |
| **F6b** | run the raster geometry pass in traced mode, write the G-buffer, discard it | `g_buffer_depth` from PathTracing must be **byte-identical to PBR's** at the same camera. That is the whole proof that raster visibility is right in traced mode, available before anything depends on it. Traced image unchanged; only frame cost moves |
| **F6c** | the tracer takes its primary hit from the G-buffer instead of tracing camera rays | traced output changes by design — compare distributions across three runs a side and judge the images. **This is what deletes the traced G-buffer instrument, so it must not land while F-geo still needs it** |

## F7 — composite the sky once

One pass, **after the opaque resolve and before Transparency**. Not before
`filterChainPass`, which sits past OITBlend and PostProcessing and would paint
over transparent particles and lens flares:

```
Resolve → SkyComposite → Transparency → OITBlend → PostProcessing → filters
```

An integration attempt put the sky in `pbr_resolve.slang` *and*
`postprocess.slang` — two implementations of one idea, the same fault that got
the runtime bake deleted. With F3 giving both modes a G-buffer there is one
place for it.

Test coverage or `depth == 1.0` on the device-depth attachment. Note
`sGBufDepth` **is** device depth in `[0,1]` — `pbr_resolve.slang:62-78` states
it and reconstructs position from it. An earlier draft here claimed it was
linear view-space distance, from misreading a `--dumptargets` dump, which
linearises.

*Acceptance:* a fixture with an opaque prop, an alpha-blended particle and a
lens flare. Sky off versus sky on, filters disabled. The changed-pixel set must
be exactly the far-depth set, and the particle and flare pixels must not move.

## F8 — the tracer keeps only transport

`ptSkyRadiance` on **bounce miss** stays: that is transport, and it is what
makes the sky an environment light. Its **primary-miss** call is compositing and
moves to F7.

Two things move with it, so F8 must say what replaces them: the primary-miss sky
currently feeds `outputs.noiseFree`, and it supplies the cyan sky colour the
surface debug view uses.

*Acceptance:* a fixed-seed fixture whose camera sees an opaque surface and whose
first secondary ray misses. Hash the traced result across the change.

## F9 — suppress exactly the shell

The manifest exists: `override/*/modules.ini` carries `meshes =` naming the
shell mesh by mesh, and `skybake` bakes exactly that list (`403c0802`). F9 makes
the renderer read the same list, so the baker and the renderer share one source
of truth instead of each evaluating a rule and hoping they agree.

This matters because neither game marks the shell distinctly enough to infer it.
K1 omits the walkmesh from a sky room but says nothing about props inside it;
TSL flags meshes individually and flags the props too. `001ebo16` flags all
thirteen of its meshes, and that set is the star shell **plus three asteroids, a
planet and a nebula**. Suppressing by flag deletes a planet and looks like the
sky works.

*Acceptance:* capture `001ebo` and `manm26ad` and confirm the asteroids, the
planet and the Ahto City rings are **still drawn**. The bar is the props, not
the sky — a sky that renders correctly while quietly removing scenery passes
every sky-shaped test.

## F10 — delete the runtime bake

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
direction, and a missing floor is a ray that hits nothing, which is black by
construction. Per-game curated configs live in `override/k1` and `override/k2`
and are committed; the 79 baked assets are gitignored and regenerated by the
tool from the player's own install.

Coverage: **56 of 117** K1 modules and **46 of 82** K2 modules name a sky. Every
entry is still marked `# review` — they are drafts, and a wrong `room =`
silently suppresses level geometry.

Backlog 1.14 carries the evidence for why runtime sky classification was
abandoned, including the 117-module sweep that killed it.
