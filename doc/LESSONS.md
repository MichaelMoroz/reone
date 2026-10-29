# Lessons — what has already been paid for once

Decisions whose reasoning still binds, approaches rejected with the argument that
killed them, and postmortems whose lesson is not a rule elsewhere. The general rules
are in [CONVENTIONS.md](CONVENTIONS.md).

**A stale figure is worse than no figure**, because it reads as evidence. The traced
frame-time figures here predate the merge — TRC-037 exists to replace them.

## One BLAS for the whole scene

Shipped. Kept because the argument decides D4.
**The whole scene is 86k triangles** — `danm14ab`: 1048 instances, 61 skinned, 653
dangly, TLAS build 56-60 us. Roughly one modern character mesh for an entire module, so
**the two-level structure was buying nothing**, and merging deleted the per-mesh skin
barrier, the per-mesh BLAS build and the refit schedule together. Identity was the named
blocker and is cheap in a ray-query pipeline — no shader binding table, no hit groups,
just a per-triangle material index buffer at ~344 KB.

Why one structure, not two: a builder that never sees the whole scene cannot make a
globally optimal split; the two structures' bounds **overlap heavily**, because foliage is
spatially interleaved with the terrain it stands on, so a ray crossing the overlap
traverses both trees; every top-level instance costs a ray transform into object space,
where merged world-space geometry costs none; and **a rebuild never drifts from the pose
it was built for.**

Two published figures still bind. **Many small structures are worse, measurably:** 2401
BLAS of 1.5K triangles refit in 7.0 ms while 81 BLAS of 52K — *more* geometry — refit in
3.7 ms, and at 714 structures we were squarely in the bad regime. And **AMD's Vulkan build
path is pathological:** 223 ms against 30.2 ms for the same work on the same GPU under
D3D12, which scaled to our scene is 4.6 ms — the entire frame budget — so **we are
Vulkan-only and this is a real risk, not a footnote** (TRC-035, D4). A full rebuild scales
to ~0.35 ms on a 2080 Ti and BLAS memory to ~2.2 MB on NVIDIA, but **both are external
extrapolations, not measurements of this renderer**, and since those memory figures are
*post*-compaction while nothing sets `ALLOW_COMPACTION`, **compaction is worth doing**
(TRC-034). **The same buffer serves raster**, so the mega-draw and the merged BLAS are one
work item, not two.

## Traced transparency, the design (TRC-019)

Settled, unbuilt; the hybrid narrowed it to bounce rays.
**Occlusion and emission are different channels.** Stochastic transparency — commit a
candidate with probability equal to its coverage — is an unbiased estimator of the
*multiplicative* channel. **Additive surfaces are the degenerate limit:** finite energy
at zero coverage, C/α divergent, so P = 0 loses the emission, any forced P occludes what
additive never occludes, and a Russian-roulette weight is unbiased but turns a smooth
glow into per-path fireflies. The constraint that decides it: **emission routes through
`noiseFree`, which bypasses the denoiser, so additive emission must be computed
deterministically however stochastic traversal gets.**
**Traversal goes stochastic for coverage:** cutouts stay binary, blended candidates
commit with P = α, additive never commits. Nearest-committed-among-successes gives the
correct chain — for t₁ < t₂, P(scatter at 2) = (1−α₁)·α₂ — **independent of enumeration
order, provided the flips are decorrelated per ray × primitive.** Paths become
unit-weight: no throughput tracking, no restarts, one scattering event per segment.
**The number to watch:** blade-behind-smoke acquires binary per-path flicker, correct in
expectation, so compare by distribution against the enumerating tracer.

**Emission is gathered over the confirmed segment and expectation does the
compositing.** Sum everything in `[0, CommittedRayT()]`; if the smoke's flip commits the
segment ends there, and if it passes the glow counts at full strength — expectation
Le·(1−α). **The commit decision *is* the transmittance sample.**

**The decision is a flat O(N) loop** over a per-frame list of world-space additive
primitives. At this game's N — bolts, sabers, a few decals — brute force beats any
acceleration structure: wave-uniform trip count, records through the scalar cache,
texture fetches only on hits inside the segment, one traversal per segment, no second AS
to build. **Crossover is a few hundred primitives**, and the escape hatch is a dedicated
additive TLAS. Two rejected rungs are traps worth naming: an in-loop `(t, Le)` array
**must** be filtered by the final committed t, because unordered enumeration makes naive
summing **biased by traversal order, differently per GPU**; and accumulating while
tracking `maxAdditiveT` is exact only when that max is under the committed t, which makes
the fallback *rate* a hardware property.

**Shadow rays do not change** — they already skip additive, and a deterministic (1−α)
product is strictly lower-variance than a coin flip.

**Sabers and bolts become analytic capsules.** The mesh saber is crossed additive planes
assuming the viewer is the camera — an assumption bounce rays violate, since a blade
reflected in a floor is seen edge-on and degenerates into a line. The evaluation becomes
the **line integral of an emission density** around the blade axis: view-independent, a
few dozen ALU, and the segment bound clips it for free. The swing trail is a
deterministic chain of decaying capsules from a CPU-side transform history — **not**
stochastic time sampling, which would inject noise into `noiseFree`. Two requirements
ride with them: **one shared Slang function, two callers**, so **the modes agree by
construction instead of by two implementations of one glow**; and **parameters come from
authored content**, a deliberate departure from the 2003 look to be decided explicitly
rather than discovered.

## Coverage is transmission — and three constants disagree

**A blended surface is glass with an IOR of 1.0:** shade it, weight by alpha, continue
with `1 - alpha`. That is what makes smoke *lit* rather than pasted on, and needs no
ordering, blend state or second pass.

**How many transmitting hits a ray may cross has three answers that do not agree:**
additive pass-through 16, blended transmission 12, **shadow ray uncapped.** A camera ray
through the Dantooine plume stops after twelve layers; a shadow ray from the same point
walks all 232 and reports the surface fully occluded. **So the smoke is lit by rays that
see a twelfth of the volume and shadowed by rays that see all of it** — the leading
explanation for the black band, consistent with traced diffuse at 0.00003 against
0.05318 on the character beside it.

Underneath sits a question the caps cannot answer. **Single scattering with exact
attenuation and nothing scattering back in gives black by construction**; real smoke
reads bright because of multiple scattering this integrator lacks. Making the limits
agree is necessary; **if it is not sufficient, the answer is a scattering approximation
rather than a larger number.**

Two consequences compound: **blended hits must not write the denoiser guides** — a thin
quad has neither the depth nor the motion of the surface behind it — and **a NaN in a
guide spreads across the frame instead of staying in its pixel** (TRC-001). 
## The sky classifier was built, measured, and rejected

A geometric classifier accepting a room only as a cube-like enclosing shell, exactly one
per level. **It was built, it worked, and a 117-module sweep says it is not enough: 61
modules had a sky before, 15 after, 46 lost.**

**The rejections are not a threshold that needs tuning.** 28 of the 46 fail the panel
gate because of *one extra mesh sharing the room with the sky*, and the Ebon Hawk sky
shared by four modules is **one mesh, 120 triangles, one tiling starfield** — a shape a
six-panel model cannot express at all. Hence the curated list, keyed by room: **naming
the room is what makes the classifier deletable rather than merely bypassed.** The faces
are baked in world coordinates and sampled with the world direction, so the runtime
carries no orientation — which makes the axis convention the one thing worth **proving**,
since **a mirrored or yawed sky looks plausible enough to ship.**

A related trap: the *old* classification was a **selfIllum luma test**, so any fully
self-illuminated interior surface — lit panels, screens — could be misclassified as sky,
which applied the sky dial to it and moved it to an instance-mask bit where **shadow rays
pass through it.** Enclosed scenes are the regression test (TRC-039).

## Geometric grass versus alpha-tested quads

Grass is punch-through quads, so **every ray-triangle hit runs a candidate shader**, which
drops out of fixed-function traversal on exactly the geometry rays cross most. Opaque
blade geometry lets traversal commit and stop; against it, 4-8x the triangles and a slower
BLAS build, per frame rather than amortised because clusters materialise as the camera
moves. **Measure the ceiling before building anything:** mark grass opaque and re-run —
the image is wrong, but the frame time is the upper bound on what geometric grass can win.
A cheaper middle option is to **trim the quads to the opaque region of the texture.**

## Rejected approaches

**Retained registration, rejected in favour of the snapshot.** Three needs get conflated
under "retained": stable identity, persistent renderer caches, and a retained
registration *protocol*. **Ray tracing needs the first two, not the third**, and the
third costs **an invalidation contract**, whose failure mode is a stale value much harder
to find than a missing draw call. It also cannot be built here: `_nodes` holds a
`shared_ptr` to every node ever created and is never erased from, so **nothing is ever
destroyed until the `SceneGraph` dies** and "unregister on leaving the scene" has no
event to hang on. **The asymmetry is the whole argument: under a snapshot the `_nodes`
leak is a memory bug to fix on its own schedule; under retained registration it is a hard
prerequisite that gates the path tracer. Pick the architecture where the scene graph's
weaknesses stay bugs instead of becoming blockers.** Age-out alone is not sufficient — a
live object that swaps a texture keeps its id forever — but **the snapshot *reports* the
version where a retained scene would have to *announce* the change. S2 must not quietly
become a retained protocol.**

**The dangly motion threshold and baking the wind loop.** None shipped, because one
merged BLAS removed the problem, and **the CPU cost was never the argument** — allocation
and fill measured 0.065 ms/frame, so the win was BLAS count. One live observation
survives: **the forcing is only the object's own motion and wind, and wind is not
authored** — it is hardcoded as `0.01f * abs(sin(_windTime))` along world X, with
`_windTime` starting at zero on every node and advancing by the same `dt`, **so under a
fixed timestep every dangly mesh sits at the same phase forever** (STR-027). For a
stationary tree the displacement field is then a pure function of (mesh, orientation,
phase) — **the same few answers recomputed 653 times** — which points at baking the loop
or sharing between instances at the same orientation. **The in-phase wind is arguably a
bug of its own**, and seeding from the node id would look better, **though it would
destroy that sharing, so decide which is worth more.**

One constraint outlives all of it. Moving the spring to compute **will change the
rendered image** — GPU float will not reproduce the CPU bit for bit and the simulation
integrates, so differences accumulate rather than cancel. **Every verification here has
rested on bit-identical frames, and this is the first change that cannot meet that bar.
Decide the replacement standard before starting**, and note two runs at the same frame
must still be byte-identical to **each other**.

**An "invariant" partition of the snapshot.** A node is invariant when its transform is
fixed *and* nothing feeding its material moves — decidable once, at init. Two cautions:
the invariant part **still needs to be in the snapshot every frame** because passes select
from it, so this saves the building and not the walking, **and the walking is the larger
half**; and it reintroduces the invalidation contract the snapshot was chosen to avoid.
**Measure the invariant fraction of a real area first.**

**`Material::staticObject` does not mean what it looks like.** `setStatic(true)` is
called in exactly one place, over room model nodes, skipping the room's animated subtree.
So **it is a room flag**, and **it constrains the transform, not the material** — a static
node still runs UV and bumpmap animation, so scrolling water is static by this flag while
its uv changes every frame. Admission already declines to trust it, so nothing depends on
it. **Do not widen it without checking what each candidate does when the player interacts
with it: "it has not moved yet" and "it cannot move" are different claims, and only the
second is safe to bake into a BLAS** (STR-011).

**Grass and particles staying out of the acceleration structure — rejected twice.** The
reviews treated current behaviour as a constraint when it was a known bug: the tracer
matched only `RegisteredMesh`, so **grass and particles were invisible to the path tracer
— no shadow, no reflection, no occlusion, and a ray passed straight through a hillside of
grass. A correctness gap, not a design choice.** A third review argued they are
camera-dependent by construction, so a pose correct for the primary camera is wrong for a
bounce ray. **True, and not a reason to do anything differently: the orientation error on
a blade seen from a bounce ray is small; the error from grass not existing in the
structure at all is total.** If it ever visibly matters — grass swimming in a mirror —
**revisit it then, with the artefact in hand rather than in principle.** The same argument
settles camera-facing particles: **a rasterised composite over the traced image cannot be
in the BLAS at all.**

**"Equally useful to raster" — three of four claims were wrong**, and the shape of the
mistake is what to keep: **the opaque/non-opaque partition is not the same split** as
raster's; the `InstanceMaterial` table is **wrong to share**, since raster reads fields it
does not carry; and the bone pool is wrong twice over, because the merged result is
already world-space skinned. And **"one path" means one canonical output, not one
upload** — the merge *reads* the per-mesh uploads, so what the geometry track deletes is
raster **drawing** from them.

## Postmortems whose lesson is not yet a rule

- **The `traceStats` atomics were ~95% of the traced frame.** Instrumentation that is not
  free must be measured as part of what it measures.
- **Point lights became spheres: the derivation was right and the objective was wrong.**
  The maths was checked repeatedly while the question it answered was never re-examined.
  **Check what a derivation is optimising before checking the derivation.**
- **A one-time exception to a delegation rule propagates into every later brief.**
  Exceptions are copied forward; rules are not re-read.
