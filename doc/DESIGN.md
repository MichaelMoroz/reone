# Design — the how and why of the open work

The mechanism of each unbuilt step and its acceptance criteria. What is built is in
[RENDERER.md](RENDERER.md); what happened along the way is in
[LESSONS.md](LESSONS.md).

## How steps are written

Steps are sized to a context, not to a coherent-looking change, because a step that does
not finish before an agent compacts gets abandoned. Each one **is briefable in a page
with no cross-references** — the step text *is* the brief — **proves its own work
positively** ("the hash did not move" shows nothing broke, not that the new thing works),
**turns on for one object before all of them** where possible, and **is committable
alone**.

## Where material data lives

| data | where | why |
|---|---|---|
| **per-pixel**: roughness, metalness | a G-buffer channel | varies per texel; no id can carry it |
| **per-object**: tints, water alpha, env cube ids, curated overrides | behind the triangle id | constant across the object, so per-pixel copies are waste |
| **ambient occlusion** | **nowhere** | the lightmap *is* the baked occlusion for static geometry and the tracer traces the real thing |

**Textured PBR costs no new attachments** — roughness and metalness need two bytes and
two are already free, and packing roughness beside the normal is what the tracer already
does, so the two layouts converge rather than drift. Two things to carry into RAS-015:
both consumers **derive** roughness from diffuse alpha today, so textures must be adopted
in **one step for both** or the shared-material rule breaks, and the curated overrides
multiply into those derived values, so they need re-expressing against textured inputs.
**The single point of change exists but is not yet single:** `resolveMaterial` holds the
base derivation, curated per-object ops, category override and roughness scale, **in that
order — the order is load-bearing and was got wrong twice**, since curation runs against
the albedo it derived, the override then *replaces* that albedo, and the scale and floor
close over whatever survived. Only the PBR resolve calls it; the two tracing modules and
the debug view still inline the same chain, so **that the four agree today is luck, not
construction.**

Two claims left deliberately unclosed: **bump has never been held to a fixture**
(RAS-013), and **the authored mirror is pinned to explicit LOD 0** (RAS-014), so minified
metal may alias. Implicit LOD in a fullscreen resolve derives from 8-bit G-buffer normals
and slid the mip back to blurred, and a compute resolve has no implicit derivatives at all
— asking for them emits `SPV_KHR_compute_shader_derivatives`, which the engine does not
request. **A footprint-derived LOD is the only available answer, not the nicer one.**

---

# G8 — the blended pass and the three alpha kinds

The draw is built: one `drawIndexed` over the pre-partitioned non-opaque range at
premultiplied `ONE, ONE_MINUS_SRC_ALPHA`, depth-tested, depth-write off.

| kind | example | where it belongs |
|---|---|---|
| alpha punchcards | leaf cards, fences, grilles | **opaque, already drawn — not this step** |
| alpha emissive | saber blades, glow decals | transparent, additive |
| alpha lit + emissive | particles, smoke | transparent, alpha blended |

**The last two are one draw.** Output premultiplied colour and fix the blend state:
additive is alpha zero, alpha-blended is alpha equal to coverage, so **the distinction
stops being a branch in the frame graph and becomes a value.**
**Do not read the tracer's non-opaque BLAS range as a classification that disagrees with
"punchcards are opaque".** Hardware traversal cannot run an alpha test, so any surface
with holes sits in a non-opaque range for the candidate loop to test — being non-opaque
*to the BLAS* is how a ray tracer expresses opaque-with-holes.
**What is open is how transmissive surfaces are lit.** The hybrid removed the mechanism
that lit them and supplied no replacement; where they are drawn is settled, and the
blended pass currently carries only additive layers, which need no lighting. **In path
tracing the draw should cover lit-blended surfaces only**, because additive sprites leave
geometry for the march — a filter that does not exist and is correct today only because
the march does not either. **When the march lands, that filter is the thing to remember
to add**; nothing in the code marks the omission.
**The threshold conflict is retro's problem, not a taste question** — the reference alpha
test is 0.1 and we use 0.5 (FID-001).
**The sort was decided against:** the draw keeps admission order, because kvp-main's
replay reorders only true-opaque depth-writing batches, so a distance sort would be a
deviation before an improvement. D2's escape hatch still points at the remap, and **its
hazard is a real trap: the remap feeds two reads, not one.** The vertex stage pulls corner
`k` of sorted slot `t` via `indices[remap[t]*3+k]`, and the fragment stage must look up
material by the **original** triangle id, because the per-triangle material table is in
merge order.

*Proves itself:* a fixture carrying all three kinds renders each correctly in retro and
PBR against the pre-G1 captures, and the upload hash stays equal across modes.

# G9 — lens flares

The anti-aliasing and bloom thirds are built; **lens flares are not, and restoring them
starts at admission, not in a shader** — the category is filtered out there, so authored
flares are registered, updated and dropped. Coordinate with STR-010, since the CPU
line-of-sight walk does not survive both.

---

# The path-tracing substage

The hybrid frame, the guide rule and the denoiser exist. **Everything below is pure
design**, and the sequencing is a **preference, not a dependency** unless a step says
otherwise — this part once claimed nothing starts until G8 and G9 are done, and the
frame landed while both were open.

**Considered and rejected: a "pass 2" tracing rays from transparency pixels.** Slow, and
its signal cannot be denoised — transparency has no stable guides — so blended surfaces
shade analytically until the radiance cache exists. **Do not re-propose it.**

**Guide-miss is the sky case:** pixels whose guide ray misses must not route through NRD,
since a no-surface pixel denoises to zero, so the composite falls back to the raw signal.
**The guide surface is the first opaque-or-cutout hit**, the rule raster implements, while
blended surfaces keep contributing radiance in the layer loop — blended surfaces in the
guides describe the glass while the denoised signal is dominated by what lies behind. At
the vent camera the fix took traced-only coverage from 831,500 to 0 and depth MAE from
2.995 to 0.0012.

## The fog grid and the march (TRC-022)

A distorted player-centred world grid, built before SHARC rather than after, and
**path-tracing only** — retro keeps sorted textured quads and analytic fog and stays the
untouched reference, **which is what makes the approximations judgeable: if a traced ring
blobs, the authored ring is on screen one mode over.**

**Density source**, settled by the emitter census: per-area fog parameters, plus the
**Normal-blend billboard smoke/dust/cloud families** (~2,250 emitter nodes across ~490
models) **and fire/explosion (144 nodes), which bake with an emissive channel** so the
march integrates density x (cached radiance + self-emission) and fire both glows and
occludes. Out: crowd sprites and birds (6,535 nodes - sprite *characters*, not media),
and rain and wave strips (shaped, directional). **1,124 lit emitters carry an authored
`tinted` flag never consumed** (TRC-023). Lighting is one sample per voxel per frame with
large temporal reuse; SHARC later feeds the same grid, and **its shape and consumers do
not change.**

**Additive sprites become spheres and capsules, not quads.** A billboard oriented to face
every ray has the same silhouette from every direction — that *is* a sphere, and a
stretched one a capsule — so intersection becomes a quadratic, the AABB is exact and
orientation-free, and the record is smaller than the quad it replaces. The census splits
them cleanly: ~1,300 radial flare/glow/spark nodes to spheres, ~493 motion-blur streaks
and 114 lightning emitters to capsules. Colour is a load-time property, **which keeps
bindless fetches out of the march loop**, and **they are volumes, not surfaces**, so the
march integrates emission along the chord — removing the hit list, the in-register sort
and all ordering care. *The accepted approximation:* shaped additive loses its shape,
which for flares, glows and bolt cores is what the texture already was. **Decide from
frames, not in advance.**

Three decisions that shape the implementation. **The id grid is a membership query, not a
field sample** — no interpolation, no resample, no temporal identity, so none of the
objections that shaped the radiance-cache design apply; **insertion is self-limiting**,
since a sprite covering more than K cells goes to an overflow list tested unconditionally
per segment, and **CPU construction is not a compromise** because additive sprites are
already lowered CPU-side per frame in admission. **One loop serves primary and bounce** —
DDA over id cells supplies span boundaries and a density march subdivides them, because
**two ray marchers is the thing to avoid** — with the one constraint that **primary must
stay deterministic, since additive emission routes through `noiseFree` and bypasses the
denoiser.** And **fog composites after FSR as a post using guide depth**, at reduced
resolution with a depth-aware upsample: fog is low-frequency, needs no AA and gains no
ghosting, and that decouples march cost from the AA pipeline entirely.

## The traced quality lane, in order

**1. ReGIR - ReSTIR reservoirs in a world-space hash grid** (TRC-026), sharing the
hashing scheme SHARC needs anyway. This replaced an earlier plan that put screen-space
ReSTIR first and SHARC on top as two structures, for three reasons. **Screen-space reuse
cannot retire the light-sampling problem** — it helps the primary vertex and nothing
else, because vertex 2+ has no pixel to reuse from, so TRC-040 survives it untouched.
**It subtracts a temporal stage instead of adding one** — screen-space reuse is a third
temporal accumulator ahead of NRD and FSR, in a chain where stabilization is already
pinned to zero because two temporal filters in series add their lag, while world-anchored
reservoirs need no reprojection and have no disocclusion. And **at our light counts it
removes the need for a light BVH entirely**: 15,270 lights across both games, K1 median
17 per module, K2 median 72, worst module 628 — a cell only has to narrow thousands to a
handful.

Decided design points. **Cell fill samples jittered points inside the voxel, not the
centre**, because a centre estimate can evaluate to zero for a light grazing at the
centre and well-lit at a corner, and **a zero in the cell pdf where the true contribution
is nonzero is lost energy**. **Occupancy is surface-scaled**, area/cell^2 not
volume/cell^3 — ~50k cells for a module with its exterior, 12.8 MB at K=16 — so **memory
does not constrain this design and should not shape it. Fill cost does, and it decides
where fill runs:** a full refresh is ~6.4M target evaluations, a sub-millisecond dispatch
and impractical on the CPU, so fill belongs beside the merge kernel. **Do not store the
surface point on an area light** — the cell target was averaged over jittered positions,
so a barycentric chosen against that average is meaningless at the shading point. **Keep
the array stride a power of two**, because TRC-004 was a grown struct turning stride into
`VK_ERROR_DEVICE_LOST`.

*Open:* whether the two payloads share one key and table or only the hashing scheme; cell
size against world scale, which wants the real light-radius distribution; and whether
cells store shadow-tested reservoirs — the one thing this shares with the BVH's blind
spot, since a cell has a position and a normal but no visibility.

**2. SHARC on top** (TRC-027), whose **update pass is separate from the render pass, and
that is a decision** — a dedicated pass at reduced resolution depositing at every vertex,
with the render pass only querying, because FSR would otherwise make cache density a
function of render resolution, full-resolution deposits concentrate near the camera where
coverage is not scarce, and `ptspp` is a performance dial so coupling ties cache fill rate
to it. **The normal in the hash key structurally defuses most wall-leaking**, and
**per-vertex light sampling is a precondition for SHARC being worth reading** — with NEE
at the primary only it caches the hemisphere-only estimate.

**3. Accumulation and consumption are different structures.** SHARC accumulates; consumers
need dense, band-limited data, so a resample stage filters it into a **dense world-space
radiance volume** — well-posed, because world-anchored resampling has no disocclusion.
**Rejected as the store:** froxels (screen-space reprojection re-imports the instability
the world-space move exists to avoid) and uniform world grids (leak-safe resolution
unaffordable, coarse resolution leaks).

**4. Volumetrics** consume the grid (TRC-028). **Prototype transmittance first:**
self-shadowing through the column is what makes smoke read as dense; inscatter without it
is glowing soup and fails the look test immediately. **Add a Dxun exterior to the K2
fixtures before this starts** (TOOL-022).

## Grass in the TLAS: chunked cluster BLASes (TRC-051)

With the instance mask on, a ray crossing 507k grass cards costs nothing extra, so
everything grass costs the traced frame is paid **before a ray is cast** — the
instance-record pass and the TLAS build, both linear in TLAS instances, today one per
card.

The unit becomes a *chunk* of clusters, where a cluster is the group the CPU already
grants per face, and the face selection is already persistent frame to frame. A
compute pass writes each cluster's blades as world-space triangles into a per-cluster
range of one grass buffer — the same `expandGrassBlade` math the raster pull uses, run
once into memory. One BLAS per chunk of N clusters (start at 64), built when a cluster
enters the grass radius or its face set changes. The TLAS holds one instance per chunk
with mask `kPtInstanceGrass`, so the ray-culling controls carry over; the card-template
BLASes, per-card records and their atomics go away. Per-blade variant, lightmap UV and
cull stay per blade.

**Static first.** Wind is applied when the card rows are generated, which is why the
TLAS is rebuilt every frame. The traced field is seen only by shadow and bounce rays,
and a swaying blade's shadow changes by less than the tracer's noise. If it reads
wrong, `MODE_UPDATE` on the chunk BLAS is the follow-up.

**Acceptance.** Same harness as [ray-culling.md](ray-culling.md): `tracing instances`
and `TLAS build` should sum to well under 1 ms at base clocks, from 4.5; the traced
frame with grass should approach the grass-off frame plus the raster grass cost; and a
before/after pair must show grass shadows still under the blades.

---

# The sky

**The curated list is the source of truth and the renderer reads it** — see
[GLOSSARY.md](GLOSSARY.md) for what it replaced and why a guess could not work. The
standalone offline baker is gone, so **the runtime bake is the only bake**, and since the
sky composites in every mode it is what every mode samples, through one shared
`skyRadiance` body so the resolves and the tracer cannot drift. **Backdrop cutouts are a
shading class, not a bake** — they take the sky's unlit surface model and intensity but
stay admitted, rasterized and traced, so do not conflate the two when reading a capture.

**Still open.** **Curation is the long pole and it is content work** (TRC-025): every
entry is still `# review`, and **a wrong room name silently suppresses level geometry**,
which is why the survey wrote its guesses as comments. **Whole-room granularity swallows
the props** (TRC-024): every mesh under the sky room takes the sky class, so `001ebo16`
goes in as one lump — the star shell *plus three asteroids, a planet and a nebula* — and
the format's reserved `meshes =` key is deliberately ignored. Neither game marks the
shell distinctly enough to infer it, and TSL flags the props too, so **suppressing by
flag deletes a planet and looks like the sky works.** *Acceptance:* capture `001ebo` and
`manm26ad` and confirm the asteroids, planet and Ahto City rings are **still drawn** —
the bar is the props, because a sky that renders correctly while quietly removing scenery
passes every sky-shaped test.

**Seams on the box geometry** are undiagnosed. **Bake a six-colour debug sky first and
confirm empirically which world direction shows which face:** if seams survive on flat
colour the fault is face frustums or edge sampling, and if they vanish it is
content-side. That also proves the axis convention, since **KOTOR is Z-up, cube faces are
Y-up, and a mirrored or yawed sky looks plausible enough to ship.**

**The tracer still composites rather than only transporting.** `ptSkyRadiance` on bounce
miss is transport; its other call is compositing, and since the hybrid it tests the
G-buffer's no-coverage sentinel rather than a miss — **so the two calls have stopped
being the same kind of thing even though neither moved.** The uncovered-primary sky still
routes through `outputs.noiseFree` and supplies the cyan colour the surface debug view
uses, so any change must say what replaces that.

---

# The structural track

What the renderer *is*: where scene state lives and what the CPU does per frame. The
per-step index is in [TASKS.md](TASKS.md) (STR-*); this is the reasoning behind it.

**Modularity first**, defined by two invariants every step is judged against.
**Render-side CPU is O(changes), never O(objects) or O(passes)** — the steady frame
replays this frame's change-log into GPU tables, then records a fixed set of dispatches
against persistent descriptors, so **a new render feature adds a GPU pass, a pipeline and
a shader, never a per-object CPU loop or per-frame descriptor churn.** And **the CPU/GPU
seam is a data contract, not a call contract** — scene state is defined once, in Slang, so
a new consumer needs zero scene-side C++ changes, and there is deliberately *no*
per-frame per-object hook to add code to, because **that hook is how every existing creep
entry got in.**

**Not the goal: recovering the CPU frame.** The scene→GPU pipeline is already ~0.5 ms;
the rest is `SceneGraph::update` self (~1.25 ms, uninstrumented) and `Game::update`
(~0.63 ms), mostly game logic this track does not touch. **Perf improvements fall out of
the structure; they are not its acceptance test.** Also not the goal: a second graphics
API.

**S0 is the step everything else is ordered by**, because the 1.25 ms is unattributed and
S3's ordering depends on what it turns out to be. It wants sub-zones in `SceneGraph` and
`Game` update (**there is no `updateAnimations` function to instrument** — the walk is an
inline loop), GPU timestamps without which every GPU-side claim in S3/S4 is unscoreable,
and **the scaling fixture**: spawn N of one blueprint, capture, repeat at 10×N, assert
render-side CPU zones flat within noise. That is the shadow-oracle idea applied to
performance, so **creep is caught the frame it lands.**

**S2 asks the question the whole track turns on:** why does admission exist at all if the
scene can be in the right form in the first place? Classification is a pure function of
the material, and a pure function of rarely-changing inputs is not a frame phase. What
remains genuinely per-frame is dynamic *data* — **not classification of anything.** The
**sync model is decided: per-frame-in-flight table copies plus a change-log**, not
versioned slots in one copy, because it keeps writes sequential, makes the frame's CPU
cost proportional to the log length, and gives the shadow oracle an observation point.
Two substrate decisions bind with it. **Canonical order:** incremental add/remove cannot
reproduce a per-frame walk order, so both paths order opaque-first-then-stable-id — a
one-time traced-distribution shift, byte-comparable after. **The full rebuild stays alive
as a shadow path**, hashing both ways every frame; it caught a real animation-timing bug
within hours of existing. Its acceptance is zero shadow-oracle mismatches **including a
module transition**, G-buffer dumps byte-identical between incremental and forced-full
paths **in one binary**, and upload-hash equality across all three modes.

**S3's constraint is what stays on the CPU.** Animation evaluation does, because game
logic reads bone transforms for attachments and hardpoints, so moving it would duplicate
state; only palette *scheduling* stops being per-frame. GPU particle simulation takes
**determinism by the grass precedent — an integer hash of `(emitter, particle, frame)`,
with no shared-generator ordering dependence**, a bug class already caught once. The pure
scheduling moves must keep raster captures byte-identical; dangly and particles cannot,
and are judged on the isolation fixtures plus distribution comparison.

**S4's precondition is deciding what "static" is provable from** — the admission proof,
**not the authored `staticObject` hint the code already distrusts.** The scaffolding
(`GpuSceneResidencyClass`, `Region`) exists with one hardcoded whole-scene region that
**no consumer reads**. **BLAS strategy is explicitly unchanged** until TRC-035 measures
the AMD build path. Acceptance is upload traffic ≈ the dynamic set only, measured not
asserted, and merge GPU cost dropping in proportion to the static fraction (danm14ab: 45k
of 86k triangles static).

**S6 is trailing deletion, and the RHI naming gate never promised it** — only relocation.

**One gate was tested and found not to be one.** The RHI seam was gated on the hybrid
landing so the frame shape would stop moving; it was built before, and nothing moved.
**Treat a stability gate as a preference until something demonstrates otherwise.**

---

# The reference engines

Read-only checkouts, not dependencies, **with divided authority:** **xoreos** reproduces
the original's fixed-function GL state machine, so it is the authority on how the
original sampled and blended; **KotOR.js** is the most feature-complete, and the
authority on light budgets, gating policy and MDL controller semantics; **kvp-main**
wraps the *retail binary*, so it observes the real draw stream and is the authority on
blend states the game actually sets. **Where they disagree, xoreos wins on GL semantics,
KotOR.js on gameplay policy, kvp-main on anything observed from the shipping game.**

**The intent-versus-limitation test** decides what binds retro: ask whether a finding
records **artistic intent** — the env-map formula, the sphere-map projection, the alpha
blend modes, submission order, all of which *are* the look — or **a technical
limitation**, like the eight-light budget or absent anti-aliasing, which retro may keep
and the other modes should exceed. It is not always obvious: KOTOR's 2D env maps are GL
**sphere maps**, so the eye-space projection is not stylistic at all but how the texels
were authored, and sampling them any other way reads the wrong pixels *in every mode*.

What each reference settles, what they confirm and what none can tell us is in
[FIDELITY.md](FIDELITY.md). One conclusion belongs here because it shapes RAS-015:
**kvp-main is the only prior art for PBR over assets that author no roughness or
metalness, and its answer is chastening** — after building an HSV classifier it clamps
metalness to 0.1 (*"KotOR's gray textures are painted, not metal"*), ships
`metallicSensitivity = 0.038` against a default of 1.0, pins roughness to 0.07-0.24, and
**omits the `1/pi` diffuse normalisation deliberately** because art authored for
fixed-function goes too dark with it. **The lesson is not the numbers; it is that
deriving PBR parameters from diffuse textures mostly needs to be turned *off*.**
