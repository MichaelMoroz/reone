# Phase F — the work in progress

Split out of `cleanup-plan.md` on 2026-08-02. That file is the record of how the
engine got here; this one is what to do next, and nothing else.

**Steps are sized to an agent's context, not to a human's sense of a coherent
change.** A step that does not finish before the agent compacts is a step that
gets abandoned - twice on 2026-08-02, at ninety minutes each. So the rule is:

- **Split by inertness, then prove each step anyway.** Every step but the last
  adds capability nothing consumes, which makes it safe - but "the hash did not
  move" proves only that nothing broke, never that the new thing works. So each
  step also carries a *positive* check that exercises what it added: a readback,
  a disassembly, a unit test. Three inert steps verified only by absence, and
  the switch fails with no way to tell which one lied.
- **Turn the switch on for one object before all of them.** Same plumbing, and a
  wrong result is one object-shaped difference instead of a scene.
- **One page, no cross-references.** The step text is the brief. If a brief has
  to say "read the plan", the agent burns context re-deriving what is written
  here and gets it wrong after compacting.
- **A bar the agent can check itself** - a hash, a grep, a target that must
  exist - so it knows when to stop without a human in the loop.
- **Independently committable**, so a failed step is discarded rather than left
  in a twenty-two-file stash nobody reopens.

Standing rules for every capture: `--dev 0` and `--grassdensity 1`, because
`reone.cfg` is graded away from defaults and wins any flag not passed. Traced
output is nondeterministic - compare distributions, never a stored figure.

---

### F is two tracks, not a chain

Split 2026-08-02, because treating it as sequential put the largest and riskiest
piece in front of work that was nearly finished. Phase F's own description names
two things — "raster consumes `GpuScene`" *and* "becomes primary visibility for
both renderers" — and they are independent:

- **F-vis** needs traced mode to stop early-returning past the G-buffer
  (`scenepipeline.cpp:191-207`). It does not need merged geometry.
- **F-geo** needs merged geometry reachable from the render pass. It does not
  need hybrid.

**They never converge.** A first version of this split claimed they met at F5,
where the mega-draw supposedly wanted both. That was invented and this document
already said otherwise: *"the phase does not hinge on this substep. Under hybrid,
raster is primary visibility whether or not one draw beats many; F5 only decides
how those draws are issued."* F-geo runs to completion alone.

**Do F-geo first**, on verification grounds rather than size — F6 is phase-sized
too, so neither order puts a smaller thing first. F2 through F5 hold the
byte-identical bar the whole way; F6 gives it up by definition. Spending the
mechanically-checkable work first, while the strongest instrument still applies,
beats trading it away to start sooner on work that needs judgement anyway.

The counter-argument, recorded because it is not weightless: the sky is ~80%
built — baker, manifest, per-game configs, 79 baked assets, all committed — and
renders nothing until F7. Committed-but-unproven work is its own kind of debt.

## F-vis — raster becomes primary visibility, and the sky composites once

### F6–F10 — one sky, one composite

Folded into F in 2026-08-02 rather than made its own phase, because it is not a
feature and not a successor: the sky can only composite once *because* F makes
raster own primary visibility in every mode. Treating it as separable is what
produced the mess recorded here.

**Reviewed 2026-08-02 and corrected. Four things this section originally
asserted were false, and they are recorded because each was written with
confidence:**

- **Hybrid does not exist yet.** `PathTracing` bypasses raster entirely -
  `VulkanScenePipeline::init` returns before allocating `_gbuffer`
  (`graphics/vulkan/scenepipeline.cpp:191-207`), and the source comment says so.
  There are **three** modes, not four: `Retro`, `PBR`, `PathTracing`
  (`scene/render/pipeline.h:58-63`); `RTDebug` is still planned. So "the
  G-buffer exists in all four" was wrong twice, and "modes only meet at AA" is
  wrong too - traced never reaches AA today. **F6 is what makes the rest true**,
  which is the real reason it comes first; the original argument about where a
  composite can live was a weaker version of this.
- **The composite cannot go before `filterChainPass`.** That point is *after*
  Transparency, OITBlend and PostProcessing (`scene/render/pipeline/vulkan.cpp:152-167`),
  so a sky pass there paints over transparent particles and lens flares. It goes
  straight after the opaque resolve:
  `Resolve/RetroGeometry -> SkyComposite -> Transparency -> OITBlend -> PostProcessing -> filters`.
- **`sGBufDepth` is device depth in `[0,1]`, not linear view-space distance.**
  `pbr_resolve.slang:62-78` states it and reconstructs position from it. An
  earlier note here said the opposite and told the reader never to test depth;
  that came from misreading a `--dumptargets` dump, which linearises. `depth == 1.0`
  is a sound "no opaque geometry" test on the raster attachment. Retro is the
  real exception - it renders forward into `_output` and has no deferred colour
  G-buffer (`scenepipeline.cpp:676-724`), so it needs its own coverage signal.
- **The config does not name meshes yet.** `override/*/modules.ini` carries only
  `room =` and `sky =`. F9 therefore begins by adding the manifest, not by
  reading one.

The classification half is done and committed — `skybake` renders sky shells
offline into cubemaps (`8fa4e55b`, `6010a309`) and the per-module configs are
curated data (`d219bd6b`). Backlog 1.14 carries the evidence for why runtime
classification was abandoned. What remains is the renderer half, and it lands
after F5 because it depends on the same unification:

- **F6 — hybridise, then merge the graph.** This is the phase-sized one, and the
  review is blunt that a full graph rewrite is not what is needed:
  `VulkanScenePipeline` is already a 1,883-line frame executor with a
  `VulkanSceneFramePlan`, and PBR and Retro already *select* steps from it
  (`scene/render/pipeline/vulkan.cpp:138-170`). What is missing is that
  PathTracing early-returns out of the whole shape. The honest change is to
  remove that exclusive return, have raster produce visibility and depth in
  traced mode too, and make the tracer emit a transport image the plan
  composites — which touches `scenepipeline.{h,cpp}` initialisation, target
  ownership, barriers and dumping; `vulkan.cpp` plan construction; the tracer's
  output contract; and sky asset loading and descriptors, which do not exist
  yet. **Do not schedule this as a small prerequisite to the sky.**

  Three steps, each proving its own work rather than only its harmlessness:

  | step | change | what proves it works |
  |---|---|---|
  | **F6a** | allocate the G-buffer in traced mode; nothing writes it | `--dumptargets` in PathTracing now lists `g_buffer_*` at the right dimensions and format, where before there were none. Traced image unchanged |
  | **F6b** | run the raster geometry pass in traced mode, write the G-buffer, discard it | `g_buffer_depth.npy` from PathTracing is **byte-identical to PBR's** at the same camera. That is the whole proof that raster visibility is correct in traced mode, and it is available before anything depends on it. Traced image still unchanged; only frame cost moves |
  | **F6c** | the tracer takes its primary hit from the G-buffer instead of tracing camera rays | traced output changes by design. Compare distributions across three runs a side, and judge the images. This is the step that spends the byte-identical bar |

  F6b is where the value is. It makes the correctness of hybrid visible while
  the old path is still running, so F6c is a switch rather than a leap.
- **F7 — composite the sky once**, in a single pass placed **after the opaque
  resolve and before Transparency** — not before `filterChainPass`, which is
  past OITBlend and would paint over particles and flares. An integration
  attempt put it in `pbr_resolve.slang` *and* `postprocess.slang` — two
  implementations of one idea, precisely the fault that got the runtime bake
  deleted. Raster tests `depth == 1.0` on the device-depth attachment; retro,
  having no deferred colour G-buffer, tests its own coverage.
  *Acceptance:* a fixture with one opaque prop, one OIT particle and a lens
  flare; sky-off vs sky-on with filters disabled; the changed-pixel set must be
  exactly the far-depth set, and the particle and flare pixels must not move.
- **F8 — the tracer keeps only transport.** `ptSkyRadiance` on bounce miss stays
  (`slang/rayquery.slang:341-349`); its primary-miss call is compositing and
  moves to F7. Note the output-contract change this implies: primary-miss sky
  currently feeds `outputs.noiseFree` (`rayquery.slang:147-157`) and supplies the
  cyan sky-miss colour for the surface debug view, so F8 must say what replaces
  both. *Acceptance:* a fixed-seed fixture whose camera sees an opaque surface
  and whose first secondary ray misses; hash the traced result across the change.
- **F9 — suppress exactly the shell.** Begins by *adding* a per-mesh manifest to
  `override/*/modules.ini`, which today carries only `room =` and `sky =`, plus
  the resolver that reads it; the runtime currently uses a heuristic room
  classifier instead (`scene/render/pipeline/rayquery.cpp:318-368`). Then the
  baker and the renderer read one list rather than each evaluating a rule and
  hoping they agree. Neither the K1 no-walkmesh convention nor the TSL per-mesh
  flag identifies the shell alone: `001ebo16` flags all thirteen of its meshes,
  and that set contains the star shell *and* the asteroids *and* the planet.
  Suppressing by flag deletes a planet and looks like success.
  *Acceptance:* unit-test the resolver against `001ebo16` — the resolved shell
  set must equal the manifest, and the asteroid and planet meshes must be absent
  from it.
- **F10 — delete the runtime bake**, `slang/sky.slang`, and the shadow-ray
  candidate rejection at `slang/tracing/trace.slang:134`, which exists only to
  cope with sky geometry that may still be present. Larger than one line: the
  same removal touches the sky feature bit and the classifier that feeds it.
  *Acceptance:* `rg -n 'bakeSkyRoom|clearSkyRoom|RayQuerySkyRoom|skyAvailable' src include slang`
  returns no runtime-bake remnants, and `slang/sky.slang` does not exist.

The trap worth naming, because it is what makes F10 safe to be strict about: a
sky that renders correctly while quietly removing scenery still passes every
sky-shaped test. So the bar is the props, not the sky — capture `001ebo` and
`manm26ad` and confirm the asteroids, the planet and the Ahto City rings are
still drawn.

That bar is **necessary but not sufficient**, and the review is right about why:
hashing `sky = none` modules only proves the untouched branch stayed untouched.
It cannot catch a sky composited in the wrong order, a shell resolved to the
wrong mesh set, or traced output moved by F8 — which is why each step above
carries its own mechanical check instead of deferring to the phase bar.

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

## F-geo — raster consumes `GpuScene`

F0 through F5. Independent of F-vis until F5, where the mega-draw wants both.
F0, F1 and F2 are done; F3 is next and is larger than it reads — see the blocker
recorded under it.

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

**Nor background geometry.** Found 2026-08-02: covering it breaks the hash, and
excluding it restores `A654109C…` exactly on `danm14ab`. Same class as grass and
particles — the sky room is drawn by a path of its own — and it disappears as a
question at F10, when the sky stops being geometry at all. Until then it is a
third exclusion, not a subtlety.

**The blocker to plan around: `VulkanRenderPass` cannot see `GpuScene`.** It
takes a `Mesh &` and draws it, so nothing raster-side can ask where a mesh lives
in the merged buffer. F3 is therefore not a shader change with some plumbing
attached — the plumbing *is* the step, crossing descriptors, the pipeline key,
the pass and the pipeline plan. An attempt on 2026-08-02 touched 22 files and is
in a stash for reference.

#### F3 is five steps, and each one proves its own work

Splitting it so the early steps are inert — nothing consumes them — makes them
safe, and safe is not the same as verified. A step whose only evidence is "the
hash did not move" has proved that it broke nothing and **nothing whatsoever
about whether what it added works**. Three green steps like that and the switch
fails with no way to tell which one lied. So every step carries a *positive*
check that exercises the new capability directly:

| step | change | what proves it works |
|---|---|---|
| **F3a** | `GpuScene` exposes each mesh's merged vertex and index offsets | a test that reads the merged buffer at the reported offset for N registered meshes and asserts the positions equal the source mesh transformed to world space, and the index count matches `faces().size() * 3`. Wrong offsets fail loudly instead of silently pointing at a neighbour |
| **F3b** | descriptor set binds the merged buffer to the vertex stage | `--vkvalidation 1` clean, **plus** a readback through that binding asserting the same vertices F3a checked. A misbound descriptor reads zeros or another buffer, and both pass a validation-only check |
| **F3c** | `mergedVertex` entry pulling by `SV_VertexID` | `spirv-dis` shows the entry point and a storage-buffer load from the merged binding. Cheap, and it catches the trap that has cost three investigations here: a shader edit that never made it into the compiled module |
| **F3d** | route **one** mesh through it | raster byte-identical. This is the first step whose success means the whole chain works, and its failure localises to one mesh rather than a scene |
| **F3e** | route the whole static opaque set | raster byte-identical on all six references |

F3d is the important one. Turning the switch on for a single mesh costs the same
plumbing as turning it on for everything and makes a wrong vertex a
one-object-shaped difference in the image instead of a scene-wide one.

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
