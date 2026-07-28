# Moving the renderer from submission to registration

Why the hand-off between the scene and the renderer had to change for ray
tracing, what has landed, and what is left. Written from the code as it stands;
every file:line below is a thing to read before changing it.

About half of this plan is now in the tree. `c536ac72` replaced per-pass draw
submission with one registration pass over a frame registry. The sections below
keep what that commit settled separate from what it left open, because the
remainder is where the ray-tracing constraints actually bite.

## Target architecture

Five boundaries. Everything below follows from these, and anything that does not
fit one of them is out of scope for this plan rather than an oversight.

**Ownership: the scene publishes, the renderer derives.** `SceneGraph` builds a
complete render snapshot each frame and owns it. The renderer consumes it and
owns only what it derives - acceleration structures, descriptor state, uploaded
buffers. The renderer never owns scene objects, and the scene never holds a
backend handle. This is a decision, not a placeholder; the reasoning is in
"Why the snapshot stays" below.

**Data model: four separate concepts, deliberately not one.**

| concept | lives on | lifetime | what it is for |
|---|---|---|---|
| object identity | `SceneNode` | creation to destruction | keying caches across frames |
| content version | snapshot entry | changes when the source does | telling a cache its contents are stale |
| renderable data | snapshot entry | one frame | what a pass draws or a TLAS instances |
| AS index | backend | one frame | TLAS instance index, 24-bit custom index |

The doc previously blended these into "the handle". They have different
lifetimes and different owners, and conflating them is what makes retained-mode
designs fail - in particular identity and version, because an id that is stable
across a texture swap is doing its job correctly and telling a BLAS cache
nothing. The debug names ride on identity; the material assignment is keyed by
name and is a fifth thing again, derived offline rather than per frame.

**Backend: registration is neutral, tracing is not.** The registry, the
snapshot and visibility live in `scene`, so all three pipelines get them at
once. Ray tracing is Vulkan-only and consumes the snapshot without any other
backend knowing it exists. Nothing ray-traced may appear in `IRenderPassExecutor`
or in the registry's vocabulary.

**Update: the frame boundary is the only consistency point.** There is no
publish/subscribe, no change notification, no dirty flags. The snapshot is built
in one traversal and is immutable for the rest of the frame; the renderer reads
a completed snapshot or nothing. Nothing in the renderer outlives the frame
except caches, and a cache entry is keyed by **identity plus content version**,
both reported by the snapshot entry. Disappearance is handled by an id ceasing
to appear; change is handled by a version mismatch. Neither requires the scene
to announce anything, which is what keeps destruction and module transitions
free of synchronisation.

**Visibility: one query, many policies.** Culling is a policy applied to the
snapshot, not a property of an object. `drawScene` already takes a camera per
pass; that generalises to a visibility policy - camera frustum, light frustum,
distance-and-relevance for a TLAS, or none for a full-scene bake. Each new
consumer selects a policy; none writes its own.

## What happens today

`SceneGraph::renderScene` (`src/libs/scene/graph.cpp:557-629`) walks the scene
exactly once per frame and records everything it finds into a `RenderRegistry`
(`include/reone/scene/registry.h:180`). Scene nodes write to the registry
directly - `MeshSceneNode::registerRender` (`src/libs/scene/node/mesh.cpp:248`)
and `registerShadow` (`:345`), `EmitterSceneNode::registerRender`,
`GrassSceneNode::registerLeafs` (`src/libs/scene/node/grass.cpp:159`). There is
no `IRenderPass` any more.

An entry carries what it *is*, not which pass asked for it. `RenderCategory`
(`include/reone/scene/registry.h:49`) has ShadowCaster, Opaque, Transparent,
LensFlare and Debug bits, and that is what lets one traversal serve six passes.

Each pass then calls `RenderRegistry::drawScene`
(`src/libs/scene/registry.cpp:172`) with a filter and a camera. It selects by
category, culls (`isCulled`, `src/libs/scene/registry.cpp:79`), and hands
survivors to an `IRenderPassExecutor` (`include/reone/scene/render/pass.h:52`):
`executeDraw`, `executeDrawSkinned`, `executeDrawDangly`, `executeDrawSaber`,
`executeDrawBillboard`, `executeDrawParticles`, `executeDrawGrass`,
`executeDrawAABB`, `executeDrawDebug`. Those are the only things left in the
renderer named for drawing.

The registry is cleared at the top of every frame (`resetFrame`,
`src/libs/scene/registry.cpp:90`) and refilled.

Three things that settled, which the rest of this plan assumed:

- **Culling is no longer a scene-graph decision.** `refreshFromNode`
  (`src/libs/scene/graph.cpp:302`) sorts meshes into lists but runs no frustum
  test. The registry holds the whole scene, so a structure built from it has no
  holes where the camera was not looking - which was the original reason to do
  any of this.
- **Instanced geometry has a material.** `registerGrass` and `registerParticles`
  take a `Material` like everything else, and `MaterialType`
  (`include/reone/graphics/material.h:31`) has `Grass` and `Particle`. A hit
  shader has one way of asking what it hit.
- **Registered and drawn are counted separately.** `RegistryCounts`
  (`include/reone/scene/registry.h:152`), `drawnCountsByPass` and
  `formatRegistryCounts` make "was this registered, and did it survive culling"
  a question with a numeric answer rather than a hunch.

## What the registry does not yet solve

**Only one culling policy is used.** `drawScene` takes a camera per call, so
per-pass policy is expressible - but every pipeline hands the *view* camera to
every pass, shadows included (`src/libs/scene/render/pipeline/pbr.cpp:434`,
`retro.cpp:188`, `vulkan.cpp:1017`). A shadow caster outside the view frustum is
dropped from the shadow map. That predates the registry and is not a regression,
but the registry is what turns fixing it into a one-argument change, and a light
frustum is the first policy worth adding. Distance-and-relevance for a TLAS, and
no culling at all for a full-scene bake, are the same seam.

**Entries are still partly pass-shaped.** A shadow-casting mesh registers
*twice*: once from `registerRender` with its real material, and once from
`registerShadow` with a `DirLightShadow`/`PointLightShadow` material
(`src/libs/scene/node/mesh.cpp:345-362`). Those two types are a pass wearing a
material's clothes - they say which shader to use, not what the surface is,
and the executors dispatch on them
(`src/libs/scene/render/pass/pbr.cpp:55-61`). One object should be one entry;
a pass should select a shader, not a material. Until then the entry count is
inflated by every caster, and any per-object structure keyed off entries has
to decide which of the two is real.

The shadow entry passes `{}` for its deformation, but this costs less than it
looks. `shouldCastShadows` (`src/libs/scene/node/mesh.cpp:201-213`) excludes
skin meshes on creatures outright, so **skinned meshes never cast shadows at
all** and no character is shadowed from its bind pose. What does lose its
deformation is a dangly or saber caster, which is not a skin mesh and so
passes the predicate. Fixing that is not a registration change:
`slang/shadow.slang:25-33` has a single vertex stage taking `POSITION` and
`localUniforms.model`, with no skinned, dangly or saber variant, so deforming
shadows need new shader entry points and pipeline keys on both backends. Keep
it separate from the entry merge.

**Nothing has identity between frames.** Unchanged by the registry, and now the
largest gap. See below.

**Visibility is a hardcoded test, not a selectable policy.** `isCulled` moved
out of `SceneGraph` into `RenderRegistry`, which is enough to unblock a complete
TLAS, but it is still one function every caller gets rather than a policy a
caller chooses. Which library the code sits in matters less than that: under the
visibility boundary above, a consumer picks a policy and none writes its own, and
that is what is missing.

## Why the snapshot stays

Earlier drafts of this plan treated a per-frame rebuild as a stopgap and fully
retained registration - `registerObject`, `unregister`, update paths - as the
inevitable end state. That was asserted, never argued, and it is wrong. The
snapshot is the architecture. Retained registration is an optimisation that the
code does not currently justify and probably never will.

Three needs get conflated under "retained":

1. **stable object identity**, so a cache can be keyed across frames;
2. **persistent renderer caches** - BLAS, uploaded buffers, descriptors;
3. **a retained scene-to-renderer registration protocol.**

Ray tracing needs the first two. It does not need the third, and each of them is
satisfied by a snapshot that carries stable ids:

- **Rigid BLAS keys on `graphics::Mesh`, not on a scene node.** The bulk of the
  scene is rigid, one BLAS serves every instance, and mesh lifetime belongs to
  the resource layer. This pattern already exists and works:
  `VulkanResources::_meshes` is an `unordered_map<const Mesh *, ...>`
  (`include/reone/graphics/vulkan/resources.h:149`), populated on demand
  (`src/libs/graphics/vulkan/resources.cpp:453-459`) and dropped in bulk. A BLAS
  cache is the same shape.
- **Per-node BLAS keys on object identity**, which a snapshot entry can carry as
  a plain id. The cache lives in the backend; an id that stops appearing ages
  out. Nothing about that requires the scene to announce a departure.
- **The TLAS is rebuilt every frame regardless.** The snapshot is already the
  right input shape for the one structure that is genuinely per-frame.
- **Temporal accumulation needs motion vectors**, which come from
  `prevTransform`, already on the entry.

Against that, retained registration costs an invalidation contract - the thing
this plan has warned about throughout, whose failure mode is a stale value that
is much harder to find than a missing draw call. `invalidateResources` and
`invalidateTexture` (`include/reone/graphics/renderer.h:82,91`) are what that
looks like when it is only guarding a texture cache.

And it costs more than that here, because **the scene graph cannot currently
emit the events it would need**. Creation is not the problem - every node is
built through one factory, `SceneGraph::newSceneNode`
(`include/reone/scene/graph.h:394-398`), including every child built by
`ModelSceneNode::buildNodeTree` (`src/libs/scene/node/model.cpp:59-80`), so the
"object entered the graph" hook already exists and is universal. Destruction is
the problem: `_nodes` (`include/reone/scene/graph.h:305`) holds a `shared_ptr`
to every node ever created and is *never erased from and never read*. `clear()`
drops the five root lists and leaves it untouched. Nothing in a scene is ever
destroyed until the `SceneGraph` itself dies.

So "unregister when the object leaves the scene" has no event to hang on,
because leaving the scene is not currently a thing that happens. Fully retained
registration would require centralising attachment and detachment, defining
ownership of children (`addChild` takes a `SceneNode &` and `_children` is a
vector of raw pointers, `include/reone/scene/node.h:52,147`), and making
reparenting and bulk destruction emit correct removals. That is a scene-graph
ownership project, and it buys nothing the snapshot does not already give.

**The asymmetry is the whole argument.** Under a snapshot, the `_nodes` leak is
a memory bug to fix on its own schedule. Under retained registration it is a
hard prerequisite that gates the path tracer. Pick the architecture where the
scene graph's existing weaknesses stay bugs instead of becoming blockers.

What survives from the retained framing is the part that was always the real
requirement:

- **Identity is stable** - assigned at `newSceneNode`, carried on every snapshot
  entry the node produces, and the key for every backend cache.
- **Everything else is rebuilt** - transform, previous transform, bones, dangly
  positions, materials, instances. There is no invalidation to get wrong because
  there is nothing to invalidate.
- **Backend caches are validated, not notified.** An entry carries a content
  version alongside its id; the cache rebuilds on a mismatch and releases on an
  id that has been absent for N frames. Age-out alone is not sufficient - a live
  object that swaps a texture keeps its id forever - and the difference from a
  retained protocol is that the snapshot *reports* the version where a retained
  scene would have to *announce* the change.

`Material::staticObject` still stops being advisory and starts selecting BLAS
build flags - `PREFER_FAST_TRACE` plus compaction against `PREFER_FAST_BUILD`.

Revisit only on measurement: if snapshot construction shows up in a profile
after the `Material` copy is gone (see "Registry lifetime" below), reconsider -
and reconsider by moving specific expensive fields out of the snapshot, not by
adopting a retained protocol wholesale.

## Acceleration structures

The axis that matters is not "skinned or not" but **whether a BLAS can be
shared between instances**.

| category | BLAS | keyed on | per frame |
|---|---|---|---|
| rigid mesh | shared | `Mesh` | nothing |
| skinned / dangly / saber | one each | scene node | refit |
| grass / particles | one unit quad, static | - | TLAS instances only |

Rigid meshes have fixed object-space vertices, so one BLAS serves every
instance and the TLAS carries the transform. Deforming meshes differ per
*node*, not per mesh - two characters sharing a model are in different poses -
so each needs its own, refit each frame.

### What the registry is actually made of

Before optimising, the composition. `danm14ab`, frame 310, from
`formatRegistryCounts`:

| | registered | drawn opaque | drawn transparent |
|---|---:|---:|---:|
| rigid | 744 | 237 | 38 |
| skinned | 61 | 13 | 0 |
| dangly | **653** | 7 | **544** |
| saber | 4 | 0 | 4 |
| emitters / particles | 41 / 55 | - | - |
| grass | 1 node / 1482 clusters | 1 | - |

**544 of the 586 transparent draws are dangly meshes.** Transparent plus dangly
is foliage, and it dominates everything: there are ten times more dangly meshes
than skinned ones, and the transparent pass is very nearly nothing else.

That reorders the deforming-geometry work. Skinning is the interesting problem
and dangly is the expensive one.

### Optimisation: foliage, not skinning, is the deforming cost

Each of those 653 dangly meshes computes its vertex positions on the CPU every
frame, allocates a vector for them (`src/libs/scene/node/mesh.cpp:318-326`), and
under ray tracing would need **its own BLAS refit every frame** because dangly
positions differ per node. Six hundred refits a frame for leaves.

Three things worth trying, cheapest first:

- **Freeze what is not visibly moving.** A dangly mesh whose positions have
  barely changed since its last build does not need a refit. A per-node motion
  threshold turns most of the foliage static for most frames, and the data to
  decide it is already computed - the displacement is right there in
  `_dangly.vertices`.
- **Ask first whether it needs simulating at all.** It is a real spring-damper -
  per-vertex velocity and displacement integrated across frames
  (`src/libs/scene/node/mesh.cpp:135-176`) - but look at what drives it. The
  artist supplies `displacement`, `tightness`, `period` and per-vertex
  `constraints`. The forcing is only two terms: the object's own motion, and
  wind. Wind is **not authored** - it is hardcoded as
  `0.01f * abs(sin(_windTime))` along world X - and `_windTime` starts at zero
  on every node and advances by the same `dt`, so under a fixed timestep every
  dangly mesh in the scene sits at the same phase forever.

  For a stationary tree the motion term is zero, so wind is the only input and
  the response is periodic with a 2π-second loop. **The displacement field is
  then a pure function of (mesh parameters, orientation, phase)** - not
  per-instance state in any meaningful sense. The same few answers are being
  recomputed 653 times.

  That points somewhere better than a faster simulation:

  - **bake the loop.** Precompute the cycle once per distinct mesh and sample
    it, and the per-frame integration disappears entirely;
  - **share the result.** Two instances of the same tree at the same
    orientation have bit-identical geometry, so they can share one BLAS with the
    TLAS carrying the transform - which is the rigid case again, and collapses
    hundreds of per-node structures. Orientation is the wrinkle: the wind is
    rotated into object space, so instances at different rotations diverge.
    Simulating in world space instead would remove that dependency and widen the
    sharing to every instance of a model. Worth checking whether it changes the
    look before assuming it is free.
  - **the in-phase wind is arguably a bug of its own.** Every tree in the area
    waves in perfect sync because nothing offsets `_windTime` per node. Seeding
    it from the node's interned name or id would cost nothing and look better,
    though it would also destroy the sharing above - so decide which is worth
    more before doing either.

  Note the CPU cost is *not* the argument here: dangly allocation and fill were
  measured at 0.065 ms/frame combined. The win is the BLAS count, which is a
  ray-tracing concern, not a frame-time one.

- **Move the simulation to compute.** Still right for whatever survives the
  above, and required regardless for skinning. The positions must be
  GPU-resident for tracing, so computing them on the CPU and uploading is paying
  twice. It is the same pass skinning needs - both write deformed vertices into
  a buffer a BLAS can build from - so build the mechanism once and give it two
  kernels.

  What it needs, and what it breaks:

  - **Per-node persistent state.** The simulation integrates velocity and
    displacement across frames (`src/libs/scene/node/mesh.cpp:135-176`), so the
    buffer has to survive between frames and be found again next frame. That is
    exactly what `SceneNodeId` and the cache-version rule exist for; this is the
    first thing that genuinely consumes them rather than carrying them.
  - **Static inputs upload once.** Base positions and per-vertex constraints
    come from `mesh->danglymesh` and never change; only the transform, previous
    transform, wind time and timestep vary per frame.
  - **It will change the rendered image.** Floating-point on the GPU will not
    reproduce the CPU's results bit for bit, and the simulation integrates, so
    small differences accumulate rather than cancel. **Every verification in
    this project so far has rested on bit-identical frames, and this is the
    first change that cannot meet that bar.** Decide the replacement standard
    before starting - a bounded per-pixel difference over a foliage region, or a
    deliberate re-baseline with the old images kept - rather than discovering
    mid-change that the usual check no longer applies.
  - **Determinism must survive.** Captures have to stay reproducible run to run
    even if they no longer match the CPU path, so the kernel must not depend on
    dispatch order or uninitialised state. Two runs at the same frame must still
    be byte-identical to each other.
- **Alpha-tested foliage is a known ray-tracing cliff.** Transparent geometry
  needs any-hit shader invocations rather than the fixed-function path, and 544
  alpha-tested quads across the view is exactly the shape that hurts. Decide
  deliberately whether foliage enters the TLAS at all, the same question the
  plan already asks about particles.

  **It also has a hard prerequisite and a silent failure mode.** Marking foliage
  non-opaque makes `RayQuery::Proceed` start returning true on candidates, and
  the loop in `slang/rayquery.slang` currently has an empty body because every
  instance is opaque. An empty body neither commits nor ignores a candidate, so
  the hit is dropped and alpha-tested geometry **disappears from the traced view
  while still rendering in raster** - which looks like anything except a
  traversal bug. Resolving a candidate means sampling the alpha at the hit,
  which needs the texture reachable from the shader, so this cannot land before
  the bindless material work. Order: material buffer and bindless textures
  first, then non-opaque foliage, and never the reverse.

### Optimisation: one object per body part

A character is not one mesh. `c_drdwar` registers **30 entries** - `belly`,
`chest`, `neck`, `head`, `l_upperarm`, `lfngr1` and so on - and `c_khounda`
registers 23, eight times over. Under rasterisation that is 30 draw calls;
under ray tracing it is 30 TLAS instances for one droid.

Two distinct cases, and they want opposite things:

- **Rigid characters** - droids are assembled from rigid parts, each with its
  own transform. Expressing them as a single skinned mesh with bone transforms
  would collapse 30 instances into one BLAS plus a refit. **The trade is real
  and needs measuring**: rigid parts currently *share* their BLAS across every
  instance of the model, so eight `c_khounda` cost 23 structures and 184
  instances. As skinned they would cost eight structures, eight instances and
  eight refits per frame. Fewer instances, but sharing is lost.
- **Genuinely skinned characters** already deform per node, so merging the parts
  of one character into one BLAS costs nothing and removes instances outright.
  There is no sharing to lose.

Either way the merge is per *model*, not per node, and wants the model root -
which registry entries now carry as `cullRoot` and a `SceneNodeId`.

### Optimisation: rebuild only what changed

The current build is per mesh on first use and the TLAS is rebuilt per frame,
which is right for a first version and wrong afterwards. What each category
actually needs:

| | when |
|---|---|
| static room geometry | once, at load |
| rigid props | on move, or never |
| skinned | refit per frame while visible and animating |
| dangly | refit per frame, subject to the motion threshold above |
| TLAS | rebuilt per frame regardless - it is cheap and everything moves through it |

**Refit is not rebuild**, and the distinction is the whole optimisation: a refit
keeps the existing tree and moves its vertices, costing a fraction of a build,
but degrades traversal quality as the pose drifts from the one the tree was
built for. So refit per frame, rebuild occasionally - on a large pose change, or
every N frames staggered across objects so the cost does not land in one frame.

An object that is neither visible nor animating needs neither. That is a
per-object decision the snapshot can already express, since it knows what was
drawn and in which pass.

### Optimisation: a light hierarchy, once areas get dense

Direct lighting selects one light per sample by importance - contribution
estimated as multiplier times attenuation times NdotL times colour luminance -
which makes shadow-ray cost constant per sample instead of scaling with the
area's light count (`kMaxLights` is 32). The selection scan itself is still
linear over the active list, and the estimate ignores occlusion.

Landing that selection took five falsified theories, and the postmortem is
recorded because the real culprit had been billing this renderer since the
first traced frame. Ebon Hawk, 4 spp, timed as capture-to-frame-900 minus
capture-to-frame-300 over 600 frames: 36.3 ms before, 61.1 ms with selection
walking the light buffer inside the sample loop. Not the shadow rays
(disabled entirely: 61.5), not a struct copy's register pressure (59.2), not
per-sample buffer traffic (hoisting the walk to a per-pixel weight table:
61.3). Pixel scope with multiplicity-deduplicated shading: 53.7. Dropping
the scratch-resident weight array and batching what was then a per-ray stats
atomic: 42.8. Putting **all** traceStats counters behind a debug flag,
default off: **1.68 ms**. The frame was 25x faster than every number ever
measured for it; the counters - unconditional `InterlockedAdd`s on one
4-byte word from every thread, serialized by the hardware as global atomic
contention - were ~95% of "the cost of the path tracer," including the
36.3 ms baseline, and their per-sample variant is precisely why the
selection landing looked 25 ms slower. Selection itself: one buffer walk per
pixel totals the weights and feeds the ambient flat term; each sample draws
one random number against per-light intervals recomputed bit-identically; a
light picked by several samples is shaded and shadow-traced once with the
multiplicity folded into the unbiased estimator, at a warp-uniform light
index. Shadow rays per pixel: 1.8 at the Hawk, 2.7 in the 18-light cantina.

Directional light records (`position.w == 0`) are promoted to sampled suns in
the same walk: no attenuation, ambient-only flag overridden, one shadow ray,
`ptSunIntensity` dial. Raster has no parity target here - its sunlight is
baked lightmaps plus cascade darkening. The promotion shipped dead the first
time: the sky dome is opaque TLAS geometry between every surface and the far
sun origin, so every sun shadow ray committed on it - measured contribution
+0.0000 while costing a full-length traversal. Sky instances now carry TLAS
instance-mask bit 2 (world is bit 1); camera and bounce rays trace with the
full mask, shadow rays with bit 1 only. Sky is environment, never occluder.

A flat power CDF is not a usable stage once emitter geometry joins the set:
importance must be weighted by each light's angular area *from the shading
point* - power alone ignores distance squared and orientation, so beside one
lamp among hundreds of panes nearly every sample lands elsewhere - and a
per-point CDF is O(N) per shading point. The entry requirement is therefore
the light BVH itself (the Conty-Kulla many-lights family): per node spatial
bounds, an orientation cone and aggregate power, descended stochastically per
sample with each branch chosen by the receiver-dependent estimate and the pdf
accumulated as the product of choices. O(log N) per sample, no CDF ever
materialised, leaves small enough that a local pick is O(leaf). Built per
frame beside the TLAS from the same snapshot. A flat CDF survives only as a
debug fallback for validating the estimator against brute force.

The same estimator absorbs emissive geometry. The several hundred emissive
panes currently light the scene only when a hemisphere ray happens to hit one
- high variance that no sample count fixes cheaply. The design: extract
emitter triangles (non-sky - the sky's huge solid angle is exactly where
hemisphere sampling wins, and kTraceSky already separates it) into the same
sampled light set as the point lights, one power CDF over both record types,
one NEE sample and shadow ray per path sample, weighted by the geometry term
over area pdf - angular-area importance in the estimator, with per-point
angular selection arriving with the light BVH. Emitters managed this way
return zero emission to bounce rays - their light arrives via the estimator -
but full emission to the camera ray, or directly-viewed screens would go
dark while indirect light double-counted. Per-triangle power in the CDF is
area times luma of selfIllum times diffuseColor, a stated approximation:
average texel emission is not known CPU-side, while the shader samples the
true textured emission at the chosen point.

### The calibration programme and the observed-defect ledger

Recorded 2026-07-29 from a live review of the traced image. These are the
user's observations with technical analysis attached; together they define
the calibration programme that replaces ad-hoc dial tuning.

**The lighting model this converges on.** The image gets a **tonemapper**,
and light intensities rise substantially so bounce lighting actually
registers - the current near-1.0 dials produce first-bounce energy too weak
to see. Calibration rules: direct light alone should land around **0.3-0.5x
of the final rgb** in directly-lit areas of real scenes (leaving visible
headroom for indirect), and direct intensity is set so lit areas match
**retro**, which stays the reference for "correctly lit". Default bounces:
2. The sky wants roughly **2x its current intensity**, and the sun should
ultimately be part of the sky - a job for future HDRI replacements rather
than the current promoted-directional stopgap. In this frame, brightness
above the old raster capture is dynamic range, not overshoot; the earlier
"3.2x ground" reading compared untonemapped traced output against PBR
raster, which is neither the visual reference (retro is) nor evidence of
an energy bug by itself. The sky-plus-lightmap double count on outdoor
statics remains a real accounting question, but its resolution lives inside
this calibration, not in a clamp.

**Ambient light has no place in path tracing.** The world-ambient term and
the ambient-only flat irradiance pre-pass are raster survivals; both retire
as light intensities rise and transport carries the frame. (The
ambient-only records themselves do not vanish - see the light notes below -
but their flat, unshadowed application does.)

**Artist-placed lights encode emission intensity.** Odyssey levels fake GI:
many point lights sit directly at emissive panels. Those pairings are a
measurement: the point light's intensity estimates the emission strength of
the panel it fakes, which is exactly the calibration the area-light/NEE
stage needs. Rules derived: a point light with no emissive surface nearby
is a real light and stays; **angular size must never be zero** - point
lights get a relatively wide angular size (soft shadows), the directional
sun stays fairly sharp; and some directional lights exist purely to emulate
GI (one of the stunt levels demonstrates this) - reference material for
what the bounce lighting should reproduce, and candidates for removal once
it does.

**Sky leaks into fully enclosed scenes** (Taris underground). Two suspect
mechanisms, both checkable: the sky classification is a selfIllum luma
test, so any fully self-illuminated *interior* surface - lit panels,
screens - can be misclassified kTraceSky, which both applies the sky dial
to it and, worse, moves it to instance-mask bit 2 where **shadow rays pass
through it**; and module seams can let hemisphere rays escape to the dome.
The classification needs to be scoped to actual sky geometry (model or
node identity, not luma), and enclosed scenes are the regression test.

**Some objects do not interact with lighting**: leaves, hair, Manaan
puddles, doors everywhere, parts of levels. These are the surfaces routed
through the transparency candidate path - `CandidateLayer` radiance is
albedo-times-lightmap-ish and never sees the sun, scene lights, or the
hemisphere. Anything classified punch-through or TransparentModel
(including doors via material type, hair via blending) shades through that
unlit path wherever alpha is below the opaque threshold. The layer model
needs direct lighting, or near-opaque texels need to commit into the full
shading path far more aggressively.

**Main screen render is incorrectly cropped** - the live window appears to
render at full resolution and crop rather than scale. Output-extent
plumbing between the trace target and the presented image; not a capture
issue.

**Feature ledger** feeding the same programme:

- **Debug view modes**: lights, object bounding boxes, emissives
  highlighted, object type - selectable from the render-registry ImGui
  window, which already knows every entry.
- **Area lights from emissive mesh parts** (triangle granularity):
  emission removed from the pathtraced direct hit and moved into NEE with
  importance sampling - the design above, now with the panel-adjacent
  point lights as its intensity calibration.
- **Multiple importance sampling** between the specular and diffuse lobes,
  replacing the current single-lobe russian-roulette split.
- **Much more ImGui control** over the tracer: per-category material
  property overrides - including a **material color override per object
  category** (rooms, doors, placeables, creatures, grass, ...) alongside
  roughness, emission, and env strength - so calibration hypotheses can be
  tested live instead of by rebuild. A flat-color override per category is
  also the quickest visual isolator: paint every door magenta and the
  "doors don't interact with lighting" class of bug identifies itself.

### Optimisation: one BLAS for everything static

The table above shares a BLAS between instances of the same mesh. The next step
is to stop having instances at all for geometry that never moves: bake every
static object into **a single BLAS with world-space vertices**, leaving one TLAS
instance for the whole static world plus one per dynamic object.

danm14ab currently builds 484 structures for 744 instances. Most of that is
room and placeable geometry that will never move, so the traced frame is asking
the hardware to traverse a top-level structure of hundreds of boxes to reach
what is really one rigid scene. Merging gives the builder the whole static set
at once, which is where it can do its best work, and collapses the top level to
almost nothing.

What it costs, and what has to exist first:

- **Vertices must be pre-transformed to world space**, so nothing is shared and
  the merged buffer is as large as the static set. That is the trade: memory
  and a build, against traversal.
- **The material lookup changes.** Today the instance custom index carries a
  `SceneNodeId` and one instance is one object; merged, a hit lands somewhere
  inside one instance and needs the geometry index or a primitive-to-material
  table to say what it hit. That has to be built before the merge, not after.
- **It has to be rebuilt when the static set changes** - module transition,
  a door that is really animated, anything toggled by a script. Cheap if it is
  genuinely rare, and a correctness bug if the set is less static than
  `Material::staticObject` claims. Check that flag against reality first.
- Distinguish static-and-never-moves from static-and-currently-still. The flag
  was advisory until it started selecting build flags, and this makes it
  load-bearing in a second way.

**Check what the flag actually means before relying on it.** `setStatic(true)`
is called in exactly one place - `src/libs/game/object/area.cpp:472-488` - over
room model nodes, skipping anything under the room's `"{modelName}a"` subtree,
which is where its animated geometry lives. So:

- it is a **room** flag - `setStatic` runs inside `Area::loadLYT` over the
  layout's rooms, and nothing else is ever marked. That reads like an
  under-count until you ask what else would qualify: a placeable opens, a door
  swings, a creature walks. Room geometry outside the room's own animated
  subtree may be exactly the set that never moves, in which case the flag is
  drawn correctly rather than drawn short. Do not widen it without checking
  what each candidate does when the player interacts with it - "it has not
  moved yet" and "it cannot move" are different claims, and only the second one
  is safe to bake into a BLAS. For scale, danm14ab's six room models are
  roughly 940 of the 1508 entries against about fourteen for every placeable
  and door, so the flag already covers the bulk of an outdoor area;
- it constrains the **transform, not the material**. A static node still runs
  `updateUVAnimation` and `updateBumpmapAnimation`
  (`src/libs/scene/node/mesh.cpp:108-129`), so scrolling water in a room is
  static by this flag while its `uv` and `bumpMapFrame` change every frame.

### The same question, applied to registration

If an object cannot change, re-registering it every frame is waste, and
registration is the measured 0.445 ms - so this looks like the lever. It is,
but not through `staticObject`, which as above says nothing about the material.

What would work is a stricter, computed test: a node is **invariant** when its
transform is fixed *and* nothing feeding its material moves - no UV animation
(`mesh.uvAnimation.dir` zero), no cycling bumpmap, no alpha or self-illum
controller. That is decidable once, at init, from data already loaded, and it
partitions the snapshot into a part that could be built once and a part that
must be rebuilt.

Two cautions before building it. The invariant part still needs to be *in* the
snapshot every frame, because passes select from it - so this saves the
building, not the walking, and the walking is the larger half. And it
reintroduces exactly the invalidation contract this plan chose the snapshot to
avoid; the difference is that the predicate is computed from immutable data
rather than maintained by hand, which is what makes it tractable. Measure the
invariant fraction of a real area first: if it is not most of the 1508 entries,
the complexity is not worth 0.4 ms.

Worth doing after the traced image is correct and measured, not before: it is
an optimisation whose whole benefit is traversal cost, and there is no traversal
cost worth optimising until the thing renders what it should.

Only **skinned** meshes need a compute pass to produce vertices. Dangly
positions are already computed CPU-side (`src/libs/scene/node/mesh.cpp:135-176`,
gathered into an array at `:318-326`); saber is a single displacement vector and
is cheap to apply on the CPU rather than run a pass for.

For grass and particles, prefer **one static unit-quad BLAS plus N TLAS
instances** over baking instances into a per-frame BLAS. The TLAS is rebuilt
every frame regardless, so this costs nothing extra and avoids rebuilding a
BLAS over hundreds of thousands of triangles. The cost is instance-buffer
bandwidth, 64 bytes each; distance culling cuts it.

## Two things that break under ray tracing regardless

**Camera-facing geometry.** `ParticleInstance`
(`include/reone/scene/registry.h:69`) carries `right` and `up` computed per
frame from the camera (`src/libs/scene/node/emitter.cpp:290-308`). Correct for
the primary view, meaningless for a reflection or shadow ray, and edge-on such a
quad is invisible. Options: a spherical or cross-quad proxy in the AS, or keep
particles rasterised and composite them over the traced image. Choose
deliberately rather than discovering that smoke has vanished from a reflection.

**Grass placement is view-dependent.** See below.

## What grass actually is

Not an object with an extent and a region. `GrassSceneNode` holds a reference to
an `aabbNode` - a mesh from the room model - and at
`src/libs/scene/node/grass.cpp:53` keeps every face of it whose `face.material`
appears in `_properties.materials`. The grass region *is* the union of those
faces, fixed at init. Each cluster inherits the face's lightmap UV via
`tryFaceUV2`, so grass is lit by the room's lightmap.

`update()` is then pure view-dependent LOD: clusters beyond
`kMaxClusterDistance2` (`:45`) return to a fixed pool of 2048 (`:42`), faces
coming into range draw from it, count per face derived from area by
`getNumClustersInFace` (`:188`).

So "grass everywhere it can go" is already well posed - the tagged faces. What
is view-dependent is only how many are currently realised, which is exactly the
policy to move into the renderer. `radius` on `registerGrass`,
`kMaxClusterDistance2` and the cluster pool all leave the scene node.

**Placement is not deterministic today.** `getRandomGrassVariant`
(`src/libs/scene/node/grass.cpp:192`) draws from a global RNG in materialisation
order, so the same face yields different grass depending on when the camera
approached it. Invisible while there is one viewpoint; immediately visible under
ray tracing when a reflection shows a hillside populated differently from the
direct view. Derive position and variant from a hash of (face index, cluster
index) instead. Worth doing on its own - it also makes captures reproducible.

**Measure the total instance count** for a real outdoor area before assuming
"everywhere" is affordable: grass faces times clusters per face, summed, in
something like danm14ab. `registeredCounts().grassClusters` already reports it.

## Materials

`Material` (`include/reone/graphics/material.h:40`) carries type, a
`TextureUnit -> Texture &` map, a `mat3x4` UV transform, colour, bump-map frame,
ambient/diffuse/self-illum colours, the static/shadows/fog flags, and optional
blend, cull and polygon-mode overrides. Everything else is derived by
`materialFeatureMask` (`:69`).

Two changes remain:

- Flatten `textures` (`:44`) from an `unordered_map` of reference wrappers into
  fixed slot indices into a bindless descriptor array. A hit shader must reach
  any material and any texture at intersection time; it cannot bind six units
  before a draw. **This is also the single biggest per-frame cost in the
  registry today** - see below - so it pays twice.
- Serialise the rest into a GPU material buffer indexed by TLAS instance custom
  index.

Note what is *not* there: no metalness, no roughness. Roughness is scavenged
from diffuse alpha, which since the environment-strength change also carries
reflection strength; the MDL reader reads specular and shininess and discards
them. A hit shader will be inferring PBR terms in exactly the place the deferred
resolve infers them today, which is the right place for that to stay.

### The visual target is the retro look, reached physically

The traced image should read like the original renderer's frame - that is what
the artists lit for - but by physical means only: no clamps, no additive
grafts, no gamma-space arithmetic. The look lives in the authored data, so the
tracer honours the data rather than imitating the math. Retro's `min(1, light)
* albedo` shoulder and gamma-space sums are not reproducible by a physical
renderer and are not targets; retro captures serve as an *aspect* reference -
which surfaces sheen, where light comes from, lit-to-shadow balance - not a
pixel-ratio one. Concretely: the envmap is authored incident radiance, so
envmapped surfaces sample it through the GGX lobe (later: trace the reflection
and fall back to the envmap on miss, so near geometry reflects for real).
Outdoor brightness overshoot is an energy accounting question, not a grading
dial: lightmaps already bake sun and sky for statics, so live sky at the
primary hit plus lightmap cache at the bounce can count the same photon twice.
Which authored source owns which light path must be decided per path, the
same masking discipline the NEE design already states for emissive panes.

## What this deletes

Several G-buffer packing tricks exist only because a deferred resolve cannot
reach the material:

- the environment-map derived layer index, smuggled through self-illum alpha as
  a byte (`slang/pbr_resolve.slang:195`);
- the geometry feature bits packed into lightmap alpha (`:34`, unpacked at
  `:177`).

A hit shader indexes the material directly, so both stop being load-bearing -
and with them goes the class of bug where a stale layer index bleeds onto a
surface that never had an environment map.

## Objects have no identity yet

Every backend cache the tracing work needs is keyed on an object still being the
same object next frame, and the scene has no way to say so.

- `SceneNode` carries no id and no name. It has `IUser *_user`, but `IUser`
  (`include/reone/scene/user.h:24`) is an empty interface - a virtual
  destructor and nothing else - so it is a back-pointer tag with nothing
  readable on it.
- Game `Object::id()` is a real, stable `uint32_t`
  (`include/reone/game/object.h:85`), but reaching it means `SceneNode::user()`
  plus a downcast to `game::Object`, and the scene library does not depend on
  game. The registry cannot use it without inverting that dependency.
- `graphics::ModelNode::number()` is a `uint16_t` unique *within a model*
  (`include/reone/graphics/modelnode.h:179`). Useful as half a composite key,
  useless alone.
- Pointer identity is what is actually used - `VulkanResources` caches by
  `Mesh *`, and `RegisteredMesh` carries a `ModelSceneNode *cullRoot`
  (`include/reone/scene/registry.h:108`).

Pointer identity has a failure mode this codebase already knows about.
`IRenderer::invalidateResources` exists for it, and says so: a backend caching
by address "cannot otherwise tell that an address has been reused by a
different object, and would hand a new mesh the previous one's buffers". That
is a bulk sledgehammer swung on module transition. Adequate for a texture
cache; useless for a TLAS instance index, where what matters is knowing *which*
object went away.

**Give `SceneNode` a stable id** - a `uint32_t` index and a `uint32_t`
generation, or one packed 64-bit value. `SceneGraph::newSceneNode`
(`include/reone/scene/graph.h:394-398`) is the single point every node passes
through, so assignment has a home already. The generation bumps on destruction,
so a reused slot yields an id that compares unequal to the stale one.

Two caveats, both from the target architecture rather than from taste:

- **The id is not the AS index.** A TLAS instance index is backend-local and
  rebuilt every frame; it is derived from the id, not carried in it. The 24-bit
  custom index constrains the mapping, not the id's width.
- **The generation cannot advance yet.** Nothing is ever destroyed - `_nodes`
  never releases - so stale-id detection is a field that will read correct
  because it never changes, which is worse than not having it. Add the field,
  do not rely on it, and fix `_nodes` before anything does.

What the id buys: a snapshot entry gets a key that survives a frame, a per-node
BLAS and a material assignment get something to hang off, and a backend cache
can be validated per entry instead of cleared wholesale. The id alone is only
half of that - it says *which* object, not *whether what was derived from it is
still current* - so the entry carries a content version beside it.

Cheap now. Awkward once things are keyed on pointers.

### Identity should carry meaning, not just be unique

A bare integer identifies an object and tells you nothing about it. Debugging
this renderer is mostly the question "what is that thing" - most of a night
went into a missing head that would have been half an hour if a pixel could
have named itself.

Nothing needs authoring; the names already exist and are simply not reachable
from the scene library:

- `graphics::Model::name()` is the MDL resref - `c_drdastro`, `m14aa_01a`
  (`include/reone/graphics/model.h:43`);
- `graphics::ModelNode::name()` is the node within it - `head_g`, `wall_01`
  (`include/reone/graphics/modelnode.h:180`);
- `game::ObjectType` gives Creature, Placeable, Door, Trigger and the rest
  (`include/reone/game/types.h:248`), where a game object is behind the node.

**Intern them; do not store strings.** A registry entry is copied for every
object every frame, and a `std::string` per entry would cost more than
everything else in it. Keep a string table on the scene graph and put a
`uint32_t` name id beside the object id. Copying stays a few words, and the text
is resolved only when something actually displays it.

So a node's identity is roughly an index, a generation, a type, and two interned
name ids - model and node. Small enough to sit in a snapshot entry, and enough
to answer "what is that" without a second lookup structure.

The two halves have different jobs, and it is worth being explicit about
which is which:

- **the integer is for indexing and identity** - the registry key, the TLAS
  instance index, what a cached BLAS hangs off. It must be cheap, dense and
  meaningless.
- **the strings are for heuristics** - deciding what a surface is made of,
  which is a question the assets cannot answer.

That second one is not a debugging luxury. Odyssey assets carry no metalness
and no roughness at all: the MDL reader reads specular and shininess and drops
them, roughness is scavenged from diffuse alpha, and `metallic` is hardcoded to
zero in both resolves. Every PBR parameter beyond the environment strength is
currently invented. Names are the only remaining signal, and the data already
uses them that way - `cm_baremetal` against `cm_dantne` are environment maps
whose names describe the material, not the geometry.

So a resref of `c_drdastro`, a node called `head_g`, a texture prefixed `cm_`
or a walkmesh surface type are all evidence about what something is made of.

Two tiers, in this order:

**1. An authored name-to-material map.** Data, shipped with the project and
under version control, keyed on the names above. Most specific key wins - model
plus node beats model, which beats texture. This is the source of truth, and
being data means a wrong assignment is a one-line fix by anyone, not a rebuild.

That needs **an editor mode to maintain it**, and the registry is what makes
one possible: it already knows every object in the frame and, with the name ids,
what each is called. So the mode is a list of what the scene actually contains,
a material assignment per entry, and a save. The useful part is the inverse
view - which registered objects have **no** authored entry - because that is
the work queue, and it is measurable as a percentage rather than a feeling.

**2. A heuristic for everything unmapped.** Pattern matching over the same
names for the long tail nobody will ever hand-assign. It will be wrong
sometimes, which is why it must be **visibly** a guess: the editor should show
which tier decided a given surface, so an inferred material is never mistaken
for an authored one, and so a bad guess is findable rather than merely
suspected.

Keeping both in one table rather than spreading inference through the shaders
is what makes either correctable.

Where it pays:

- the render target viewer can name what is under the cursor;
- a hit record in a path tracer carries an instance index, which resolves to
  `c_drdastro / head_g` rather than to a number;
- material inference gets a place to live that is not a hardcoded constant in
  a resolve shader.

One constraint from the destination: a TLAS instance custom index is 24 bits.
That bounds how many instances one frame's mapping can address, not the width
of an id - the mapping is rebuilt with the TLAS.

## What the per-frame rebuild costs

Clearing and refilling every frame is correct by construction - there is no
invalidation contract to get wrong - and it is a stopgap, not the end state.
Two reasons, and only one of them is cost.

### The performance budget

The traced frame targets **200 fps at 1 spp on simple scenes** - 5 ms of
frame budget on the development machine's RTX 5090. That hardware traces
billions of rays per second; at ~1080p and 1 spp the frame needs roughly six
million rays (primary, bounce, shadow), a fraction of a millisecond of pure
ray throughput. The target is currently **met at 4 spp**: 1.68 ms at the
Ebon Hawk, 2.74 ms at the Taris cantina, once the stats-counter atomics
moved behind a debug flag (see the light-hierarchy section for that
postmortem - every earlier cost figure for the traced frame was ~95%
counter serialization). The standing disciplines survive the win: GPU
timestamps around TLAS build and trace dispatch are still owed so future
regressions get attributed by facts rather than five falsified theories,
and every optimisation must name the milliseconds it claims. Headroom work
when the budget tightens again - denser scenes, more bounces, pane NEE -
remains the structural list: leaner dispatch shape, merged static BLAS,
refit-over-rebuild. Feature trims stay off that list.

### Measured, 2026-07-28

The registration refactor cost real frame time and this is what it was. OpenGL,
`danm14ab`, `--pbr 1`, capture harness, wall clock differenced between a
300-frame and a 900-frame run so startup and module load cancel, with a
warm-up run discarded first.

| commit | ms/frame | |
|---|---:|---|
| `a7b2bf0f` | 4.51 | immediately before the registry |
| `c536ac72` | 5.62 | the registry - **+1.11 ms, +25%** |
| `72fb544e` | 5.01 | after step 1, which recovered 0.61 ms |

The remaining ~0.5 ms is **snapshot construction**, timed directly rather than
inferred. Per-slot, `a7b2bf0f` against `c536ac72`: Graphics render +1.146 ms,
Update +0.544 ms, input and audio unchanged. Inside HEAD's Graphics slot:

| phase | ms/frame |
|---|---:|
| snapshot registration (`renderScene`) | **0.445** |
| the six `drawScene` walks | 1.873 |
| `_objects.clear()` destruction | 0.020 |
| `resetFrame` bookkeeping loop | 0.002 |

Registration is a stage that did not previously exist - the old traversal was
fused into drawing - and it accounts for 85-90% of the residual.
`RegisteredObject` is **488 bytes**, so 1508 entries are ~0.70 MiB constructed
and destroyed per frame, and the variant makes a three-entry billboard pay the
largest alternative's width.

**Ruled out, with numbers**, because negative results are what stop the same
guesses recurring:

- the 653 dangly position vectors: **0.065 ms/frame** including both allocation
  and fill - real, and an order of magnitude too small;
- the dead `drawnPasses` clear in `resetFrame`: **0.002 ms**;
- **caching the cull test per model root: no effect at all.** Before the
  registry, `cullModels` computed visibility once per root per frame and
  culled subtrees wholesale; now `isCulled` runs per entry per pass, ~9000
  times against ~200. A one-slot cache removed those calls and moved frame time
  by nothing, because an AABB-frustum test is tens of nanoseconds. **Reasoning
  from a ratio of call counts is how that hour was lost** - attribute cost to
  something measured, not to something that merely happens often.

Two traps in measuring this again. A run immediately after a build pays a
one-time shader and pipeline cache cost that inflates the 300-frame baseline
and silently deflates the difference; that produced a nonsense 1.755 ms reading
once. And `checkIdentityStability` fires whenever the Graphics channel is on
and exists only after `296a0474`, so instrumentation compared across that
boundary has to log somewhere else.

**Not yet decided:** whether 0.445 ms - about 9% of the frame - is worth
shrinking the entry for. `RegisteredMesh` carries three `mat4` inline, 192 of
its 488 bytes, and `transformInv` is derivable while `prevTransform` is read
only by motion vectors. That is a contained change with no architectural risk,
and it is the only lever identified so far. Retained registration is *not* the
answer here for the reasons in "Why the snapshot stays"; the cost is entry
construction, not the rebuild policy.

### Where the per-frame cost actually is

Read the copies rather than guessing at them, because the obvious suspect is
not the expensive one.

**The `Material` copy dominates, and it is not the geometry.** Every
`registerRender` builds a `Material` on the stack whose `textures` is an
`unordered_map`, inserting one to four entries
(`src/libs/scene/node/mesh.cpp:254-268`) - a bucket array plus a node each. Then
`registerMesh` takes it by `const &` and the entry copy-initialises it
(`src/libs/scene/registry.cpp:118`): a second full hash table, built and torn
down every frame. Two per entry, roughly 1,500 entries in danm14ab, before a
vertex is touched. Same pattern in `EmitterSceneNode`
(`src/libs/scene/node/emitter.cpp:309-325`) and `GrassSceneNode`
(`src/libs/scene/node/grass.cpp:174-180`).

Flattening `textures` into fixed slot indices makes `Material` trivially
copyable and this cost disappears - and that is already the bindless change the
Materials section wants for unrelated reasons. It is the whole fix; nothing
about when the registry is filled comes into it.

**Dangly positions are moved, not copied.** `registerMesh` takes
`RegisteredDeformation` by value and `std::move`s it into the entry
(`src/libs/scene/registry.cpp:111,118`), and the call site passes a prvalue
`RegisteredDangly {std::move(positions)}` (`src/libs/scene/node/mesh.cpp:326`).
One allocation per dangly mesh per frame, for the `reserve`, and the positions
are recomputed every frame anyway - so there is nothing here to save.

**Skinned meshes do copy.** `RegisteredSkin {_bones, _prevBones}`
(`src/libs/scene/node/mesh.cpp:315`) copies both vectors by value - 2 x
`kMaxBones`(24) x 64 B and two allocations per skinned node per frame. `_bones`
is a member whose capacity is already retained across frames, so the copy exists
only because the entry owns its bones rather than referencing them. Fixed by the
entry holding a span once ids make the node safe to reference for a frame.

**A cost in the other walk entirely:** `drawScene` re-copies instance arrays on
every pass that selects them. Grass instances are copied into `visible` and then
again into a 256-cluster `batch` per chunk
(`src/libs/scene/registry.cpp:238-262`); particles into `visible` (`:222-231`).
That is in the draw walk, not the register walk, so nothing about registration
affects it - and it scales with the "grass everywhere" plan above. It
wants a span over the stored vector plus a `(first, count)` pair on
`executeDrawGrass`.

Numbers before defending any of this: `formatRegistryCounts` over
`registeredCounts()` and `drawnCounts()` in danm14ab, not the estimates here.

### What this does not argue

None of the above is an argument for retained registration; see "Why the
snapshot stays". The rebuild is the architecture, and the three copies worth
removing are removed inside it:

- the `Material` copy, by the bindless flattening in step 1;
- the bone copy, by having the entry reference the node's `_bones` rather than
  own a duplicate, once identity makes that reference safe to hold for a frame;
- the instance re-copy, by spans, which is not a registration question at all.

What the rebuild genuinely cannot supply is a key that survives a frame, and
that is supplied by putting a stable id on the node - not by changing when the
registry is filled.

## o

Doing this while both backends exist means implementing it twice. The
registration split avoided that: registry and culling live in `scene`, so all
three pipelines got them at once and the executor interface is the only thing
each backend implements.

Two tracks, not one list. They share a prerequisite - object identity - and
diverge after it. Keeping them separate is the point: the material work below
kept falling out of earlier versions of this plan because a single ordered list
implies everything in it gates what follows, and that half does not.

Nothing here ends the per-frame rebuild, because the rebuild is the
architecture. What changes is that entries carry a stable id and the backend
keeps caches behind it.

Done:

- ~~Record instead of draw. Registry built, nothing else changes.~~ `c536ac72`,
  and in the scene library rather than behind the Vulkan pass as originally
  planned - which turned out cheaper, because `IRenderPass` and its per-pass
  callback map could be deleted outright.
- ~~`Grass`/`Particle` material types; `Material` on the instanced entry
  points.~~

### Critical path to a traced frame

1. ~~**Bindless flattening of `Material::textures`.**~~ Done in `96b2d432`:
   `std::array<Texture *, 6>` on a `MaterialTextureSlot` enum, `Material`
   trivially copyable, verified pixel-identical on both backends.
2. ~~**One entry per object.**~~ Done in `6d4a528f`: `ShadowCaster` rides in
   the categories, `beginPass` tells the executor which pass it is in, and
   `MaterialType` keeps only what a surface *is*. Deforming shadows were
   deliberately excluded - see "What the registry does not yet solve".
3. ~~**Light-frustum culling for the shadow passes.**~~ Done in `49496d2f`, and
   it is the cautionary entry in this list. It was written as "one argument at
   the `drawScene` call sites"; it was in fact `graphics::Frustum` extracted out
   of `Camera` plus a `VisibilityPolicy` on `drawScene`, +191 net lines across
   16 files, to admit two extra shadow casters that change **zero pixels** at
   frame 900 of danm14ab.

   The code is right and the frustum extraction removes real duplication. The
   *sequencing* was wrong: step 9 below says it "generalises step 3", which is
   the tell that they were always one piece of work. Nothing in steps 4-6 needs
   light-frustum shadow culling, so the abstraction was paid for four steps
   before its first consumer and shipped as a bug fix that fixes nothing
   measurable. `VisibilityPolicy::noCulling()` exists today with **no callers**;
   it is owed to step 8.

   **Gate the rest of this list on a consumer.** A step that only widens a seam
   for a later step should land with that step, or immediately before it - not
   at the position where the idea first occurred. Steps 3 and 4 together are
   about +256 net lines whose entire justification is steps 5-9; if those stall,
   that is dead abstraction in a shipping renderer.
4. ~~**Stable ids on `SceneNode`.**~~ Done in `296a0474`, together with interned
   model and node names, per-entry pass flags, and the registry panel that
   reads all three. They landed as one change because the panel is their only
   consumer, which is the gating rule applied rather than bent. The generation
   field is present and inert; `_nodes` never releases, so no index is reused.

#### Rigid geometry traces first

The order below changed after reading the backend. `doc/vulkan-rt-backend.md`
§10.1 wants ray *query* before a ray-tracing pipeline - no shader binding
table, no new pipeline type, a visible result early - and two facts move
everything else:

- **There is no ray-tracing groundwork at all.** `device.cpp` requests features
  11 and 13 and no device extensions of its own: no
  `VK_KHR_acceleration_structure`, no `VK_KHR_deferred_host_operations`, no
  ray query, no `features12`, so no buffer device address and no descriptor
  indexing. Nothing can build an acceleration structure today.
- **A rigid BLAS keys on `graphics::Mesh`, not on scene-node identity.** The
  bulk of the scene is rigid, mesh lifetime belongs to the resource layer, and
  `VulkanResources::_meshes` is already that cache's shape. So the first
  acceleration-structure work needs neither the ids from step 4 nor the
  versioning below.

That is why identity came before tracing in earlier drafts and should not
have: it is a prerequisite for *deforming* geometry only.

5. **Device groundwork.** `VK_KHR_acceleration_structure`,
   `VK_KHR_deferred_host_operations`, `VK_KHR_ray_query`, plus `features12`
   for buffer device address and descriptor indexing, and the VMA allocator
   created with the buffer-device-address flag. Self-contained and
   independently verifiable: the device either creates with the validation
   layers silent or it does not.
6. **Geometry addressable by the builder.** Mesh buffers need shader device
   address and acceleration-structure-build-input usage. Suballocating every
   mesh into one buffer (`vulkan-rt-backend.md` §9) is the eventual shape but
   is not required to build a BLAS from per-mesh buffers, so defer it.
7. **Rigid BLAS**, keyed on `Mesh` and built once, `PREFER_FAST_TRACE` with
   compaction where `Material::staticObject` says so.
8. **TLAS per frame from the snapshot.** The correctness rule is **every
   eligible snapshot object goes in** - no relevance test, no frustum test, and
   explicitly not routed through `drawScene`'s culling, which would reintroduce
   the camera-frustum policy this whole plan exists to escape. This is what
   finally calls `VisibilityPolicy::noCulling()`. Build it the expensive way
   first; a reflection missing geometry is not a performance result you can
   interpret.
9. **A ray-query shader that proves it** - a traced shadow or ambient-occlusion
   term, composited over the raster frame. First visible evidence any of this
   works, and what steps 5-8 should be judged against.

#### Then deforming geometry

10. **Backend caches keyed by id *and* content version.** Age-out handles
    disappearance - an id absent for N frames releases what was derived from
    it - and nothing else. A live object whose mesh, material, texture or
    deformation source changes keeps its id, so age-out never fires and a
    per-node BLAS would reuse derived data that no longer matches. Not
    hypothetical: `Creature` swaps main texture and environment map on live
    nodes (`src/libs/game/object/creature.cpp:1239-1248`), and
    `invalidateTexture` exists precisely for content changing under an
    unchanged identity (`src/libs/movie/movie.cpp:104`). Generalises the
    `VulkanResources::_meshes` pattern
    (`include/reone/graphics/vulkan/resources.h:149`); the difference from a
    retained protocol is that the snapshot *reports* the version rather than
    the scene *announcing* a change. **Lands with 11**, which is its only
    consumer.
11. **A deformation compute pass and per-node BLAS**, with two kernels sharing
    one mechanism: skinning, and dangly. **Dangly first** - there are 653 of
    them against 61 skinned, and they are 544 of the 586 transparent draws, so
    the cost is there rather than where the name suggests. Both kernels write
    deformed vertices into a buffer a BLAS builds from, and both need per-node
    state that survives the frame, which is where step 4's ids stop being an
    investment and start being load bearing. Note this is the first step whose
    output cannot be verified by bit-identical frames; see "foliage, not
    skinning" for why and for what to replace the check with.
12. **GPU material buffer**, indexed by TLAS instance custom index. Needs 1 and
    8; completes what a hit shader reads.
13. **The rest of the visibility work.** Step 3 already built the seam, so what
    remains is the policy it could not justify on its own: **TLAS relevance**,
    plus dropping `radius` and the cluster pool from the scene node.

#### Admission gates on step 8

Two items are parallel work rather than sequenced steps, but they are not
unconstrained: each gates whether its own geometry may enter the TLAS. Either
resolve it, or admit the geometry knowingly excluded and say so in the code -
what is not acceptable is admitting it without having decided.

- **Grass placement determinism** gates grass. Non-deterministic placement means
  a reflection shows a hillside populated differently from the direct view,
  which is a correctness bug rather than a nicety, and one that looks like a
  tracing bug when it is not.
- **The camera-facing particle decision** - proxy geometry in the AS versus
  keeping particles rasterised and compositing - gates emitters. Until it is
  answered, emitters have no defined representation to instance.

#### Genuinely unconstrained

- **The per-pass instance re-copy** in `drawScene` - `visible`, then `batch`.
  It is in the draw walk, not the register walk, and it scales with the grass
  plan above.
- **`SceneGraph::_nodes` never releases anything.** A `shared_ptr` to every node
  ever created, never erased, never read, untouched by `clear()`
  (`include/reone/scene/graph.h:305,396`). A module transition leaks the
  previous module's entire node graph. It is a bug on its own terms, and under
  the snapshot architecture it stays one - but until it is fixed, no id
  generation can ever advance, so anything relying on stale-id detection is
  relying on a field that never changes.

### Material identification

Branches after step 4 and gates nothing above. What it produces is a traced
frame that looks right and can be debugged, not one that renders at all, so it
can proceed alongside steps 5-9.

- **a. Interned model and node names beside the object id.** The string table on
  the scene graph, `uint32_t` name ids. Everything below needs this and nothing
  above does.
- **b. The authored name-to-material map**, plus its loader. Data under version
  control, most specific key wins. This is the source of truth.
- **c. An editor mode over the registry** to maintain it - the list of what the
  scene contains, an assignment per entry, a save, and the inverse view of
  which registered objects have no authored entry.
- **d. The heuristic tier** for the unmapped tail, tagged as inferred so the
  editor can show which tier decided a surface.

The order within this track is load-bearing, not incidental: the heuristic
comes last because it is the fallback, and it needs the authored map to already
exist as the thing that overrides it. Shipping the guess first makes every
later authored entry a correction rather than a decision.

`slang/pbr_resolve.slang:8-10` fills `SurfaceParams` from a G-buffer and notes
that a path tracer fills the same struct from a hit record, everything
downstream shared. That is the seam both tracks hang off; keep it.

## Open questions

- What happens to camera-facing particles - a proxy in the AS, or keep them
  rasterised and composite over the traced image? This is an admission gate on
  step 8, not a free-floating question: emitters have no defined representation
  to instance until it is answered.
- Granularity for emitters: one registered object per emitter, or per particle
  system when an emitter has several?
- Do walkmeshes and AABB debug geometry register at all, or stay immediate-mode
  alongside the 2D and GUI layers, which must keep explicit draw order? They
  register today, gated on `_renderAABB`/`_renderWalkmeshes`/`_renderTriggers`
  (`src/libs/scene/graph.cpp:585-605`), which works but was not a decision.
- Does the render-target viewer's need for backend-neutral target handles fold
  into this, or stay separate? It is currently the reason the Vulkan editor has
  no preview pane.
