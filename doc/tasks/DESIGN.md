# Design extract — the how and why of the open work

Rescued from `doc/phase-f.md` (the raster track, the path-tracing substage, the
V steps, the reference-engine survey) and `doc/renderer-redesign-plan.md` (the
structural track, S0–S6) before both are deleted. What follows is the design
content for work that is **still open**: the mechanism of each unbuilt step, its
acceptance criteria, the decisions already taken with the reasoning that settled
them, and the rules that outlive any single step. Per-step status tables and
commit hashes used as status markers are dropped; hashes survive only where they
are evidence for a measurement or for a fix.

**Audited against the tree on 2026-08-09, and that reintroduced hashes as status
markers.** Enough of what this document plans has since been built that reading
a future-tense passage as still-future is now the main way to be misled by it. So
landed work carries a **short status annotation naming its commit**, placed at
the head of the step or beside the specific claim, and **nothing is deleted** —
the reasoning behind a decision keeps its value after the decision is
implemented, and a plan that was overtaken is evidence about how this project
mis-predicts itself. Where a premise stopped holding rather than being satisfied,
the annotation says so in those words. Line numbers are re-checked; where only a
line moved, the old cite is kept beside the new one so an older brief still
resolves.

---

# Part 1 — Standing rules

## The goal, and why it is split by mode

**Make the engine better while preserving the retro look.** Those pull in
opposite directions often enough that the rule needs stating: the two aims are
separated by *mode*, not traded off inside one.

- **Retro is preservation.** It reproduces what the original did. A deviation is
  a bug, not a taste question, and the reference engines settle it. Retro is also
  the fidelity anchor every other mode is judged against, which is why it goes
  first in each step and why it must stay honest.
- **PBR and path tracing are the improvement.** They are held to looking right,
  not to matching the original, and 2003's constraints are not binding on them.

**The intent-versus-limitation test.** The useful consequence when reading a
reference finding: ask whether it records **artistic intent** or **a technical
limitation**. The env-map formula, the sphere-map projection, the alpha blend
modes and submission order are intent — they *are* the look, and they bind retro
absolutely. The eight-light budget, skeleton-derived shadows, absent
anti-aliasing and unfiltered textures are limitations — retro may keep them, the
other modes should exceed them.

One case shows the distinction is not always obvious: KOTOR's 2D env maps are GL
**sphere maps**, so the eye-space projection is not a stylistic choice at all —
it is how the texels were authored, and sampling them any other way reads the
wrong pixels *in every mode*. A cube map has no such constraint, so there
world-space reflection is available to the improving modes.

The structural track does not touch this contract: retro stays the fidelity
anchor, PBR and path tracing stay the improvement, and every structural step is a
pure refactor by construction, held to the instruments that make that checkable —
upload-hash equality across modes, the shadow-oracle tripwire, byte-identical
raster captures, traced output judged by distribution.

## How steps are written

A step that does not finish before an agent compacts gets abandoned — that
happened three times on 2026-08-02, at ninety minutes each. So steps are sized to
a context, not to a coherent-looking change, and each one:

- **is briefable in a page, with no cross-references.** The step text is the
  brief. A brief that says "read the plan" spends the agent's context re-deriving
  what is already written, and derives it wrongly after compacting.
- **proves its own work, positively.** "The hash did not move" shows nothing
  broke; it shows nothing about whether the new thing works.
- **turns on for one object before all of them** where that is possible. Same
  plumbing either way, and a wrong result is one object-shaped difference instead
  of a whole scene.
- **is committable alone**, so a failure is discarded rather than left in a
  twenty-two-file stash nobody reopens.

## Look before you measure

Before building a metric: **render the thing and look at it.** Every metric built
on 2026-08-02 either missed the real defect or misled — the sky continuity check
said nothing about buildings baked into the horizon, and the "holes" triage
ranked a correct starfield worst. Metrics check the rest of the set once you know
what you are looking at.

The instrument for looking is the free camera, and it no longer costs a trip to
the console: it sits on the editor menu, restores the previous camera on the way
out, and looks on a held right button so the menu that turns it on stays
clickable. That same work retracted a measurement which appeared to show the free
camera never reaching the renderer at all — the captures had frozen the frame
before the module finished loading, so they compared two loading screens. **A
capture taken too early proves nothing about the thing it names.**

## Capture rules, everywhere

- `--dev 0`, or the frame-time readout forges a difference.
- `--grassdensity 1`, because `reone.cfg` is graded away from defaults and wins
  any flag not passed.
- **No jitter on either side of a cross-mode comparison**, or the comparison
  measures sampling noise. Raster jitters through the projection matrix and the
  tracer jitters the ray, so with jitter on every alpha-cutout edge decides
  independently and the comparison is dominated by sampling noise — it once read
  2.94% where the truth was 0.67%, and the mistake survived long enough to send a
  step chasing a residual that was not there.
  **The flag this rule was written against is gone.** `9accfeddd` deleted
  `--taajitter`, `JitterMode` and the editor combo: jitter is no longer a dial
  beside the rule but a consequence of it, and `SceneGraph::computeJitter`
  (`src/libs/scene/graph.cpp:662-673`) returns zero unless the active
  anti-aliasing slot is FSR. **So the rule is now spelled `--antialiasing
  fxaa`** (or `off`) on both sides. It remains a rule, and it now has teeth it
  did not have before: the flag's *default is mode-dependent* — FSR in path
  tracing, FXAA everywhere else (`src/apps/engine/optionsparser.cpp:330-333`) —
  so a cross-mode capture that passes nothing jitters the traced side and not the
  raster side, which is exactly the comparison this rule exists to forbid.
- Traced output is nondeterministic — compare distributions, never a stored
  number.

**Captures are deterministic per binary, not across binaries.** The capture frame
counts from process start, so anything that shifts load timing — adding one
shader module was enough — shifts animation phase at a fixed frame: 92k pixels
moved on `danm14ab` because the plaza droid stood differently, and one material
dedup split on `bumpMapFrame`, with admission untouched. Cross-commit dump
comparisons must expect animation-phase drift or mask animated actors. The upload
hash is stable within a binary and across modes — **that equality is the
invariant; its absolute value across commits is not.**

## Where material data lives — the rule, settled in G6

G6 first carried per-object ambient and diffuse in two new RGBA8 attachments, 8
bytes per pixel to copy fields that already existed in the record, and a shape
where every future material field a resolve wanted cost another attachment. That
was replaced by a 16-bit **material id** in the G-buffer and a lookup in the
`instanceMaterials` buffer the resolves can already reach through the mega-draw's
own set 2. Material transport fell from 8 bytes per pixel to 2 and the whole
G-buffer from 32 to 26; `0xFFFF` is the uncovered sentinel, and more than 65,535
records warns and drops the frame rather than wrapping, which matters because
dedup keeps real counts in the low hundreds. The rule that generalises it, and
the reason it survives real PBR textures:

**Superseded in mechanism, not in rule — `4980518c`.** The material-id
attachment is gone; the G-buffer now carries a 32-bit **triangle id** instead
(`GBufferAttachment::TriangleId`, `Format::R32Uint`, at
`include/reone/graphics/rendering/gbuffer.h:34-44` and
`src/libs/graphics/rendering/gbuffer.cpp:86-89`). The triangle is the strictly
stronger key: `mergedMaterialIds[triangleId]` recovers the material, and the
triangle's own vertices recover the *geometric* normal exactly, which no stored
8-bit normal can — which is what let the tracer take its primary from the
G-buffer at all. Consequences for everything below: the sentinel is
`kNoTriangle = 0xffffffffu` (`gbuffer.h:66`), not `0xFFFF`; the 65,535-record
ceiling and its drop-the-frame guard are retired; and the six attachments cost
24 bytes per pixel, not 26. **Read "behind the material id" below as "behind the
triangle id" — the per-pixel / per-object split it decides is unchanged.**

| data | where it goes | why |
|---|---|---|
| **per-pixel**: roughness, metalness | a G-buffer channel, sampled in the mega-draw | it varies per texel; no id can carry it |
| **per-object**: ambient and diffuse tints, water alpha, env cube ids, derived env layer, curated overrides | behind the material id | constant across the object, so copying it per pixel is pure waste |
| **ambient occlusion** | **nowhere** | the lightmap *is* the baked occlusion for static geometry and the tracer traces the real thing — a third answer to a question two systems already answer |

**Textured PBR therefore costs no new attachments.** Roughness and metalness need
two bytes, and two are already free or about to be: `eyeNormal.a` is written as
literal zero today, and `selfIllum.a` carries `envMapDerivedLayer`, which is
per-object and moves behind the id. Packing roughness beside the normal is also
what the tracer already does — its guide channel is literally
`traced_normal_roughness`, NRD's own format — so the two consumers' layouts
converge rather than drift.

Two things to carry into that work when it happens: both consumers currently
**derive** roughness from diffuse alpha (`material.slang`'s `clamp(alpha, 0.2,
1.0)` and its raster counterpart), so real textures must be adopted in **one step
for both** or the shared-material rule breaks; and the curated per-category
roughness and metalness overrides multiply into those derived values today, so
they need re-expressing against textured inputs rather than against a derivation.

**Both are now easier than when written — `735fccd69`.** The derivation moved to
`slang/lib/material_ops.slang`, so "one step for both" is one function:
`resolveMaterial` (`material_ops.slang:76`) holds the base derivation
(`:81-82`, with the 0.2 floor promoted to a push constant), the curated
per-object ops, the category override, and the roughness scale, in that order.
The PBR resolve calls it (`slang/pbr_resolve.slang:331`). **The tracer does
not** — `slang/tracing/material.slang:268-291` and
`slang/tracing/primary.slang:174-196` still inline the same chain and borrow
only `curatedChannel` from the shared module, and `slang/debug_view.slang:160`
carries a third private copy. So the single point of change exists but two
consumers have not been moved onto it; that is the residue to close before
textured inputs are adopted.

## Two open claims left deliberately unclosed

- **Bump has never been held to a fixture.** It rides in the material record and
  `scene_draw.slang` samples it, but nothing has checked it against authored
  content. Envmap was checked by the metal work; bump is still an unchecked claim
  and should be given a fixture rather than assumed. *Still open, 2026-08-09:*
  `slang/scene_draw.slang:302-308`, `bumpMapArray`/`bumpMapFrame`/`bumpMapScale`
  at `slang/lib/scene_schema.slang:38-41`, no fixture anywhere.
- **The authored mirror is pinned to explicit LOD 0**, which filters nothing at
  distance, so minified metal may alias. LOD 0 is there because the first attempt
  at the metal fix moved the droid by only 0.0004: implicit LOD inside a
  fullscreen resolve derives its derivatives from 8-bit G-buffer normals and slid
  the mip straight back to blurred. Choosing a LOD from surface footprint rather
  than pinning it is a separate policy, noted where the sampling happens.
  *Still open, 2026-08-09:* `slang/pbr_resolve.slang:248-250`. The reason it did
  not resolve itself when the resolve became compute (`fedcb7445`) is worth
  knowing — a compute shader has no implicit derivatives at all, so implicit LOD
  is not merely inadvisable there, it emits
  `SPV_KHR_compute_shader_derivatives`, a feature the engine does not request.
  **A footprint-derived LOD is now the only available answer, not the nicer one.**

## Ordering constraints that survive everything

- **The geometry track measures against the traced G-buffer, and unification
  deletes the traced primary visibility that produces it** — finish with the
  instrument before removing it. **Overtaken by events, `4980518c`, and the
  outcome is worse than the constraint anticipated.** Unification landed and the
  instrument was *not* deleted: `traced_diffuse`, `traced_eye_normal`,
  `traced_depth` and `traced_motion` are still emitted
  (`src/libs/graphics/rendering/tracingpipeline.cpp:243-248`, written at
  `slang/tracing/outputs.slang:185-190`). But they are now filled from
  `ptReadPrimary`'s raster-derived surface (`slang/path_trace.slang:254-259`),
  so **they no longer measure anything raster does not already say.** The
  instrument survives its own deprecation notice while having quietly stopped
  being an independent check — which is the more dangerous state of the two,
  because it still produces plausible numbers. Anything still leaning on it needs
  a different oracle.
- **The sky is black in retro and PBR through G9, and that is a decision, not an
  oversight.** The shell is suppressed unconditionally in every mode, and the sky
  chain (V0 fix the baker, V4 suppress from its manifest, V2 composite, V5 delete
  the runtime bake) runs after G9. So "the raster track is finished" means
  finished apart from the sky, and every by-eye acceptance from G6 to G9 is
  judged against a black-sky frame.
  **No longer true — `fedcb7445` ran V2 ahead of G9.** Both raster resolves shade
  the sky themselves at the no-material sentinel
  (`slang/retro_resolve.slang:129,140`; `slang/pbr_resolve.slang:305,314`)
  through the shared `skyRadiance` (`slang/lib/skycube.slang:41`). The rest of
  the chain is untouched — V0, V4 and V5 are all still open, and the cube is
  still fed by the *runtime* bake — so what changed is the acceptance baseline,
  not the chain: **captures from `fedcb7445` onward are judged against a
  sky-bearing frame, and comparing one against a pre-`fedcb7445` capture compares
  two different decisions.**
- **The fog grid is not on the raster track.** It was once specced beside G8; the
  march is path-tracing only, so the grid, the id grid and every marcher decision
  sit in the path-tracing substage.
- **RTDebug's cross-consumer scene validation must be pinned before V1c deletes
  the traced primary path** — it is the only scene validation that does not
  depend on judging an image, and the structural steps lean on it as hard as the
  raster track does. **Premise gone: RTDebug was never built and will not be.**
  V1c has landed without it. What exists instead is a debug *view* channel
  orthogonal to render mode — `slang/debug_view.slang`, twenty channels
  (`kMaxDebugView = 19`, `include/reone/graphics/options.h:63`), selected by
  `--debugview` and dispatched by `ScenePipeline::debugViewPass`
  (`src/libs/graphics/rendering/scenepipeline.cpp:951-997`). It answers in all
  three modes because every mode now draws the same G-buffer
  (`src/apps/engine/editor.cpp:1538-1541`), which is more than a fourth mode
  would have given — **but it is a viewer, not a validator.** The
  image-independent cross-consumer check this constraint was protecting still
  does not exist and now has no owner.

---

# Part 2 — The open raster steps

## G6c — PBR is the traced shading model, evaluated deferred

**Status 2026-08-09: the material half landed in `735fccd69`; the lighting half
below is still unbuilt.** Read the diagnosis that follows as the record of why —
it was accurate when written, and the commit is its answer. Where it says the
resolve applies none of the chain, it now applies all of it.

**Decided 2026-08-04.** PBR mode is not a second shading model that happens to
resemble the tracer's. It is the *same* model with the transport removed —
deferred raster instead of path tracing, and nothing else different. Anything the
tracer derives about a surface, the deferred resolve derives identically.

It does not today, and **the gap is entirely in evaluation rather than in
transport.** `pbr_resolve.slang:147` already reads the same `InstanceMaterial`
out of the same `instanceMaterials` buffer, by the material id in the G-buffer,
so every field the tracer uses is already in the resolve's hand. Both start from
the same stand-ins — `metallic = 0`, `roughness = clamp(alpha, 0.2, 1.0)` — and
`material.slang:218` says in as many words that they are kept aligned. Then
`material.slang:225-248` applies a chain the resolve applies none of:
`curatedAlbedoMul`, the curated roughness and metalness channels, the
per-category metallic scale in `overrideParams.w`, the albedo tint in
`overrideColor`, the roughness override in `overrideParams.x`, `roughnessScale`,
and the env strength in `overrideParams.z`.

So **every curated material decision is invisible in PBR.** The droid calibration
that the metal fix settled applies in the traced mode only, and the raster mode
it was judged against cannot express it. It also explains why the two raster
modes read so alike: with metalness pinned at zero and roughness reduced to one
authored channel that means mirror strength rather than roughness, PBR has almost
no material variation left to differ by. Measured at one camera per module, retro
against PBR differs on 6.1% of pixels in `tar_m03aa`, 8.7% in `korr_m33ab`,
15.1% in `danm14ab` and 57.2% in `unk_m41aa` — largest where lighting dominates
and smallest where lightmaps do, which is the signature of a difference that
comes from the lighting integral and not from materials.

**The shape:** lift `material.slang`'s derivation into `slang/lib` and have both
consumers call it, the same way `commitsTracedCoverage` became shared when the
shadow pass needed it. The derivation is pure arithmetic over the record — no
ray, no hit — so nothing about it is traced. Guard it with **the traced mode
staying byte-identical**, since the tracer must come out of the refactor
unchanged.

**Built that way — `735fccd69`.** The chain is `slang/lib/material_ops.slang`,
and the PBR resolve calls `resolveMaterial` at `slang/pbr_resolve.slang:331`.
Two things the shape did not predict, both recorded in the commit and worth
carrying:

- **The ordering inside the chain is load-bearing and was got wrong twice.**
  Curation runs against the albedo it derived, the category override then
  *replaces* that albedo, and the scale and floor close over whatever survived.
  A shared function is only one policy if the order inside it is the order both
  consumers had.
- **The refactor surfaced a bug the diagnosis had not seen.** The resolve's push
  constants were built as `{resolveFlags()}`, so every member after the first
  was value-initialised — `thinTransmission` was zero in PBR while retro passed
  the authored fraction, and thin surfaces transmitted no light at all. Grass and
  foliage are the whole point of that flag. All three call sites now fill every
  field through one function.

**The sharing is incomplete, and the gap is the tracer's side of it.** Only the
PBR resolve calls `resolveMaterial`; `slang/tracing/material.slang:268-291` and
`slang/tracing/primary.slang:174-196` still inline the chain and import only
`curatedChannel`, and `slang/debug_view.slang:160` holds a private third copy of
the same arithmetic. **That the three agree today is luck, not construction** —
which is precisely the failure mode this step existed to end, now reproduced one
consumer along.

**The lighting function goes with it, and the split is by mode — decided
2026-08-05.** Retro keeps Odyssey's lighting maths *including its bugs*: the
dimensionally-wrong range cull that compares a distance against `radius²`, and
the `radius²/(radius+d)²` falloff that is not inverse-square. Those are the look,
and they are recorded as faithful ports rather than accidents. PBR does not keep
them — it takes the corrected model the tracer already has, the sphere-light
solid angle `Ω = 2π(1 − cos θ)` with `θ = asin(saturate(R/d))` and the
`3/2 − 2ln2` scale, for the same reason it takes the tracer's material
derivation: PBR is the traced shading model with the transport removed.

That is a behaviour change and it is meant to be one. It also means the audit's
finding is **not** "PBR has a bug": `pbr_resolve.slang:83-85` and `:235` carry
the Odyssey cull and falloff deliberately, with a comment saying they are kept as
the GL shader had them. G6c is what retires them, in PBR only.
`retro_resolve.slang:116` keeps its copy, and **any future refactor that shares
lighting code between the two modes must keep both functions rather than
converging them.**

*Unbuilt as of 2026-08-09, and the line numbers have drifted.* PBR still carries
the Odyssey pair: `lightAttenuationQuadratic` at `slang/pbr_resolve.slang:239-242`
and the `radius²` cull at `:442-448`, still with the comment saying they are the
GL shader's. Retro's copies are `slang/retro_resolve.slang:96-97` and `:193`.
The corrected model PBR is to take is `slang/tracing/lighting.slang` —
`kPtLightBudget` (the `3/2 − 2ln2` scale) at `:41`, `ptSphereAttenuation`'s
`2π(1 − cosθ)` at `:59-71`, and `asin(saturate(R/d))` at `:77`. Nothing is shared
between the two files, so the split this paragraph asks for is still available at
its full price.

## G8 — the blended pass, and the three alpha kinds

**Status 2026-08-09: the draw is built; the sort is not, and was rejected rather
than deferred.** `ScenePipeline::blendedPass`
(`src/libs/graphics/rendering/scenepipeline.cpp:501-555`) draws the non-opaque
range at `BlendMode::Premultiplied` — ONE, ONE_MINUS_SRC_ALPHA
(`include/reone/graphics/types.h:125-128`) — depth-tested with depth-write off
(`:525-526`), through `sceneDrawBlendedFragment`
(`slang/scene_draw.slang:397`). The three kinds are a first-class enum,
`AdmissionKind { Opaque, Cutout, LitBlended, AdditiveEmissive }`
(`include/reone/scene/gpuscene.h:156-161`). It runs unfiltered in every mode
(`src/libs/scene/render/pipeline/renderpipeline.cpp:312`), including path
tracing — correctly so, because the march the scope note below defers additive
to does not exist yet. **When the march lands, that filter is the thing to
remember to add**; nothing in the code marks the omission today. Details per
subsection.

**Scope: retro and PBR.** In path-tracing mode additive sprites leave geometry
and are handled by the march (see the substage), so the blended draw there covers
lit-blended surfaces only — a filter on the range it draws, not a fork of the
draw. Briefed without this, the draw double-counts additive in traced mode.

The G-buffer holds opaque surfaces. Everything else is a second draw over the
same merged buffer, after shading. There are exactly three kinds of alpha and
they are not variations of one thing:

| kind | example | where it belongs |
|---|---|---|
| **alpha punchcards** | leaf cards, fences, grilles | **opaque, and already drawn — not part of this step** |
| **alpha emissive** | saber blades, glow decals | **transparent, additive** |
| **alpha lit + emissive** | particles, smoke | **transparent, alpha blended** |

**The last two are one draw, not two.** Output premultiplied colour and fix the
blend state at `ONE, ONE_MINUS_SRC_ALPHA`: additive is then simply alpha zero,
and alpha-blended is alpha equal to coverage. The material decides which it is by
the alpha it writes, so no second pipeline and no second pass are needed — **the
distinction stops being a branch in the frame graph and becomes a value.**

Punchcards are in that table to say what this step does *not* touch. They are
opaque geometry and they already work: the gated mega-draw publishes their
coverage into the G-buffer, both shadow passes test them the same way through
`commitsTracedCoverage` in `lib/megadraw_geometry.slang:47`, and the tracer's
`commitsCoverage` holds the same 0.5 threshold. Nothing about a blended pass
changes any of that. *Both cites renamed, same behaviour:* `megadraw_geometry`
folded into its one consumer in `8606171d5`, so `commitsTracedCoverage` is now
`slang/scene_draw.slang:93` against `kMegaAlphaTestThreshold` at `:38`; the
tracer's `commitsCoverage` split into `isCutout`/`isBlendedCoverage`
(`slang/tracing/trace.slang:90,95`) over `kAlphaTestThreshold` at `:88`. The two
0.5s still agree, and `scene_draw.slang:85-89` now says in the source that they
must.

**The threshold conflict is real, and it is retro's problem rather than a taste
question.** An earlier draft dismissed it as a wording slip: this document's own
phrase "discards on zero alpha" was read against the 0.5 the code has always used
and waved off. But the reference value is neither 0 nor 0.5 — it is **0.1**
(xoreos `graphics/graphics.cpp:440`), so 0.5 is five times the reference, and
"0.5 is right because it discards the soft authored fringe" is a rendering-taste
argument, not a preservation one. Retro is preservation, so this needs a capture
rather than a preference; `retro-rendering-differences.md` row 19 owns it.

**The other half of that dismissal stands**, and it is worth keeping because the
mistake is easy to repeat: reading the tracer's non-opaque BLAS range as a
classification that disagrees with "punchcards are opaque". It is not a
classification at all. Hardware traversal cannot run an alpha test, so any
surface with holes has to sit in a non-opaque range for the candidate loop to
test it (`rayquery.cpp:926-929` says exactly that; the file is now
`src/libs/graphics/rendering/rayquery.cpp`, and the two-geometry opaque /
non-opaque BLAS split it describes is at
`src/libs/graphics/vulkan/tracingstructure.cpp:160-161`). Being non-opaque *to
the BLAS* is how a ray tracer expresses opaque-with-holes, which is the same
thing raster expresses with `discard`.

### Sorting, which blending needs and one draw does not provide

**Decided against, 2026-08-09 — the sort was not built and the reference finding
at the end of this subsection is why.** The blended draw issues one
`drawIndexed` over the pre-partitioned non-opaque range in admission order
(`src/libs/graphics/rendering/scenepipeline.cpp:549`), and the source states the
reasoning where the draw happens (`scenepipeline.cpp:546`,
`slang/scene_draw.slang:394-396`): kvp-main's replay of the retail draw stream
reorders only true-opaque depth-writing batches, so a distance sort would be a
deviation before it was an improvement. No remap buffer exists. **What follows
is therefore the design of the road not taken** — kept because the escape hatch
in "Open questions" still points at it, and because the remap's two-read hazard
is a real trap for whoever revisits it.

Blended output is order-dependent and the scene graph does not sort — the comment
claiming distance-sorted transparent buckets sits above code that builds them in
node-iteration order, and the old path papered over it with OIT, which is boxed.
The tracer needs no order; raster does. So the ordering is a **raster-side
derived artifact, not part of the scene description** — the shared upload stays
byte-identical between modes and the equality check is untouched.

**The shape: CPU-sort only the blended set — lit and additive-emissive together,
since they interleave — and feed the draw a per-triangle remap buffer.** The set
is small (a few hundred particle quads, faded meshes, water), well inside CPU
budget. Order is decided entirely CPU-side today — `dstTriangleBase` is assigned
by walking the object vector — so nothing on the GPU has an opinion to fight.

**The remap feeds two reads, not one, and missing the second is the bug to warn
about:** the vertex stage pulls corner `k` of sorted slot `t` via
`indices[remap[t]*3+k]`, and the fragment stage must look up material by the
**original** triangle id, `materialIds[remap[base+prim]]`, because the
per-triangle material table is in merge order, not sorted order.

Sort keys come from the CPU side that already knows them: procedural quads carry
world positions in their records, mesh triangles get transform-applied centroids.
Skinned blended meshes would sort by their untransformed-bind approximation,
which is acceptable for a sort key and not worth CPU skinning.

Per-object sorting falls out for free in admission order; the remap only has to
exist where triangles of different objects interleave. Depth-test against the
opaque G-buffer, **depth-write off**, or blended fragments reject each other.

A reference finding lands directly on this step: **keep submission order for
non-opaque draws.** kvp-main's replay of the real game reorders *only* true-opaque
depth-writing geometry, so if the remap sort reorders transparents, expect
regressions the original did not have. **This is the finding that won.** It was
written as a caveat on the sort and turned out to be an argument against it: the
draw keeps submission order and there is nothing to caveat.

*Proves itself:* a fixture carrying all three alpha kinds at once — an
alpha-blended pane, an additive glow and a punchcard — renders each correctly in
retro and PBR against the pre-G1 captures; sort correctness is shown by a
deliberately interleaved pair, two transparent objects whose triangles alternate
in depth, which is wrong without the remap and right with it; and the upload hash
stays equal across modes, proving the sort is a raster-side artifact rather than
a change to the shared description.

## G9 — the shared output stage: bloom, lens flares, anti-aliasing

**Status 2026-08-09: the anti-aliasing third landed, and sharpening with it;
bloom and lens flares are untouched.** The stage exists as designed — one
selector, one occupant, every mode:

```
enum class AntiAliasing { None, Fxaa, Fsr };   // include/reone/graphics/options.h:36-47
```

The header states the shape in the same terms this step does: *"A slot rather
than a switch: every mode runs the same one… One slot means one occupant —
running a temporal resolve and then a spatial one over its output is two
anti-aliasers stacked, which is what this enum exists to make unrepresentable."*
Selection is `ScenePipeline::antiAliasingPass`
(`src/libs/graphics/rendering/scenepipeline.cpp:883,901-907`); path tracing goes
through it like the others (`:1066-1067`). **Which side of the display transform
the occupant sits on is decided by the occupant, not the mode** — FSR before,
FXAA after (`src/libs/scene/render/pipeline/renderpipeline.cpp:284-298,321-322`),
which is a resolution of the "where does sharpening sit" question the step left
open. Sharpen is a pass of its own, last of the frame, after the display
transform (`d943d5d1a`; `scenepipeline.cpp:1006-1020`), because an unsharp mask
should judge the picture a viewer sees. **Bloom does not exist** — no
implementation and no shader; **lens flares do not exist** — the category is
still filtered out at admission, exactly as this step predicted
(`src/libs/scene/render/admission.cpp:501-502,508`). Details below.

Raster has had no anti-aliasing since G1 boxed FXAA and sharpen with the rest of
the old post chain; FSR exists but is wired only into the traced path. G9 makes
the frame's tail a **shared output stage every mode ends in**, and it carries
three things, not one:

- **Bloom, for every mode including path tracing.** The old renderer wrote
  hilights to a second resolve target and blurred them (`hilightsBlurPass`); the
  blur shaders survive in `postprocess.slang`. It is a display effect, so it
  belongs to all three modes rather than to raster's resolve.
- **Lens flares, for every raster mode.** They were in the retro pipeline as well
  as PBR's, drawn as billboards in the old post walk. Their category `LensFlare`
  is still filtered out at admission, so restoring them starts there, not in the
  shader.
- **Anti-aliasing**, with two methods: **FSR** at NativeAA — the temporal
  resolve, which **requires jitter on**; and **FXAA** — spatial, single-frame,
  which **requires jitter off**, since a jittered frame with no temporal resolve
  just shimmers.

**So the AA choice *drives* the jitter setting rather than sitting beside it as
an independent dial.** **Done — `9accfeddd` took the second half.** The option,
its enum, the CLI flag and the editor combo are all deleted, and
`SceneGraph::computeJitter` (`src/libs/scene/graph.cpp:662-673`) reads the active
AA slot alone. The commit puts the argument in one line: *"a dial beside a rule
can only ask for the answer the rule already refused."* Measured through
G-buffer depth: 77% of it moves between a jittered frame and an unjittered one,
and 0% between a frame that started in FXAA and one switched to FXAA at runtime.
**That measurement closes half of the open question below** — the offset
demonstrably reaches rasterization now — but it was taken on depth, not on the
resolved image, so whether FSR *consumes* it in the output is still unmeasured.

`9ba51344` took the first half of that: `taajitter` was
still a global option, but `computeJitter` returned zero outside path tracing,
because jittering a grid nothing resolves is shimmer by construction. On a frozen
scene with a static camera it was moving 6–12% of pixels per frame in both raster
modes, and **the reason it read as *shadows* crawling is a matrix mismatch worth
recording here since G9 owns the reversal:** the offset only ever reached
`globals.projection`, while megadraw rasterises through `globals.viewProjection`,
which `uniforms.slang` documents as deliberately unjittered so motion vectors
stay clean. Depth was written through one matrix and inverted through another, so
every reconstructed world position wobbled by the Halton offset each frame, and
the shadow map lookup turned that into a binary flip along every shadow edge.

G9 is what opens the gate again, and **it should open it per method rather than
per mode.** One selector, deriving jitter, is the shape — and it is also what
makes the modes comparable, because a retro and a traced capture at the same
setting then differ in shading only. **Built exactly so:** `AntiAliasing` is the
one selector, jitter is derived from it, and it is per method rather than per
mode.

**Still open, and now the last unfinished part of G9's AA third: path tracing
shows no image difference at all between jitter on and off.** `9ba51344` also
corrected
megadraw to rasterise through the jittered projection so depth and its inverse
agree, but that correction is unexercised rather than verified, because no
measurement in traced mode moved a pixel either way. It was kept because it
matches the documented intent, not because a result forced it. Either FSR is not
consuming the offset or the offset is not reaching the sampling, and both are
defects in the one mode that has a temporal resolve today — so **G9 cannot treat
"FSR requires jitter on" as established until traced output is shown to respond
to jitter at all.** *Unchanged by `9accfeddd`:* that commit measured the offset
into G-buffer depth, not out of the resolve, so the gate opened on the strength
of the documented intent rather than on a result. The measurement this paragraph
asks for — traced output against traced output, jitter the only difference — has
still not been taken, and it is now harder to take, because the dial that used to
express it is gone. **Switching AA method to reach it also switches the
resolver**, so the difference a naive A/B captures is FSR-versus-FXAA, not
jitter.

The shaders survive from G1 (`postprocess.slang` still carries `fxaaFragment`,
now at `:184`), and the FSR path exists in `fsrupscaler.cpp`; the work is
plumbing them into one selectable stage and deciding where sharpening sits
relative to it. **Both done** — see the status note at the head of this step.

*Proves itself:* an edge-heavy fixture captured in all three modes under each
method, jitter derived rather than set, judged by eye against the pre-G1 retro
captures for FXAA and against the current traced output for FSR; bloom and flares
judged against those same captures. Frame-cost delta per mode. **Outstanding for
the AA third** — it shipped without this fixture, and the by-eye acceptance it
asks for was never recorded.

### What the legacy renderer had, and where each piece went

G1 kept every shader, so none of this is lost work — it is a wiring inventory.
Checked against the pre-G1 passes:

| feature | shader | fate | status 2026-08-09 |
|---|---|---|---|
| transparency / OIT | `oitBlendFragment` | **G8**, replaced by the sorted premultiplied draw | **done, and the shader is deleted** (`9accfeddd`, with `boxBlur4`, `gausBlur9`, `gausBlur13`). The draw is premultiplied but **not** sorted — see G8 |
| FXAA, sharpen | `postprocess.slang` | **G9** | **done.** `fxaaFragment` `postprocess.slang:184`, `sharpenFragment` `:146` |
| bloom (hilights + blur) | `postprocess.slang` blurs | **G9**, now all three modes | **not done, and the blurs it was to be built from are gone** — deleted unused by `9accfeddd` before bloom claimed them. Bloom is now a from-scratch pass, not a rewiring |
| lens flares | billboard path | **G9**, both raster modes; unfilter `LensFlare` at admission | **not done.** Still collected (`src/libs/scene/node/light.cpp:67-78`) and still filtered at admission (`src/libs/scene/render/admission.cpp:501-502`) |
| sky | — | the sky chain, after G9 | **ran early** — composited in both resolves by `fedcb7445`; see the ordering note in Part 1 and V2 |
| **SSAO** | `pbr_ssao.slang` | **unowned — decide** | **decided and built** — inside the resolve |
| **SSR** | `pbr_ssr.slang` | **unowned — decide** | **decided and built** — its own dispatch, PBR only |

The resolve currently runs a neutral AO of 1.0 and says so
(`pbr_resolve.slang`). SSAO and SSR are the two that deserve a real decision
rather than a slot: both are **screen-space approximations of occlusion and
reflection that path tracing computes properly**, so they are raster-only
catch-up, not shared features — worth restoring only if raster is meant to stand
on its own against the traced image rather than as its cheaper sibling.

**Decided the other way, and the mechanism is the argument — `fedcb7445`.** The
PBR resolve became a compute dispatch, and that is what let the screen-space
effects live where their inputs already are rather than as passes of their own.
AO needs only depth and normals, so it computes *inside* the resolve
(`screenSpaceOcclusion`, `slang/pbr_resolve.slang:204`, called at `:346-348`) and
the neutral 1.0 is now only the toggle's off-branch, not a stand-in. SSR needs
the lit image, so it is a **second** dispatch reading the resolve's result —
deliberately not a branch in the same kernel, which would read whichever
neighbouring workgroups happened to have run
(`src/libs/graphics/rendering/scenepipeline.cpp:721-724`, appended at
`renderpipeline.cpp:268-269`). Neither standalone shader exists any more.

Two consequences of the *way* it was decided, which the "unowned — decide"
framing would not have produced:

- **SSR is PBR-only, never retro** (`renderpipeline.cpp:263-267`), so the
  raster-catch-up reasoning above survives intact — it just resolved to yes for
  the improving mode and no for the preservation one.
- **Reflections substitute for the authored cube at the same `1 − alpha` weight
  rather than adding to it**, because the resolve has already put the cube in
  those pixels. Adding would double the env term, which is the same
  double-counting trap the env-map formula in Part 6 warns about.

---

# Part 3 — The path-tracing substage

Settled 2026-08-03 as design, revisable on measurement. Nothing here starts until
the raster track is done, which means after G8 and G9.

**Status 2026-08-09: the frame below is built; nothing after it is.** The
hybrid frame, the guide rule and the denoiser all exist. The fog grid, the
march, additive-as-primitives, the id grid, ReSTIR and SHARC are all still pure
design — grepping the tree for `froxel`, `raymarch`, `capsule` as a primitive,
`restir`, `reservoir`, `sharc` and `radiance cache` returns no implementation.
Also note the ordering premise this part opens with **did not hold**: it says
nothing starts until after G8 and G9, and the frame below landed while both are
still partly open. That was harmless here, but it means the sequencing in this
part is a preference, not a dependency, unless a step says otherwise.

## The frame, and why raster owns primary visibility

Raster owns primary visibility for every mode; **the tracer becomes a lighting
strategy over shared surfaces.** The frame:

```
raster geometry   →  opaque G-buffer            (shared, the megadraw)
raster blended    →  transparency layer(s)      (shared geometry, sorted remap draw)
PT pass 1         →  rays from opaque surfaces  (the G-buffer is the ray-origin set)
denoise           →  NRD over the opaque signal
PT resolve        →  opaque + blended + additive emission + sky as a layer
FSR (jitter on) / FXAA (jitter off)
```

**Built — `4980518c`.** `slang/path_trace.slang:22` says it in the source:
*"There is no camera ray. Primary visibility is rasterized in this mode as in
every other."* The kernel calls `ptReadPrimary` at `slang/path_trace.slang:201`
and traces only outwards; the surface is reconstructed from the G-buffer's
triangle id (`slang/tracing/primary.slang:18-19,33-40`), which recovers the
*geometric* normal exactly — an interpolated 8-bit stored normal could not, and
that is why the attachment is a triangle id rather than the material id it
replaced. **There is no camera-ray fallback and no option to restore one.**

Three corrections to the diagram as drawn:

- **`megadraw` is `scene_draw`** (`fedcb7445` renamed it, along with skin →
  `scene_resolve`, rayquery → `path_trace`, nrd_composite → `nrd_resolve`).
- **The blended draw is not a sorted remap draw** — the sort was rejected; see G8.
- **"NRD over the opaque signal" is now two denoisers, not one.** A combined
  diffuse+specular denoiser for the traced bounce and a diffuse-only one for
  primary-vertex direct light, registered as two identifiers in one `nrd::Instance`
  (`include/reone/graphics/vulkan/nrddenoiser.h:89-90`, types chosen at
  `src/libs/graphics/vulkan/nrddenoiser.cpp:44-46,57-59`, dispatched as two
  batches at `:581-585`). Whether direct light goes through the second one is a
  three-way option, `ShadowFilter { Off, Penumbra, Denoiser }`
  (`include/reone/graphics/options.h:86-92`) — **so "the direct channel is
  denoised" is a setting, not a fact about the frame.** Blue noise belongs to the
  tracer rather than the denoiser (`8611ae52b`;
  `slang/tracing/resources.slang:102`).

**Considered and rejected: a "pass 2" tracing rays from transparency pixels.**
Tracing from the transparency layer again is slow, and its signal cannot be
denoised — transparency has no stable guides. Blended surfaces shade analytically
in the interim (the same lit blended draw both modes share), and upgrade to
sampling the radiance cache when it exists. **Do not re-propose pass 2; the
replacement is the cache below.**

**Guide-miss is the sky case.** With guides describing the first
opaque-or-cutout hit, pixels whose guide ray misses (smoke over baked sky) must
not route through NRD — a no-surface pixel denoises to zero. The composite falls
back to the raw signal there; sky radiance is deterministic and never needed
denoising. This is a preview of the PT resolve owning the sky layer explicitly.

The rule that produced those guides is itself a decision worth not re-litigating:
**the tracer's guide surface is the first opaque-or-cutout hit** — the rule
raster already implements — while blended surfaces keep contributing radiance in
the layer loop. Blended surfaces in the traced G-buffer feed NRD guides whose
depth, normal and motion describe the glass or the smoke while the denoised
signal is dominated by what lies behind, observed as reprojection artifacts. The
traced G-buffer is the instrument for geometry and coverage for opaque and
cutout; for blended coverage it recorded a defect. At the vent camera the fix
took traced-only coverage from 831,500 to 0 and depth MAE from 2.995 to 0.0012
(`98ad7e4f`).

## The fog grid, the march, and additive as primitives

**The first fog grid, specced 2026-08-03:** a distorted player-centred world grid
— the simple incarnation of the end-state volume, built now rather than after
SHARC.

**Density source:** the per-area scene fog parameters, plus — the emitter census
(`714bd700`) settled which particles bake — **the Normal-blend billboard
smoke/dust/cloud families** (~2,250 emitter nodes across ~490 models, the ambient
always-on traversal load: vents, sandstorms, mist authored as `fx_smoke`) **and
fire/explosion (144 nodes), which bake with an emissive channel** — the voxel
carries density plus self-emission, and the march integrates
density × (cached radiance + self-emission), so fire both glows and occludes as
media. Everything else stays out of the density bake: crowd sprites and birds
(6,535 nodes — sprite *characters*, not media), and rain and wave strips (shaped,
directional). Additive families do not bake either — they become the sphere and
capsule primitives below. The census also surfaced that **1,124 lit emitters
carry an authored `tinted` flag the renderer has never consumed** — the tint
colors the media when baking, so honoring it starts there.

**Lighting:** one sample per voxel per frame with large temporal reuse.
**Consumption:** trilinear at render, a **software raymarch of density ×
emission** applied twice — after each hardware ray segment (bounces included) and
at the PT resolve when combining final channels. When SHARC arrives later it
feeds this same grid through the resample stage; **the grid's shape and consumers
do not change.**

**Media and additive are one march, and it is path-tracing only.** Retro does
exactly what the original did — sorted textured quads for everything
alpha-blended, analytic area fog, no grid and no marcher. It stays the untouched
fidelity reference, **which is what makes the approximations below judgeable: if
a traced ring blobs, the authored ring is on screen one mode over.** PBR keeps
retro's treatment for now; adopting the march there is a later, separate
decision.

### Ray-oriented additive sprites are spheres and capsules

A billboard oriented to face every ray has the same silhouette from every
direction — that *is* a sphere, and a stretched one is a capsule. So intersection
is a quadratic rather than plane-and-basis maths, the AABB is
`position ± radius` (exact, orientation-free), and the record — position, radius,
colour — is smaller than the quad it replaces. The census splits them cleanly:
~1,300 additive flare/glow/star/spark nodes are radial → spheres; ~493
motion-blur streaks and 114 linked lightning emitters are elongated → capsules.
Colour is a load-time property (texture average × tint × alpha), **which keeps
bindless texture fetches out of the march loop entirely.**

**They are volumes, not surfaces**, so the march integrates emission along the
chord instead of committing a hit. That removes the hit list, the in-register
sort and all ordering care: an additive sprite is just another emissive term in
the same integral the media already computes.

### The id grid: a uniform spatial hash, rebuilt per frame, built on the CPU

**Membership query, not field sample** — no interpolation, no resample stage, no
temporal identity, so none of the objections that shaped the radiance-cache
design apply. It is boundless (a firefight across the plaza keeps its glow) and
pathologically sparse. **Insertion is self-limiting:** a sprite covering more
than K cells goes to a small **overflow list tested unconditionally per
segment**, so pathological content self-selects instead of blowing up the table —
no need to know the size distribution in advance. A coarse occupancy bitmask over
the same key space keeps the common empty-cell case to a bit test rather than a
probe chain.

**CPU construction is not a compromise:** additive sprites are particles, already
lowered CPU-side per frame in admission, so the table builds where the data is
and uploads through the existing procedural-quad path. Watch it in Tracy; if it
ever shows, the same code moves to the merge compute, whose shape it already
matches.

### The loop, one implementation for primary and bounce

```
DDA over id cells:
  cell occupied?  gather its spheres/capsules (plus the overflow list)
  march density across the cell span at its own step rate
    per step: sum emission of primitives overlapping this step
              composite inscatter, advance transmittance
```

DDA supplies span boundaries; the density march subdivides them, so the two
structures need no aligned resolutions. **Two ray marchers is the thing to
avoid** — primary and bounce differ only in step count and jitter, not in code.

One constraint follows from the traced-transparency design: **primary must stay
deterministic, because additive emission routes through `noiseFree` and bypasses
the denoiser** — fixed steps on primary, stochastic taps only on bounces.

*Consequence:* in path-tracing mode additive sprites leave geometry entirely, so
the blended draw covers **lit-blended surfaces only** there — a filter on what
the draw covers, not a fork of it, the same shape as the G-buffer's
opaque-and-cutout rule. Lit blended fragments still sample the march's integrated
`(inscatter, transmittance)` output at their own depth, so glass behind smoke
dims correctly.

*The accepted approximation, to be judged against retro:* shaped additive loses
its shape — a ring becomes a blob, lightning a glowing tube. For flares, glows,
sparks and bolt cores, a radial profile is what the texture already was, so
nothing is lost; for the shaped minority it is a real change in the primary view.
If it reads badly, the outs are a small textured-quad path for just those
families, or an optional radial-UV texture lookup on primary where step counts
are fixed. **Decide from frames, not in advance.**

*Left open on purpose:* additive emitters lighting the media back (a bolt
illuminating the smoke around it). The per-voxel lighting sample can reach them
through the same primitive list, but the original game never did it; it is a
dial, not a requirement.

### Fog × AA, the working answer

Under TAA the march would sit as a post on the AA result to dodge reprojection;
FSR complicates that in principle — **but this engine runs FSR at NativeAA, no
upscaling**, so "before FSR at render res" and "after FSR at display res" are the
same resolution, and the choice reduces to whether fog participates in FSR's
temporal accumulation. Composite the march **after** FSR as a post using guide
depth (fog is low-frequency; it needs no AA and gains no ghosting).

And the march need not run at display resolution at all: **march at reduced
resolution** — half or quarter, as production volumetrics commonly do — and
depth-aware upsample at the composite. That decouples march cost and resolution
from the AA pipeline entirely, **which dissolves the super-resolution question
for good:** under any FSR mode the march res is its own dial, and the composite
upsamples to whatever the display res is. Revisit only if fog ever carries
frequencies a quarter-res march visibly loses.

## The traced quality lane, in order

Sequenced after the fog grid and the march. Each entry exists because of the one
before it.

1. **ReSTIR DI first.** The current light loop is already one-sample RIS with no
   reuse; reservoirs plus temporal/spatial reuse is the same estimator matured.
   It attacks variance at the source for every pixel, has no world structure to
   build or invalidate, and its temporal reuse reprojects against exactly the
   stable guide surfaces the guide fix provides. The traced-transparency
   restructure's unit-weight paths and unified primary visibility both simplify
   it, hence the ordering.
2. **SHARC on top, long term.** Spatial-hash radiance cache: sparse on-demand
   entries where paths land, multi-resolution through the key, world-anchored
   accumulation, and **the normal in the hash key structurally defuses most
   wall-leaking.** Fed by the paths we already trace; enables bounce shortening.
   Its output is point-sampled and jittered — sharp, sparse, noisy — which forces
   the next stage.
3. **Accumulation and consumption are different structures.** SHARC accumulates;
   consumers need dense, smooth, band-limited data. So a resample stage filters
   SHARC into a **dense world-space radiance volume** — well-posed filtering,
   because world-anchored resampling has no disocclusion. The volume's shape is
   one of two, decided at build: multi-octave cascades (discrete levels, seam
   interpolation at boundaries), or a single warped non-uniform grid centred on
   the player, **snapped in ~1 m jumps and resampled at the snap** — which
   converts camera motion into discrete amortised resample events with zero
   per-frame reprojection between snaps. **Rejected as the store:** froxels
   (screen-space reprojection re-imports the instability the world-space move
   exists to avoid — froxels survive only as a possible view-side fog integrator
   that holds no history of its own) and uniform world grids (leak-safe
   resolution is unaffordable, coarse resolution leaks).
4. **Volumetrics** consume the grid above. Density lives there — analytic area
   fog plus the baked smoke, dust, cloud and fire families — and radiance is the
   coarse field this stage resamples into. **Prototype transmittance first:**
   self-shadowing through the column is what makes smoke read as dense;
   inscatter without it is glowing soup and fails the look test immediately.
   **Add a Dxun exterior (4xxDXN) to the K2 fixture set before this work
   starts** — it is the all-fog stress case and the render-and-look-at-it rule
   applies.

### Revision 2026-08-09: the reservoirs go in a world-space hash grid

The sequence above puts screen-space ReSTIR DI first and SHARC "on top, long
term", as two structures. Discussion with the developer replaced that with one:
**ReSTIR reservoirs stored in a world-space hash grid** — ReGIR in the
literature — sharing the hashing scheme SHARC needs anyway. What follows is why,
and the parts that are decided versus still open.

**Screen-space reuse cannot retire the light-sampling problem.** It helps the
primary vertex and nothing else, because vertex 2+ has no pixel to reuse from.
The defect RECORD 2.6 names — bounce lighting does not exist for analytic
lights, since NEE runs at the primary hit only — survives it untouched. A
world-space structure is keyed by position, so every vertex hits it.

**It also subtracts a temporal stage instead of adding one.** Screen-space reuse
is a third temporal accumulator ahead of NRD and FSR, in a chain where
stabilization is already pinned to zero because two temporal filters in series
add their lag and the second cannot recover what the first smeared. World-
anchored reservoirs need no reprojection and have no disocclusion — the same
argument Part 3 already uses to reject froxels as the radiance store.

**At our light counts it removes the need for a light BVH entirely.** Measured
2026-08-09 across every module of both games: 15,270 lights total, K1 median 17
per module and K2 median 72, worst module 628 (`302nar`), worst single room 120
(`m44aa_01x`). Emissive triangles, counted from four scene captures, run 768
(`danm14ab`) to 6,628 (`manm26ae`). A cell only has to narrow thousands to a
handful, which a uniform or power-weighted candidate draw does; the O(log N)
descent a Conty-Kulla BVH buys is answering a question this N does not pose.

**Cell fill samples jittered points inside the voxel, not the cell centre.** The
reservoir's target is then the cell *average*, which is the distribution the
reservoirs are supposed to represent. A centre-point estimate is wrong exactly
where LOD makes cells large — distance falloff is nonlinear across the cell, so
the centre over-weights near lights for the far side — and worse, it can
evaluate to zero for a light that is grazing at the centre and well-lit at a
corner. A zero in the cell pdf where the true contribution is nonzero is lost
energy. The unbiasedness condition is only that support: the per-point RIS at
shade time re-weights against the true `NdotL·BRDF·attenuation`, so the estimator
stays unbiased as long as the cell pdf is nonzero wherever the point target is.

**The normal in the key is cheap here, contrary to the first estimate.** For a
volumetric grid it multiplies cells by the bucket count. For a surface-anchored
one it does not: a cell holding a floor patch has one normal, and bucketing only
splits cells straddling a corner or an edge — call it 1.2-1.5x. That is what
makes one key serve both payloads, and SHARC needs the normal regardless to
defuse wall-leaking.

**Occupancy is surface-scaled, which is what makes the whole thing affordable.**
Entries exist where shading points land, so the count goes as area/cell², not
volume/cell³. A 10x10x3 m room is ~320 m² of shadeable surface, so ~320 cells at
1 m; a forty-room building ~13,000; a module with its exterior 30,000-40,000,
and ~1.3x for normal splitting puts it near 50,000. At K=16 reservoirs of 16
bytes that is 12.8 MB, with the hash table under 1 MB beside it. **Memory does
not constrain this design and should not shape it.**

**Fill cost does, and it decides where fill runs.** A full refresh is
cells x K x M target evaluations — around 6.4M at those numbers — which is a
sub-millisecond compute dispatch and impractical on the CPU, where a reference
binned-SAH build already measured 0.55 us per primitive. Fill belongs in a GPU
pass beside the merge kernel, amortisable across frames because the lights are
static room-model nodes. The CPU-side build cost that framed the earlier BVH
discussion is not a cost this design has.

**The reservoir is two structures, not one.** Streaming form during fill carries
the selected candidate, the running weight sum and the candidate count. Stored
form carries only what shading reads: a tagged candidate index and the unbiased
contribution weight `W = wSum / (M * p̂(y))`, eight bytes, extended to sixteen if
cells accumulate across frames (capped `M`) or later store shadow-tested samples
(a visibility bit and the tested point). Under 8,000 candidates fit in 13 bits
with a type tag distinguishing analytic lights from emitter triangles, so the
packing has room. **Do not store the surface point on an area light**: the cell
target was averaged over jittered voxel positions, so a barycentric chosen
against that average is meaningless at the shading point — re-pick it there.
This is a GPU-shared struct and takes the schema treatment: one declaration in
`slang/lib/scene_schema.slang`, a C++ mirror with `alignas` and `static_assert`ed
offsets, both moving in the same commit. A reservoir array is the most
stride-sensitive thing this design adds, so keep it a power of two — TRC-004 was
a grown struct changing storage-buffer array stride into `VK_ERROR_DEVICE_LOST`.

**SHARC's update pass is separate from the render pass, and that is a decision
rather than a detail.** Point 2 above says "fed by the paths we already trace",
which reads as updating the cache from the main path trace. The reference shape
is a dedicated update pass at reduced resolution — a jittered subset of pixels,
deeper paths than the render pass needs, depositing at every vertex — with the
render pass only querying. Three reasons it wins here. FSR sits in the chain, so
coupling makes cache density a function of render resolution and of whatever the
upscaler is doing. Full-resolution deposits are badly distributed rather than
merely wasteful: pixel density concentrates them near the camera on
camera-facing surfaces, which is not where cell coverage is scarce. And `ptspp`
is 1 in the live config and 3 by default, so coupling ties cache fill rate to a
performance dial — turn spp down and the cache degrades with it, which is
backwards. Either way SHARC is camera-anchored: cells exist where paths recently
landed, and what the camera stops seeing ages out.

**Sequencing, revised.** Per-vertex light sampling is a precondition for SHARC
being worth reading, not a neighbour of it: the cache accumulates whatever the
paths find, so with NEE at the primary only it caches the hemisphere-only
estimate and bounce shortening propagates that deficiency faster. So the grid
lands first, carrying reservoirs; SHARC's radiance payload joins the same
hashing scheme afterwards. Items 1 and 2 above invert.

**Open.** Whether the two payloads share one key and one table or share only the
hashing scheme; cell size against world scale, which wants the real light-radius
distribution rather than the 1 m assumed here; and whether cells store
shadow-tested reservoirs, which is the one thing this design shares with the
BVH's blind spot — a cell has a position and a normal but no visibility.

## The additive-gathering ladder

The five-rung ladder for gathering additive emission along a segment — and why
rung 5, a flat O(N) loop over a per-frame world-space primitive list, beat both
the in-loop array and a dedicated additive TLAS — is recorded in
[RECORD.md](RECORD.md) under the traced-transparency design, which is where it
originated. The substage depends on two of its conclusions: additive emission is
gathered over the confirmed segment rather than committed, and the primary ray
must stay deterministic because that emission routes through `noiseFree` and
bypasses the denoiser. *That channel still exists and still bypasses:*
`slang/tracing/outputs.slang:80`, storage image at `:39`, composited at `:158`
as `final = noiseFree + diffuse·diffFactor + specular·specFactor`.

---

# Part 4 — V: raster becomes primary visibility, and the sky composites once

## V0 — fix the baker, and make the offline asset the only sky

**Open as of 2026-08-09, and the whole diagnosis below still holds.** Two things
moved and neither is the substance: the offline tool is now 1,488 lines, and the
runtime bake is `Sky::bakeSkyRoom` (`src/libs/graphics/rendering/sky.cpp:79`),
not `VulkanRayQuery::bakeSkyRoom` — it moved out of `vulkan/` with the S5 stage-3
relocation. **The premise "V0 must be true before V2 can happen" was overtaken:
V2 happened anyway in `fedcb7445`, composited from the runtime cube.** So V0 now
gates V4 and V5 only, and V2's sky is the one V0 exists to replace.

**There are two bakers and only one of them has a consumer.**
`src/apps/skybake` is the offline tool — 1,392 lines, casting rays from inside
the shell into six faces — and its committed configs live in `override/k1` and
`override/k2`. `VulkanRayQuery::bakeSkyRoom` is a runtime GPU bake of the same
idea. Grepping `src/libs` and `src/apps/engine` for a consumer of the offline
assets returns **nothing**: every sky rendered today comes from the runtime bake.
The offline tool is the intended survivor, so **V0 is what has to be true before
V2, V4 and V5 can happen at all.** *Re-checked 2026-08-09: still nothing reads
them.* `skybake` writes `px.png`…`nz.png`, `equirectangular.png` and a `sky.ini`
manifest (`src/apps/skybake/skybaker.cpp:1017-1031`); no file outside
`src/apps/skybake` mentions any of those names. `slang/lib/skycube.slang` — read
by both raster resolves and by the tracer — samples `Sky::_skyCube`, the runtime
product (`src/libs/graphics/rendering/sky.cpp:96-201`).

Two defects, one of them structural:

- **Seams on the box geometry.** Undiagnosed. The instrument is prescribed:
  *bake a six-colour debug sky first and confirm empirically which world
  direction shows which face.* Six flat faces make both faults self-evident — if
  seams survive on flat colour the fault is face frustums or edge sampling; if
  they vanish it is content-side (shell UV seams, tiling, filtering). Run that
  before touching anything, since it doubles as the axis-convention proof (KOTOR
  is Z-up, cube faces are Y-up, and a mirrored or yawed sky looks plausible
  enough to ship).
- **Whole-room granularity swallows the props.** Every committed config entry is
  `room = <room>` / `sky = <room>`; `grep -c meshes` over both `modules.ini`
  files returns **0** *(re-run 2026-08-09: still 0 and 0)*. The wiring is not
  what is missing — `403c0802` gave the tool a per-mesh list (`skybaker.cpp:88`,
  now `:96`), parses `meshes =` when a config carries it (`skybaker.cpp:676`, now
  `:684-691`) and emits a draft one when it generates a config
  (`skybaker.cpp:625`, now `:633`). The committed configs predate that and carry none,
  so every one of them still falls back to the draft `shellMeshes()` heuristic.
  So `001ebo16` goes in as one lump — the star shell *plus three asteroids, a
  planet and a nebula* — which is exactly the city-skyline / planet / asteroid
  content that must stay geometry. **V0 is therefore curation, not plumbing:**
  fill `meshes =` in for the rooms that hold props.

*Acceptance:* the six-colour probe renders with correct face-to-direction mapping
and no seams; a real bake of a props-holding room contains the shell **and
nothing else**; and the assets remain loadable by nothing yet — V0 fixes the
producer only, so it can be judged on its own output rather than through a
renderer that does not read it.

*Coverage is the long pole, and it is content work, not code:* 56 of 117 K1
modules and 46 of 82 K2 name a sky, and every entry is still marked `# review` —
they are drafts, and a wrong `room =` silently suppresses level geometry.

## V1 — hybridise

**Landed in full — `4980518c`. V1a, V1b and V1c are all done.** What follows is
the plan as written; keep it for the sequencing argument and for the V1c warning
at the end, which is the one part that did *not* come true.

`ScenePipeline::init` no longer early-returns: the G-buffer is allocated
unconditionally in all three modes
(`src/libs/graphics/rendering/scenepipeline.cpp:152-157`), and the only mode gate
left guards the shadow maps (`:188`). The mode reaches the pipeline as a
`_primaryRayMode` bool rather than as a `RenderMode` — the string `PathTracing`
does not appear in that file at all. `RTDebug` was never built; see the ordering
note in Part 1 for what took its place.

`PathTracing` bypasses raster entirely today: `VulkanScenePipeline::init` returns
before allocating the G-buffer (`scenepipeline.cpp:171-183`). There are **three**
modes — `Retro`, `PBR`, `PathTracing` — not four; `RTDebug` is still planned.

Phase-sized, but *not* a graph rewrite: `VulkanScenePipeline` is already a frame
executor with a `VulkanSceneFramePlan`, and the raster modes already select steps
from it. What is missing is that PathTracing early-returns out of the whole
shape. Removing that reaches initialisation, target ownership, barriers and
dumping, plan construction, and the tracer's output contract.

| | change | what proves it |
|---|---|---|
| **V1a** | allocate the G-buffer in traced mode | **First establish whether this is work at all.** `--dumptargets` in PathTracing already emits `g_buffer_depth.npy` at full size, entirely zero, against raster's 1.77–645. Either it is allocated and unwritten, or `dumpTargets` synthesises zeros for an absent target. The source and the dump disagree; settle it before writing code |
| **V1b** | run the geometry pass in traced mode, write the G-buffer, discard it | `g_buffer_depth` from PathTracing **byte-identical to PBR's** at the same camera. That is the whole proof that raster visibility is right in traced mode, available before anything depends on it. Traced image unchanged; only frame cost moves |
| **V1c** | the tracer takes its primary hit from the G-buffer instead of tracing camera rays | traced output changes by design — compare distributions across three runs a side and judge the images. **This deletes the traced G-buffer instrument, so it must not land while the raster track still needs it** |

**The V1c warning did not play out as written, and the way it failed is the part
worth keeping.** The instrument was not deleted — `traced_diffuse`,
`traced_eye_normal`, `traced_depth` and `traced_motion` are still emitted
(`src/libs/graphics/rendering/tracingpipeline.cpp:243-248`). They are now filled
from the raster-derived primary surface (`slang/path_trace.slang:254-259`), so
they agree with raster by construction and can no longer disagree with it. **A
cross-check that cannot fail is not a cross-check**, and this one looks exactly
as it always did. The constraint should have been "V1c invalidates the traced
G-buffer instrument", which is the stronger and less avoidable statement:
deletion at least announces itself.

The design also gained a mechanism it did not have when written: **the G-buffer
names the triangle**, not the material. That is what makes the reconstructed
primary exact rather than approximate, and it is described in Part 1 under the
material-data rule.

## V2 — composite the sky once

**Landed — `fedcb7445` — but not as one pass, and the difference is the
interesting part.** The goal ("one implementation, ordered so it cannot paint
over transparency") was met by deleting the pass rather than by placing it. The
sky is a shared *function*, `skyRadiance` (`slang/lib/skycube.slang:41`), called
by both raster resolves at the no-material sentinel
(`slang/retro_resolve.slang:129,140`; `slang/pbr_resolve.slang:305,314`) and by
the tracer for a miss (`slang/tracing/resources.slang:263-264`, which says in
the source that the bodies are shared *"so the raster resolves and the tracer
cannot drift"*). `slang/sky.slang:60-67` is the tombstone for the pass this step
designed.

Two substantive changes to what is written below:

- **The test is not `depth == 1.0`; it is `triangleId == kNoTriangle`**
  (`pbr_resolve.slang:305`, `retro_resolve.slang:129`). Same pixel set — one
  fragment writes both the triangle id and the depth, and the source says so at
  `pbr_resolve.slang:300-302` — but the sentinel is the one to code against now.
- **Ordering stopped being a question.** A sky shaded inside the opaque resolve
  is before transparency by construction; there is no pass left to misplace. The
  acceptance below is correspondingly unrunnable as written (no sky on/off
  toggle), and no equivalent was recorded — **the "particle and flare pixels must
  not move" check was never performed**, though it is weakened by lens flares
  still not being drawn at all (see G9).

**Scope: the raster modes.** In path tracing the sky is a layer of the PT resolve
(see the frame diagram), which composites it against everything else at once and
cannot paint over anything. What follows is retro and PBR, where there is a
discrete transparent pass to order against.

One pass, **after the opaque resolve and before transparency**. Not at the end of
the chain, which sits past the transparent pass and would paint over particles
and lens flares:

```
G-buffer → shade → SkyComposite → transparency → post → filters
```

An integration attempt put the sky in the PBR resolve *and* in postprocess — two
implementations of one idea, the same fault that got the runtime bake deleted.
With retro and PBR both shading from one G-buffer there is one place for it.

`depth == 1.0` on the device-depth attachment is the test. Note `sGBufDepth`
**is** device depth in `[0,1]` — `pbr_resolve.slang:63-81` states it and
reconstructs position from it *(the statement moved with the compute rewrite; it
is now `reconstructViewPos`'s doc comment at `pbr_resolve.slang:169-187`,
specifically `:175-176`)*. An earlier draft claimed linear view-space
distance, from misreading a `--dumptargets` dump, which linearises.

*Acceptance:* a fixture with an opaque prop, an alpha-blended particle and a lens
flare. Sky off versus sky on, filters disabled. **The changed-pixel set must be
exactly the far-depth set, and the particle and flare pixels must not move.**

## V3 — the tracer keeps only transport

**Restated by V1c rather than done.** `ptSkyRadiance`
(`slang/tracing/resources.slang:263`) still has two call sites: bounce miss
(`slang/path_trace.slang:441`) and the uncovered primary
(`slang/path_trace.slang:207`). But the second is no longer a *miss* — there is
no camera ray to miss, so it is a test of the G-buffer's no-coverage sentinel,
which is the same condition the raster resolves branch on. **The two calls have
therefore stopped being the same kind of thing**, which is what this step was
trying to achieve, even though neither call moved. What is still genuinely open
is the tidying: the uncovered-primary sky still routes through
`outputs.noiseFree`, so the tracer still composites rather than only
transporting.

`ptSkyRadiance` on **bounce miss** stays: that is transport, and it is what makes
the sky an environment light. Its **primary-miss** call is compositing and moves
to V2.

Two things move with it, so V3 must say what replaces them: primary-miss sky
currently feeds `outputs.noiseFree`, and it supplies the cyan sky colour the
surface debug view uses.

*Acceptance:* a fixed-seed fixture whose camera sees an opaque surface and whose
first secondary ray misses. Hash the traced result across the change.

## V4 — suppress exactly the shell

The manifest is `override/*/modules.ini`. **V0 is what puts the shell meshes in
it** — today every entry names a whole room and no config carries a `meshes =`
key, so there is not yet a per-mesh list for anything to read. Once V0 has
curated one, V4 makes the renderer read that same list, **so baker and renderer
share one source of truth instead of each evaluating a rule and hoping they
agree.** Today the renderer does the latter: the shell is suppressed by a
geometric room heuristic, which is what the original decision wanted deleted.
*Unchanged 2026-08-09.* The heuristic is
`src/libs/scene/render/admission.cpp:578-630` — among rooms flagged
`isBackgroundScenery()`, take the largest by volume whose AABB spans at least
`kSkyOverlapThreshold = 0.5` of the whole-scene extent on **every** axis — and
the suppression it drives is `classifyMesh` at `:191-196`. Nothing in `src/libs`
opens `modules.ini`; the only reader in the tree is
`src/apps/skybake/main.cpp:108`.

Neither game marks the shell distinctly enough to infer it. K1 omits the walkmesh
from a sky room but says nothing about props inside it; TSL flags meshes
individually and flags the props too. `001ebo16` flags all thirteen of its
meshes, and that set is the star shell **plus three asteroids, a planet and a
nebula**. **Suppressing by flag deletes a planet and looks like the sky works.**

*Acceptance:* capture `001ebo` and `manm26ad` and confirm the asteroids, the
planet and the Ahto City rings are **still drawn**. The bar is the props, not the
sky — a sky that renders correctly while quietly removing scenery passes every
sky-shaped test.

## V5 — delete the runtime bake

**Open, and V2 raised the stakes.** Since `fedcb7445` the runtime cube is what
*every* mode's sky is sampled from, not just the tracer's — so deleting it now
blanks the sky in retro and PBR too unless V0's offline assets have a consumer
first. The V0-before-V5 ordering was always stated; it is now load-bearing in a
second place.

`bakeSkyRoom` and its call sites, `slang/sky.slang` and its shaderpack wiring,
and the shadow-ray candidate rejection at `slang/tracing/trace.slang:167`, which
exists only to cope with sky geometry possibly still being present. Larger than
one line: the same removal reaches the sky feature bit and the classifier that
feeds it.

*Three cites re-checked 2026-08-09.* `bakeSkyRoom` is
`src/libs/graphics/rendering/sky.cpp:79`, called from
`src/libs/scene/render/pipeline/renderpipeline.cpp:207`. **There is no shaderpack
wiring left to delete** — the app is gone and Slang compiles in-process, so
`sky.slang` is simply an entry in the module table at
`src/libs/graphics/vulkan/shadercompiler.cpp:42`. The shadow-ray rejection is now
`slang/tracing/trace.slang:183-186`, and its comment still states the rationale
this step gives it verbatim.

V5 also inherits an untangling job from the raster deletion: **the sky bake still
rides raster's per-mesh infrastructure** — `VulkanMesh` and its vertex input
descriptions, the pipeline key's vertex input fields, the per-mesh resources and
zero buffer, `LocalUniforms` and the uniform ring. Giving it its own *shader* was
not the same as independence, and all of that had to survive the raster
deletion for the bake's sake alone. *Still exactly true, and now measurable:*
`VulkanResources::drawMesh` (`src/libs/graphics/vulkan/resources.cpp:537`) has
**one** caller in the entire tree — `src/libs/graphics/rendering/sky.cpp:195`,
inside the six-face bake loop. `LocalUniforms`, `BoneUniforms` and
`DanglyUniforms` are all still declared (`include/reone/graphics/uniforms.h:153,
200, 205`) and still size the uniform ring's per-kind slots
(`src/libs/graphics/vulkan/descriptors.cpp:72-74`).

*Acceptance:*
`rg -n 'bakeSkyRoom|clearSkyRoom|RayQuerySkyRoom|skyAvailable' src include slang`
returns no runtime-bake remnants, and `slang/sky.slang` does not exist.

---

# Part 5 — The structural track, S0–S6

`phase-f` was the feature track: what the renderer draws and how it looks. This
is the structural track: what the renderer *is* — where scene state lives, what
the CPU does per frame, what the code that a person reads actually says. Compiled
2026-08-04 from a four-part architecture survey (object model, scene↔GPU
boundary, Vulkan surface census, GPU representation), a Tracy measurement of the
same date, and the backlog.

## The goal, and what is deliberately not the goal

**Modularity first.** Two invariants define it, and every step is judged against
them:

1. **Render-side CPU is O(changes), never O(objects) or O(passes).** The
   steady-state frame is: replay this frame's change-log into GPU tables
   (proportional to what moved), then record a fixed set of dispatches and a
   handful of draws against persistent descriptors. **A new render feature adds a
   GPU pass, a pipeline and a shader — never a per-object CPU loop, never
   per-frame descriptor churn.** Growth in scene content lands on GPU-computed
   indirect counts.
2. **The CPU/GPU seam is a data contract, not a call contract.** Scene state is
   defined once, in Slang, and consumed by every pass. A new consumer needs zero
   scene-side C++ changes. There is deliberately *no* per-frame per-object hook
   to add code to — **that hook is how every existing creep entry got in** (flare
   LOS per light, settle per mesh, skin rebuild per mesh, particle
   re-registration per tick).

**Not the goal: recovering the CPU frame.** The 2026-08-04 Tracy run settled this
— the scene→GPU pipeline is already ~0.5 ms; the remaining CPU lives in
`SceneGraph::update` self (1.25–1.35 ms, uninstrumented) and `Game::update`
(0.63–0.71 ms), most of which is game logic this track does not touch. **Perf
improvements fall out of the structure; they are not its acceptance test.** Also
not the goal: a second graphics API. The RHI is shaped so one *could* exist, but
portability justifies nothing here — logic decoupling does.

## What the 2026-08-04 measurement settled

Steady state, `danm14ab`, headless, both modes; two independent runs agreeing
3–8%:

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

Consequences for ordering: the biggest attributable per-frame scene costs are the
deformation streams and classification dedup, not collection; the biggest
*unattributed* cost is inside `SceneGraph::update`, which S0 must open before S3
commits to an order; and the diagnostics skill's calibration line
(`collectInto` ≈0.73, `prepare` ≈0.60) predates the persistent-registration and
GPU-grass work and is stale by 46×/4×.

## Interleaving with the feature track

The two tracks interleave; neither blocks on the other finishing.

| structural step | earliest sensible point | why | state 2026-08-09 |
|---|---|---|---|
| S0 | now | pure instrumentation; G8/G9 benefit from it immediately | part-done: some scene zones, no `Game::update` zones, no GPU timestamps, no scaling fixture |
| S1 | now | independent of the raster track; everything after it consumes it | **done** (`a6dc6ed1c`) |
| S2 | after G8 lands | G8 is the last step that grows the classification/record vocabulary; invert once the schema is quiet | not started |
| S3 | after S2 | the change-log is what the sims write into | not started |
| S4 | after S2, BLAS half after the AMD build measurement | residency partitions the tables S2 creates | not started |
| S5 stage 1 | now, opportunistically | the four builders are Vulkan-internal and step on nobody | **done** |
| S5 stage 2 | after V1c | the frame shape (who owns primary visibility) must stop moving before the seam is formalized | **done, and built *before* V1c — the gate was a preference, not a dependency; see "S5 as built"** |
| S6 | trailing | deletions become possible as consumers disappear (V5 unhooks the last `VulkanMesh` user) | not started; V5 has not landed, so the `VulkanMesh` blocker still holds |

Three backlog items are absorbed rather than duplicated: **R2** (the
translation-layer deletion — **S2 *is* R2**, with the sync design made concrete),
**dangly into the merge kernel** (S3), and **Slang in the engine** (S1, promoted
from P3 to track-blocking because every later step multiplies shaders and schema
consumers).

## R2's reasoning, which S2 inherits

R1 made the rebuild cheap. **R2 asks the question R1 should have started from:
why does admission exist at all if the scene can be in the right form in the
first place?** Classification is a pure function of the material — blending mode,
texture features, dials. A pure function of rarely-changing inputs is not a frame
phase; it runs when the input changes.

The layer is historical: it was the tracer's private policy grafted beside
raster's material system, and unifying the policy kept the shape of a per-frame
pass. What remains genuinely per-frame is dynamic *data* — bone palettes, dangly
positions, particle instances, billboard basis — **not classification of
anything.**

End state: **nodes own slots in the persistent GPU-shaped tables.** Setting a
material computes its record and kind once, through the same shared function —
one policy, preserved — and interns it. Moving writes matrices into the slot.
Animation writes palettes into its own arena. **A frame is *flush dirty ranges,
append dynamic streams*.** The `Registered*` intermediates and the per-frame
assembly loop die; one schema instead of three.

The wrinkles that look like blockers and are not: calibration dials re-bake
affected records on the generation bump (set-time, not frame-time), and the sky
rule is a registration-time predicate once the room is known.

The persistent-registration substrate carries over whole — stable-id table, dirty
hooks, refcounted interner, node-owned streams, canonical order — and above all
**the shadow tripwire**, which is just as necessary when nodes write tables
directly: stale-slot bugs are the same failure class, and it caught a real
animation-timing bug within hours of existing.

Two load-bearing decisions from that substrate that still bind:

- **Canonical order.** Incremental add/remove cannot reproduce a per-frame walk
  order, so both paths order opaque-first-then-stable-id. One-time
  traced-distribution shift; byte-comparable forever after.
- **The full rebuild stays alive as a shadow path.** Behind a flag, every frame
  builds the upload both ways and compares hashes — the stale-invalidation
  tripwire, run through every acceptance capture, **including a module transition
  so unregister/re-register is exercised.** The upload-hash instrument is what
  makes this class of refactor checkable at all.

## S0 — attribute the blind spot, arm the guard-rails

The cheapest step and the one everything else is ordered by.

- **Sub-zones** inside `SceneGraph::update` (`updateAnimations`, `refresh`,
  `updateShadowLight`, `prepareOpaqueLeafs`/`prepareTransparentLeafs`,
  flare/particle blocks) and inside `Game::update` (module, objects, AI/scripts,
  GUI, imgui begin). Also `snapshotPreviousFrame` under `SceneGraph::render`.
  Tracy macros compile out when off; there is no cost argument against this.
  *Partly done 2026-08-09.* The macro is `R_PROFILE_ZONE`
  (`include/reone/system/profiler.h:24`). Present in `src/libs/scene/graph.cpp`:
  the outer `update` (`:239`), the **flare** block (`:265`), the **particle**
  block (`:287`), `updateLighting` (`:304`, not on the list above), `render`
  (`:535`) and `collectInto` with three sub-zones (`:720-753`). Missing:
  `refresh` (`:441`), `updateShadowLight` (`:344`), `prepareOpaqueLeafs`
  (`:484`), `prepareTransparentLeafs` (`:488`), and `snapshotPreviousFrame`
  (`:691`). **There is no `updateAnimations` function to instrument** — the
  animation walk is the inline `root->update(dt)` loop at `:246-256`, which is
  why it never got a zone. `src/libs/game/` contains zero `R_PROFILE_ZONE` sites,
  so none of the `Game::update` half exists.
- **GPU timestamps in `IStatistic`.** Every GPU-side claim in S3/S4 is unscoreable
  without it; the backlog already calls it the blocker.
- **The scaling fixture — the no-creep tripwire.** A headless run: `warp testbed`,
  spawn N of one creature blueprint, Tracy capture; repeat at 10×N. Assert the
  render-side CPU zones (collection, flush, record) are flat within noise while
  N-proportional zones are only the ones declared N-proportional (animation, for
  now). This is the shadow-oracle idea applied to performance: **creep is caught
  the frame it lands.** Runs on demand, not in CI — but every S-step's acceptance
  includes one run.
- **Update the diagnostics skill's calibration line** to the current numbers.

*Proves itself:* a zone table where ≥90% of `update`+`graphics` CPU is in named
leaf zones; the fixture produces its two numbers unattended; the S3 ordering
question (is leaf-prep or animation the bigger half of the 1.25 ms?) has an
answer.

## S1 — Slang in the engine, one schema

**Built — `a6dc6ed1c`, "Slang compiles in the engine, and the scene schema has
one source", which is also the commit that deleted `src/apps/shaderpack`.** Both
halves are in. The step text below is kept as the design; three notes where the
result differs from what it asked for are inline.

Two halves, one step, because the second is what makes the first pay.

**Runtime compilation.** Link Slang, compile at startup and on demand, cache
compiled SPIR-V keyed on source hash so warm startup pays nothing, errors to the
console, keep the last good module per pipeline so a typo does not take the frame
down, a force-recompile command. The build-time shaderpack step and its
stale-module trap both cease to exist. *As built:*
`src/libs/graphics/vulkan/shadercompiler.cpp` — session in-process at `:174-182`,
all modules compiled at `init()` `:243-259`, cache at
`%TEMP%/reone/slang-cache` (`:238`) keyed on an FNV hash **of every `.slang`
file in the tree, sorted, plus a config-version string** (`:271-291`), last-good
retention at `:523-525`. The force-recompile command is `recompileshaders`
(`src/apps/engine/engine.cpp:273-283` → `recompileAll()`, `:510-530`).

**One schema.** `MergedVertex` was declared four times (1 C++, 3 Slang);
`InstanceMaterial` three times; the sync mechanism was a comment. The scene
tables move into one Slang module (`slang/lib/scene_schema.slang`) that every
shader imports, and the C++ mirrors are *verified against Slang reflection* —
extending exactly what the offline uniform generator already does for the uniform
blocks, now available at runtime because the compiler is in the process. **A
wrong mirror fails at startup naming the field, not at 2 a.m. naming nothing.**
The storage-buffer-stride hazard gets its tripwire the same way. **Every Slang
field is offset-checked, not a sample**, so swapping two adjacent `float4`s is
caught where a stride comparison alone would pass. The rule the mirrors follow:
**a schema mirror carries the same name as its Slang struct**, and only CPU-only
types keep a `GpuScene` prefix. *As built, and it went further than asked:*
`SlangShaderCompiler::validateSchemas()`
(`src/libs/graphics/vulkan/shadercompiler.cpp:538-608`), called at startup from
`renderer.cpp:123`, checks **seven** types rather than three — `InstanceMaterial`,
`Matrix3x4`, `MergedVertex`, `SceneObject`, `ProceduralQuad`, `GrassFace`,
`GrassRange` (`:612-703`) — and enforces four conditions, not one: per-field
offset (`:587-590`), a field Slang has that the mirror does not (`:592-593`), a
field the mirror has that Slang does not (`:596-601`), field count (`:602-604`),
and stride (`:605-607`).

**Dead shader inventory rides along:** enumerate shaderpack entries against live
pipeline creation, delete the orphans. *Done, and repeatedly since* —
`901cee651` deleted eight, `5802dd7df` and `8606171d5` folded ten modules into
their consumers, `9accfeddd` swept four unused fullscreen entry points.

*Proves itself:* raster captures byte-identical across the change (it is a
toolchain move, not a shader change); a deliberately mis-sized C++ mirror aborts
startup with the field named; cold and warm startup cost measured and stated;
`rg -c 'struct MergedVertex|struct SceneObject|InstanceMaterial'` finds each once
in Slang and once in C++. *Met, and the last gate is met more strictly than
written:* each struct is declared **once, in Slang only** —
`slang/lib/scene_schema.slang:28,75,89` — with no C++ redeclaration at all. The
mis-sized-mirror abort has a regression test that asserts the message names the
field (`test/graphics/vulkan/shadercompiler.cpp:114-140`).

**Deliberately still open, so it is not mistaken for finished:** the uniform
blocks are *not* on this path — `uniformlayout.generated.h` is still produced
offline and committed, so **there are two schema mechanisms** until that is
folded in.
**Closed since — but check the residue.** `uniformlayout.generated.h` no longer
exists and nothing generates it; the uniform blocks are size-checked through the
same in-process reflection at `shadercompiler.cpp:748-769`. What survives is a
dead probe: `CMakeLists.txt:268-276` still looks for `slangc` "for the explicit
uniform-layout regeneration utility" and sets `SLANGC_EXECUTABLE`, which nothing
reads. **One schema mechanism, one misleading build message.**

**Warm compile cost is the number to watch if module count grows.** Measured cold
3.8–4.2 s, warm 144 ms. The warm figure started at 282 ms because `module()` was
re-reading all 41 sources on every call to answer "is this current", which also
ran on every lazily created pipeline mid-game; only the explicit reload paths
hash now. **Those numbers are no longer anchored to anything.** The set has been
consolidated hard since: 12 modules in `kModules` against the twenty this step
was measured on, and 28 `.slang` files against 41. Re-measure before citing
either figure — the *rule* (watch warm cost as module count grows) is what
survives, and the trend has been the other way.

### The heap-corruption finding, which outlives the step

**Releasing Slang's global session takes the heap with it** (`STATUS_HEAP_CORRUPTION`,
0xC0000374). The session owns Slang's compiler back-end DLLs, and dropping the
last reference faults. It is now created once per process and **deliberately
never released**, which is also what Slang's own guidance asks for since creation
is expensive. That took engine captures from 3/3 crashing to 4/4 clean, cold
cache included. The engine crash was teardown-only — module loaded, frame
rendered, screenshot written, frame-slot line logged, then the fault — **which is
why it was invisible to anything that judged the image.**

**`tests.exe` is not fully fixed by that, and the residue is still open:** with
the session retained, a *cold-cache* Slang-only run still faulted 1/3 while warm
runs were clean 2/2. The engine compiles all twenty modules cold without
faulting, so the distinguishing factor is not compilation itself but how often
the test does it — `recompileAll()` force-recompiles the whole set a second time,
and several tests each build their own compiler over a copied source tree.
**Per-module `ISession` churn was tested and ruled out as a fix**: reusing one
session per compiler still faulted the first Slang test on 1/5 isolated
cold-cache launches. The experiment was reverted rather than retained as an
unproven workaround. **Until the remaining cause is isolated, treat a green
full-suite run as weak evidence — this failure has looked absent twice and was
not.**

**The remaining cause was isolated — `6d1f79ce7`, "The Slang module was released
without ever being retained".** It is a *second, independent* refcount bug in the
same file, and it explains the residue this paragraph could not: `ComPtr::attach`
takes ownership **without** adding a reference, but `loadModule` returns a module
the session keeps in its own cache. Attaching it meant scope exit released a
reference this code never held, destroying the session's cached entry while the
session still pointed at it. Symptom: `0xC0000374` or an outright hang on roughly
two runs in three. The fix is three lines — assign rather than attach
(`src/libs/graphics/vulkan/shadercompiler.cpp:222-225`) — and twelve consecutive
suite runs pass where the same loop previously produced two heap-corruption exits
and a hang.

**The "green is weak evidence" rule should be retired only on a fresh cold-cache
run, not on this paragraph.** The 12/12 was the measurement the commit took; it
is stronger than anything the residue had, and it is not the same experiment as
the 1/5 isolated cold-cache launches above. `MASTER.md`'s TOOL-001 has not been
updated for this commit either.

*The generalisable finding, which is why this stays: two different refcount
mistakes on the same object produced the same crash signature, and fixing the
first made the second look like an unexplained residue rather than a bug. A
partially fixed heap corruption is more misleading than an unfixed one.*

Two wrong turns are recorded because both cost time. The first attribution — heap
damage done by earlier image-decoder tests and merely *detected* by Slang — was
**wrong, and wrong for a bad reason**: it assumed Google Test's filtered run
preserved the full-suite order without checking, when the filtered run schedules
the Slang suite first. The crash reproduces in the first Slang test alone against
a fresh `TEMP`, so no other suite is involved. The second was the debug/release
CRT mix; it is a real fault and is fixed, but the crash rate was unchanged either
side of it.

A distinct teardown-order defect was found in the same pass and fixed:
`Engine::deinit()` never reset `_vulkanRenderer`, so the renderer — holding the
surface created from the window — was destroyed by `~Engine`, after
`_window.reset()` and `SDL_Quit()`. `_shaderCompiler.deinit()` preceding
`_device.deinit()` was checked and was never the problem. **A Debug capture used
while chasing this blocked in the final `VulkanRenderer::endFrame()`, so the
checked-VMA path has still not been exercised to completion — worth finishing
separately, since it is the standing instrument for this bug class.**

## S2 — finish the inversion: nodes own GPU slots (this is R2)

**Not started, verified 2026-08-09 — every premise below still holds.**
`RegisteredSkin/Dangly/Saber` are at `include/reone/scene/gpuscene.h:64,68,72`,
`ObjectRecord` at `:125`; `GpuSceneAdmission::prepare` is still a per-frame phase
(`src/libs/scene/render/admission.cpp:558`, called once per frame from
`renderpipeline.cpp:236`); classification still runs every frame
(`classifyMesh` `:190`, `classifyProcedural` `:418`), memoised only by the
`dirtyAdmission` generation this step already describes as the dial wrinkle. No
change-log exists — grepping for one returns nothing.

What this step adds to R2's end-state is the concrete machinery:

- **Persistent device tables** — objects, interned materials, bone arena, dangly
  arena, procedural sources — with slot allocation at registration (stable
  `SceneNodeId` index is the natural key). The `_nodes`-never-releases leak
  becomes load-bearing here and **is fixed as part of this step, not deferred
  around.**
- **Sync model, decided: per-frame-in-flight table copies plus a change-log.**
  Mutations append `(slot, range, payload)` to a CPU log; each frame replays the
  log segments the in-flight copy has not yet seen, then truncates behind the
  oldest frame. **Chosen over single-copy versioned slots** because it keeps
  writes sequential, makes the frame's CPU cost literally proportional to the log
  length, and gives the shadow oracle a natural observation point (hash the log,
  or rebuild-and-compare a full frame behind the flag — R1's tripwire, retained
  verbatim).
- **Classification and the material record are set-time.** Setting a material
  computes record + kind once through the shared classifier and interns it.
  `dirtyAdmission` generations survive for dial changes — a re-bake on bump,
  set-time not frame-time.
- **The borrowed pointers die.** `RegisteredSkin/Dangly/Saber`'s raw pointers into
  node-owned vectors — the boundary's standing lifetime hazard — are replaced by
  owners writing into their arena slots through the log. Same for the grass
  face-table pointer.
- **What gets deleted:** the `Registered*`/`ObjectRecord` intermediates, the
  per-frame `prepare`/upload-vector assembly, `GpuSceneAdmission` as a frame
  phase (its policy lives on in the set-time classifier; a thin flush remains),
  and the second scene-side read path (`RayQueryPipeline` reading `objects()` for
  the sky bake) — **V5 deletes the bake, S2 must not recreate the pattern.**

*Proves itself:* R2's own gates, unchanged — zero shadow-oracle mismatches across
the acceptance set including a module transition; G-buffer dumps byte-identical
between incremental and forced-full paths **in one binary** (no phase-drift
excuse); upload-hash equality across all three modes holds. Plus: the scaling
fixture shows collection/flush flat in N, and the former collect+prepare zones
reduce to the dynamic streams alone.

## S3 — the remaining per-frame CPU work becomes events or GPU

**Not started, verified 2026-08-09 — all five loops are still there.** Skin
palettes: `MeshSceneNode::buildDeformation` rebuilds 128 bones unconditionally
(`src/libs/scene/node/mesh.cpp:352-376`). Dangly: the spring is still solved on
the CPU at `mesh.cpp:167-183` and packed at `:377-386`. Particles: CPU
simulation in `src/libs/scene/node/emitter.cpp:98-188`, with `addParticles`
re-registration at `:335`. `settleMeshTransform`:
`src/libs/scene/gpuscene.cpp:411`, called per mesh per frame from
`mesh.cpp:420`. Flare LOS: `SceneGraph::testLineOfSight`
(`src/libs/scene/graph.cpp:974`) walking `_walkmeshRoots` on the CPU, called at
`:269`.

Ordered by the measured table, revisable by S0's attribution. **Each item deletes
a per-frame-per-thing CPU loop — the creep checklist, retired:**

- **Skin palettes become event-driven.** 61 meshes × 128 bones rebuild
  unconditionally today (0.098 ms). Palettes are computed only when a mesh's
  animation actually advanced, written through the change-log. **Animation
  evaluation itself stays on the CPU** — game logic reads bone transforms
  (attachments, hardpoints), so moving it would duplicate state. Skinning is
  already GPU (the merge), palette *derivation* stays CPU, palette *scheduling*
  stops being per-frame.
- **Dangly moves into the merge kernel:** the spring is solved where its output
  is consumed, deleting 789 calls/frame and the double-buffered upload.
  Determinism note: fixed 1/60 step and per-vertex state in the arena keep
  capture runs reproducible.
- **Particles simulate on the GPU.** Emitters upload spawn-parameter records on
  change; a compute pass integrates particle state in a persistent arena and
  emits quads into the procedural range. **Determinism by the grass precedent —
  integer hash of `(emitter, particle, frame)` for stochastic decisions, no
  shared-generator ordering dependence** (a bug class already caught once). The
  emitter is the record, particles are GPU state. The per-tick `addParticles`
  re-registration dies.
- **`settleMeshTransform` dies.** The prev-transform latch becomes part of the
  transform write itself (the slot holds current and previous; the flush rotates
  them), deleting a per-mesh-per-frame walk.
- **Flare LOS becomes a GPU visibility query** — one ray per flare light against
  the TLAS/merged scene, feeding the flare's alpha. Coordinate with G9, which
  owns un-filtering `LensFlare` at admission; land whichever comes first, but
  **the CPU LOS walk does not survive both.**

*Proves itself:* each named zone at ~zero in Tracy with the GPU-side cost measured
by S0's timestamps; raster captures stay byte-identical for the
skin/settle/flare items (pure scheduling moves); dangly and particles are judged
on the isolation fixtures (`warp testbed grass|smoke`) plus distribution
comparison in traced mode; the scaling fixture stays flat.

## S4 — residency: stop rewriting the static world

The merge currently rewrites every vertex of the scene every frame — static
geometry included — because `prevPosition` lives per-vertex and everything is
published `Dynamic`. The scaffolding for better (`GpuSceneResidencyClass`,
`Region`) already exists, unused. *All still exactly so, 2026-08-09:*
`GpuSceneResidencyClass` at `include/reone/graphics/rendering/gpuscene.h:204`,
`Region` at `:301-308`, with the sole producer hardcoding one whole-scene
`Dynamic` region (`src/libs/graphics/rendering/gpuscene.cpp:490-492`) that **no
consumer reads**; every classification site publishes `Dynamic`
(`src/libs/scene/render/admission.cpp:332,494`); the merge dispatches over
`max(vertexCount, triangleCount)` threads (`gpuscene.cpp:443-445`).

- **Static/dynamic partition** of the merged buffer and the material table. The
  static region is written at module load and never touched; `prevPosition =
  position` there by construction. The merge dispatch covers dynamic ranges only.
- **First, decide what "static" is provable from** — the admission proof, **not
  the authored `staticObject` hint the code already distrusts.**
- **The per-element binary search goes:** with regions, ranges dispatch
  per-object-run, or a precomputed per-vertex object id replaces the search.
  Either kills the O(log N) per thread. *There are **three** of them, not one:*
  `findVertexObject` (`slang/scene_resolve.slang:199-214`), `findTriangleObject`
  (`:220-237`) and `findGrassRange` (`:239-251`) — the third arrived with the
  GPU-grass work after this step was written, and it scales the same way.
- **BLAS strategy is explicitly unchanged** until the AMD build path is measured:
  the single full rebuild stands (it is GPU time, and small); compaction lands
  here; **the static/dynamic BLAS split stays the documented escape hatch** if
  AMD's 7.4× Vulkan build gap is real on current drivers. *Still unchanged:*
  one BLAS with an opaque and a non-opaque geometry over the whole merged buffer,
  rebuilt every frame at `MODE_BUILD_KHR`
  (`src/libs/graphics/vulkan/tracingstructure.cpp:160-166`, recorded from
  `src/libs/graphics/rendering/rayquery.cpp:104-109`). No compaction query exists
  anywhere in the tree.

*Proves itself:* per-frame upload traffic ≈ dynamic set only (measured, not
asserted); merge GPU cost drops in proportion to the static fraction (danm14ab:
45k of 86k triangles static); upload-hash equality and the shadow oracle hold; a
module transition rebuilds the static region exactly once.

## S5 — the RHI: logic files read as logic

The audit's numbers: 11,715 Vulkan-touching lines, of which ~4,300 are wrapper
tier and ~1,930 are ceremony inlined into the seven high-level files — five
hand-written image-transition helpers, ~368 descriptor-boilerplate lines, ~182
dynamic-rendering preambles, and passes that spend 41 lines preparing one
`vkCmdDraw(cmd, 3, 1, 0, 0)`. **The RHI's job is that the seven files stop
containing any of it. Its *internal* complexity is unconstrained by policy — it
is the one part of the tree licensed to be ugly.**

**Stage 1 — four builders, Vulkan-only, start whenever.** No design risk,
independent of every other track:

1. a layout-tracking image type — generalize what `VulkanGBuffer` already does;
   the five transition helpers and ~65 inline barrier sites collapse into it;
2. a `RenderPassScope` RAII type over `VkRenderingInfo` + viewport + scissor;
3. a pipeline builder covering compute and RT — the gap `VulkanPipelineCache`
   leaves, which is why `rayquery.cpp`, `gpuscene.cpp` and `nrddenoiser.cpp` each
   hand-roll layouts, pools and pipelines;
4. a descriptor-write builder.

Measured expectation: **~1,100 lines out for ~350 in.**

**Stage 2 — the seam, after V1c.** Once raster owns primary visibility in every
mode the frame shape stops moving, and the wrapper tier plus the builders
formalize into an interface layer: device, swapchain, queues, buffers, images,
pipelines, submission. **The gate is mechanical:**

    rg 'vk[A-Z]|Vk[A-Z]|vma[A-Z]' src include --glob '!**/graphics/vulkan/**'
    rg 'Vulkan|VK_[A-Z0-9_]+|ImGui_ImplVulkan' src include --glob '!**/graphics/vulkan/**'

**Both lines are the gate, and the second was added on 2026-08-05 because the
first is not sufficient.** It matches Vulkan *API symbols*; it does not match
Vulkan *type names*, because `VulkanImage` begins `Vu`, not `Vk`. Nor does it
match `VK_API_VERSION_1_3` or `ImGui_ImplVulkan_*`. Two things passed the
one-line gate while being full of Vulkan: `engine.cpp`, which held eleven
Vulkan-ish tokens of which exactly one matched, and the five relocated clients,
which owned `unique_ptr<VulkanGBuffer>` members and included backend headers
from public ones. **A file can be entirely dependent on the backend and read as
clean under the first line alone.**

The gate was originally written as two greps scoped to `src/libs/graphics`. That
form was retired on 2026-08-05: the first passed *vacuously* — no file under
`src/libs/graphics` outside `vulkan/` has ever contained a Vulkan token — and
the second ("only RHI files may match") named a file set nobody had defined, so
all 21 files matched it. Scoped repo-wide instead, the gate has real content.
Measured the day it was rewritten, the entire external surface was six sites:

- `src/apps/engine/engine.cpp` — a `VkFormat`, a `VkRenderingInfo` preamble and
  `vkCmdBeginRendering`/`vkCmdEndRendering`, all of which S5 stage 1's
  `RenderPassScope` absorbs;
- `src/libs/scene/render/pipeline/vulkan.cpp` — a `VkCommandBuffer` in the
  `mergeGeometry` override signature.

**The API surface is therefore not what makes stage 2 large — the five client
files' internals are.** A second count taken at the same time: 85 occurrences of
13 `Vulkan`-prefixed *type names* outside the backend, which stage 3 removes.

`IRenderer` sheds its Vulkan leak (`begin2DRendering` exists only to scope
dynamic rendering — the RHI owns that scope). Slang runtime (S1) supplies the
shader half of the seam, so **pipeline creation takes source + schema, not SPIR-V
blobs + hand-declared layouts.**

*Proves itself:* per stage, captures byte-identical (these are pure refactors);
the grep gate empty; the seven core files' line counts recorded before/after
against the audit's ~5,000 → ~3,300 estimate — **a number to report, not a bar to
force.**

**Stage 3 — the clients leave the folder.** Added 2026-08-05 (STR-028..030).
Once a class no longer names Vulkan it has no business living in `vulkan/`, and
the layout it should move to already half-exists: `include/reone/graphics/`
holds `gpuscene.h` and `rayquery.h` as backend-free data — `InstanceMaterial`,
`MergedVertex`, `SceneObject`, `GpuSceneUpload`, `RayQuerySubmission` — and
`renderer.h`, `renderer2d.h`, `pbrtextures.h` as interfaces. The data/interface
line was drawn and then not followed through; stage 3 finishes it.

Five files move to `src/libs/graphics/`, losing the prefix: `gpuscene` (572),
`scenepipeline` (1300), `rayquery` (1539), `pbrtextures` (357), `renderer2d`
(252). `vulkan/` drops from 9,398 to 5,378 `.cpp` lines and holds nothing but
the RHI.

**This is what makes the stage-2 gate self-enforcing.** The boundary stops being
a file allowlist somebody has to maintain and becomes a directory: the RHI *is*
`vulkan/`, defined by what remains after the clients leave.

Two files do **not** move, and the reason is worth stating because it bounds the
RHI's ambition: `nrddenoiser` (502) and `fsrupscaler` (152) are vendor SDK
integrations that take native Vulkan handles by construction. An RHI cannot
express them without handing the handle straight back through, so they stay as
vendor bindings inside the backend. `renderer` stays too — it *is* the RHI's
face.

**The naming rule (set 2026-08-05, and it is the acceptance test for stage 3):
no `Vulkan` identifier may appear outside `vulkan/`.** Not the prefix on a type,
not a parameter name, not a file name. Two shapes satisfy it:

- **A class that is genuinely backend-free has no Vulkan counterpart at all.**
  `VulkanGpuScene` does not become `GpuScene` *alongside* something Vulkan — it
  becomes `GpuScene`, full stop, and no `VulkanGpuScene` exists. This is the
  case for all five relocated clients once they are on the RHI.
- **A class that must stay backend-specific is split**: an API-independent parent
  that the rest of the engine names and holds, and a `VulkanSomething` child
  inside `vulkan/` that is the object actually constructed. Callers name only the
  parent.

The parent's *prefix* is free so long as the name is API-independent (settled
2026-08-05). So the existing `I`-for-interface convention in `AGENTS.md` stands
where the parent is purely abstract — `IPBRTextures` ← `VulkanPBRTextures` — and
a concrete backend-free class simply takes the plain name, `GpuScene`. What is
forbidden is a name that says which API it is, wherever it appears outside
`vulkan/`.

**The parents carry API-neutral *types*, not just API-neutral names** (settled
2026-08-05, after a first attempt got this wrong). A parent whose signature
still says `VkFormat`, `VkImageView` or `VkDescriptorSet` is a Vulkan interface
wearing an `I`, and it makes the gate *worse*: the first cut of step 6a
introduced three such parents under `include/reone/graphics/` and took the
Vulkan-token count outside `vulkan/` **from 2 sites to 16**. The seam therefore
needs neutral types — a `Format` enum, type-safe opaque handles, and whatever
command-buffer form the clients require — with the translation to Vulkan living
inside `vulkan/`.

This is knowingly the more expensive of the two options. It buys the thing the
whole stage exists for: with neutral types the five clients become genuinely
backend-free and can physically leave `vulkan/` (STR-028). Names-only would have
been a smaller seam, but the clients could never move, and STR-028 would collapse
into a rename. **"Minimise the RHI" still governs *within* this choice** — a
neutral type earns its place by a client needing it, never by symmetry with one
that does.

**The seam expresses intent, not Vulkan calls** (settled 2026-08-05). The
default failure mode of an RHI is a call-per-call mirror — an `IBuffer` with the
shape of `vkCmdCopyBuffer`, an `ICommandBuffer` with the shape of
`vkCmdPipelineBarrier2`. That is the worst of both: it is as large as Vulkan and
buys nothing, because a client written against it still has to think in Vulkan.
Prefer the operation the client actually means. "Upload this data and make it
visible to the next pass" is one seam call; the barrier struct, the stage masks
and the access flags are the backend's business.

**This matters most for ray tracing.** `rayquery` names 26 distinct Vulkan
types, nearly all of them acceleration-structure and ray-query machinery.
Wrapping them one for one would roughly double the seam for one client.
**Acceleration-structure construction and ray traversal are both abstractable at
the level of what they mean** — build a structure over this geometry; trace
these rays against it — and that is the form to aim for. A smaller, higher-level
seam is also the *cheaper* one to satisfy, since the alternative is a neutral
mirror of every `VkAccelerationStructure*` type.

This supersedes an earlier reading of stage 3 that proposed *collapsing*
`IPBRTextures` and `I2DRenderer` on the grounds that one implementation needs no
interface. **That was wrong for this codebase**: the parent is what keeps the
Vulkan name out of the caller's vocabulary, which is the whole point of the
stage. Parents stay whether or not a second backend ever exists — the
justification is naming discipline, not portability, consistent with "not the
goal: a second graphics API" above.

*Proves itself:* the stage-2 grep empty with `vulkan/` as the only exclusion;
`rg 'Vulkan' src include --glob '!**/vulkan/**'` empty — **this is the stage-3
gate**; captures byte-identical, since every step here is a move or a rename.

## S5 as built — 2026-08-05

The track is complete. Both gate lines are zero:

    rg 'vk[A-Z]|Vk[A-Z]|vma[A-Z]'       src include --glob '!**/graphics/vulkan/**'
    rg 'VK_[A-Z0-9_]+|ImGui_ImplVulkan' src include --glob '!**/graphics/vulkan/**'

and no Vulkan **type name** survives outside the backend either. All five
clients hold zero Vulkan tokens: `GpuScene`, `Renderer2D`, `PBRTextures`,
`ScenePipeline`, `RayQuery`.

**What the estimate got right and wrong.** Stage 1 predicted ~1,100 lines out
for ~350 in; actual was 1,495 out for 1,119 in, so the deletion side beat it and
the addition side ran high. The seven high-level files were expected to fall
from ~5,000 to ~3,300; instead they left `vulkan/` entirely, which was not the
plan when the estimate was written.

**Two premises did not survive contact.** Stage 2 was gated on V1c (TRC-020) so
the frame shape would stop moving; it was built without V1c and nothing moved,
so that gate was a stability preference rather than a dependency. And the seam
was expected to be the large part; the measured API surface outside the backend
was **six sites**, while the client internals were the work.

**The shape that mattered was intent, not translation.** `RayQuery` began with
27 distinct Vulkan types, 16 of them acceleration-structure machinery. Mirrored
one-for-one it would have roughly doubled the seam for one client. Expressed as
what it means — *build a structure over this geometry*, *trace these rays
against it*, *give me the tracing pipeline I need* — the types stayed inside
`vulkan/` and the seam did not grow. `gpuscene` is the smaller demonstration:
four exposed barriers became one `merge()`, and a `VkBufferUsageFlags` bitfield
became `initHostVisibleStorage(size)`.

**The RHI is 18 parent interfaces** under `include/reone/graphics/`, with
`Vulkan*` children inside `vulkan/`, which holds 7,457 `.cpp` lines: the RHI,
two vendor bindings that take native handles by construction (`nrddenoiser`,
`fsrupscaler`), and the renderer that is the RHI's own face.

## S6 — deletions the track leaves behind

Trailing cleanup, each unblocked by an earlier step; none worth its own session
until its blocker lands. **None has happened, verified 2026-08-09 — correctly,
since neither V5 nor S2 has landed.** `VulkanMesh`
(`include/reone/graphics/vulkan/mesh.h:43`), `acquireTextureSet`
(`include/reone/graphics/vulkan/descriptors.h:143,154`) and `resetFrame()`
(`include/reone/scene/gpuscene.h:175`, called from
`src/libs/scene/graph.cpp:550,637`) all still exist. Note `VulkanMesh`'s 14
references are all *inside* `vulkan/`, so S5's naming gate is clean with the type
still present — **the gate never promised deletion, only relocation.**

- the legacy per-mesh path: `VulkanMesh`, per-object `LocalUniforms`,
  `BoneUniforms`/`DanglyUniforms` blocks and their uniform-ring wiring — last
  consumer is the runtime sky bake, so this trails **V5**;
- the per-draw texture-set path (`acquireTextureSet`) once 2D rides the RHI;
- the `Registered*` types and `scene::GpuScene`'s frame-phase surface (S2's
  mechanical remainder);
- `resetFrame()` and other confirmed no-ops.

*Proves itself:* the greps return nothing, `--target tests` passes, and the
acceptance capture set is unchanged.

## Decisions taken in the structural plan — review these

1. **Slang runtime is promoted P3 → track-blocking** (S1 before S2+). The schema
   single-source is the reason; the iteration loop is the bonus. — *acted on;
   S1 is done and S2 has not started.*
2. **Sync model: per-FIF table copies + change-log replay**, not versioned slots
   in one copy. — *still a decision; unbuilt.*
3. **Animation evaluation stays CPU; palettes become event-driven outputs.** —
   *half true by default: evaluation is CPU, palettes are not yet event-driven.*
4. **Particle simulation moves to GPU with integer-hash determinism**; the
   emitter is the registered record. — *unbuilt.*
5. **BLAS single-full-rebuild stands** until the AMD measurement; the split is
   the documented escape, not the default. — *holds; no measurement taken.*
6. **RHI in two stages**, seam formalized only after V1c; stage 1 may start
   immediately. — **the V1c gate was tested and found not to be one.** Stage 2
   was built before V1c and nothing moved under it; stages 1–3 are all done.
7. **S2 waits for G8** — the last vocabulary-growing step — rather than racing
   it. — *G8's draw has landed and its sort was rejected, so the vocabulary is
   quiet: this gate is satisfied and S2 is unblocked.*

## Open questions, owned by a step

- What the 1.25 ms inside `SceneGraph::update` actually is → **S0**; may reorder
  S3, and decides whether leaf-prep survives the inversion or becomes GPU-driven
  draw generation (a possible S4 extension, not assumed).
- Whether G8's CPU transparency sort ever outgrows its budget → the scaling
  fixture watches it; the Enderton-style stochastic-depth fallback is the named
  escape. **Moot for now: there is no CPU transparency sort.** The question
  revives only if the submission-order decision is reversed.
- Whether the RHI seam should also carry the 2D renderer or leave it as a direct
  client → decide in S5 stage 2 from what `renderer2d.cpp` looks like after stage
  1 (it is only 7% Vulkan tokens today). **Answered by stage 3: it carries it.**
  `Renderer2D` is one of the five relocated clients and holds zero Vulkan tokens.

---

# Part 6 — The reference engines, and what they say we get wrong

Surveyed 2026-08-04 from `C:\Development\odessey` — **read-only reference
checkouts, not dependencies**:

- **xoreos** — C++ Aurora/Odyssey reimplementation. Reproduces the original's
  *fixed-function GL state machine* (actual `glTexGeni`/`glBlendFunc` calls), so
  it is the authority on **how the original sampled and blended**.
- **KotOR.js** — TypeScript/Three.js, the most feature-complete. Authority on
  **light budgets, gating policy and MDL controller semantics**.
- **kvp-main** — a Vulkan wrapper over the *retail binary*, so it observes the
  real draw stream. Authority on **blend states the game actually sets**, and the
  only source for **modern PBR over these assets**.

**Where they disagree, xoreos wins on GL semantics** (it emulates the state
machine); **KotOR.js wins on gameplay-side policy**; **kvp-main wins on anything
observed from the shipping game.**

## Confirmed correct — do not "fix" these

- **The env-map formula.** `color += env * (1 - diffuse.a)` — additive, no
  Fresnel, no lerp, applied *after* the lightmap multiply and *not* attenuated by
  it. All three agree (xoreos `shaderbuilder.cpp:609`, KotOR.js
  `ShaderOdysseyModel.ts:423`, kvp-main sees `ONE_MINUS_DST_ALPHA/ONE` in the
  retail stream).
- ~~**`if (alpha == 0) discard`** is exactly the retail
  `glAlphaFunc(GL_GREATER, 0)`.~~ **Corrected 2026-08-05, and it does not belong
  in this section.** xoreos sets `glAlphaFunc(GL_GREATER, 0.1f)` once globally
  (`graphics/graphics.cpp:440-441`) and only disables it around the env-map pass
  — **the reference value is 0.1, not 0.** Since our raster tests at 0.5, this is
  an **open difference** rather than a confirmed match; it is
  `retro-rendering-differences.md` row 19, and the G8 passage above is corrected
  with it.
- **Lightmap multiplies** the diffuse result — the retail stream's
  `DST_COLOR/ZERO` pass.
- **The separate emissive/hilights buffer** shape kvp-main independently
  converged on.

## Corrections, ranked, with status

1. **The 2D `EnvMap` is a GL sphere map, not equirectangular.** We computed
   `atan2/asin`; the original is
   `m = 2·√(rx²+ry²+(rz+1)²); uv = (rx/m+0.5, ry/m+0.5)` on the **eye-space**
   reflection (xoreos `shaderbuilder.cpp:376`). We also routed 2D env maps
   through the 128² prefiltered IBL array instead of sampling the authored
   texture. **This is the path most KOTOR metal uses. Fixed in `cd3fbec2`**, in
   both resolves and in `pbr_ibl`'s convolution.
2. **The reflection vector is eye-space**, not world-space, for both cube and
   sphere paths — hence the original's camera-locked reflection. Computed
   per-vertex from the *geometric* normal; a deferred resolve can only manage
   per-pixel from the G-buffer normal, **which is an accepted divergence. Fixed
   in `cd3fbec2`**, resolves and kept forward shaders together.
3. **Implicit-LOD cube sampling in a fullscreen resolve** slides the mip, because
   the reflection's derivatives come from 8-bit G-buffer normals. **Fixed in
   `cd3fbec2`** by pinning the authored mirror to explicit LOD 0, **which leaves
   minified metal unfiltered** — see the open item in Part 1.
4. **`ShadowOpacity` is authored per area and we throw it away** — parsed at
   `resource/parser/gff/are.cpp:369`, unused. **Tried and rejected in
   `2f00b5f5`:** it is a BYTE holding exactly two values across the retail set,
   50 in 22 modules and 205 in 74, and neither reading of that pair is what
   either group wants, so it is logged and drives nothing while shadow strength
   is a chosen 0.5. **The survey was right about the field and wrong about the
   conclusion, which is the useful shape of that finding: a parsed-and-ignored
   value is worth looking at, not worth assuming is a parameter.**
   `SunShadows`/`MoonShadows` are still parsed and ignored. *Since then the
   chosen strength became an override dial rather than a constant:*
   `GraphicsOptions::shadowOpacity` defaults to `-1` meaning "use the authored
   byte", and its own comment calls itself "the knob for judging how that byte
   should map to a strength" (`include/reone/graphics/options.h:671-676`). **The
   rejection stands; the instrument for revisiting it now exists.**
5. **Split the shadow term in two** — a BRDF factor and an ambient/IBL factor.
   kvp-main's `ShadowResult { factor; iblFactor; }` exists for exactly the
   double-darkening problem the cascade work hit, and its shipped floors let
   skylight fall to 27–36%, far below our 25% *cap*. **Done in `0fb05120`**:
   `getShadow` returns both factors and the 25% cap is gone.
6. **Self-illum is additive** — "vanilla adds `GL_EMISSION` on top of the
   texture" (kvp-main `MaterialSystem.cpp:196`). **We modulate. Open.**
   **Resolved by splitting it across modes, which the entry did not anticipate.**
   Retro is now additive and says why in the source: *"self-illum stays an
   additive term the albedo never modulates, because the original drew it as a
   separate additive blend pass"* (`slang/retro_resolve.slang:225-229`). PBR
   still modulates, **deliberately** — *"a coefficient on the light reaching the
   surface, not a colour added on top of it. Adding it instead saturates anything
   fully self-illuminated — a sky — to white"* (`slang/pbr_resolve.slang:497-500`).
   That is the intent-versus-limitation test from Part 1 applied: the additive
   blend is intent and binds retro; the white-out it causes on a modern lighting
   integral is PBR's problem to solve differently.
7. **Additive with no alpha channel uses `SRC_COLOR/ONE`**, not `SRC_ALPHA/ONE`
   (xoreos `modelnode.cpp:684`). **Open.** *Still open 2026-08-09, and do not
   confuse it with `852b77d10`* — that commit fixed where the display **encode**
   sits relative to the premultiply for additive surfaces (halo 124→118, core
   246→235 on a saber blade), which is a different defect in the same pixels. The
   blend-factor question is untouched.
8. **Keep submission order for non-opaque draws** — kvp-main's replay of the real
   game reorders *only* true-opaque depth-writing geometry. **This lands on G8:**
   if the remap sort reorders transparents, expect regressions the original did
   not have. **Adopted — and it decided G8 rather than qualifying it.** The
   blended draw keeps admission order and no remap was built; the reasoning is
   restated at `src/libs/graphics/rendering/scenepipeline.cpp:546` and
   `slang/scene_draw.slang:394-396`.
9. **The light budget was 8 global and 3 per model** (`videoquality.2da`,
   KotOR.js `LightManager.ts:24`). We allow 32. **More lights than the artists
   authored for will not look better, it will look wrong in ways that are hard to
   attribute.** **Moved, not closed — `58c55eaab`.** The 32 was `kMaxLights`
   doing two jobs at once: sizing the uniform block and deciding how many lights
   a frame carries. Those are separated now — the ceiling is 64
   (`include/reone/graphics/types.h:46`, mirrored in `slang/uniforms.slang`) and
   the count is a live option, `maxLights`, defaulting to **48**
   (`include/reone/graphics/options.h:199`). **So the number this entry objects
   to went up, and is now a dial rather than an array bound** — which is what
   makes the objection testable: four lights against forty-eight is a visible
   difference on Dantooine and in the enclave, so the reference's 8 is one
   setting away and can be captured against. Nothing has decided what retro's
   value should be.

## What none of them can tell us

**Original shadows.** xoreos renders none, KotOR.js built them and disabled them,
kvp-main invented modern cascades. The only surviving statement is that the
original cast creature shadows **from the skeleton, not the render mesh**
(KotOR.js `OdysseyModel3D.ts:1230`), and that it shipped both a shadows and a
*soft* shadows toggle. **Our cascades are a modern reconstruction with no
reference to check against.**

## For the PBR texture question, when it comes

kvp-main is the only prior art for PBR over assets that author no roughness or
metalness, and **its conclusion is chastening:** after building an HSV material
classifier, it **clamps metalness to 0.1** — "KotOR's gray textures are painted,
not metal" — and ships `metallicSensitivity = 0.038` against a default of 1.0,
with roughness pinned to 0.07–0.24. It also **omits the `1/π` diffuse
normalisation deliberately**, because art authored for fixed-function goes too
dark with it. **The lesson is not the numbers; it is that deriving PBR parameters
from diffuse textures mostly needs to be turned *off*.**

## Settled, so it is not re-litigated

The sky's offline half is finished and committed. `skybake` renders sky shells
into cubemaps **by casting rays from inside the shell** — mesh count and shape
stop mattering, tiling falls out of the hit UV, orientation falls out of the ray
direction, and a missing floor is a ray that hits nothing, **black by
construction.** Per-game curated configs live in `override/k1` and `override/k2`
and are committed; the 79 baked assets are gitignored and regenerated from the
player's own install. Coverage: 56 of 117 K1 modules and 46 of 82 K2 modules name
a sky, every entry still marked `# review`. The evidence for why *runtime* sky
classification was abandoned — including the 117-module sweep that killed it —
lives in the backlog's sky entry.

*Re-counted 2026-08-09 and all four numbers hold: 117 and 82 module sections,
56 and 46 named skies, 0 `meshes =` keys in either file.* **Read "finished" here
narrowly** — it means the *approach* is settled and is not to be re-argued, not
that the tool is correct. V0 above lists two live defects in it, and the two
statements have been read against each other wrongly before.

# Part 7 — One rendering path (RAS-036, built 2026-08-26)

## The contract

One assembly, owned by the composite (`slang/composite.slang`), fed by whichever
provider shades the frame:

    final = noiseFree + (diffuse + directDiffuse) * diffFactor + specular * specFactor

- `noiseFree` — emission at the primary, sky on a miss, sky-class radiance; fog
  blend in `.a` (zero on terminators, so the sky is never fogged).
- `directDiffuse` — demodulated primary direct (`radiance*NdotL/pi`, no albedo),
  with the mode's OWN visibility: shadow maps in PBR, shadow rays traced.
- `diffuse` — demodulated indirect: `ao*lightmap*lightmapIntensity` plus ambient
  fill plus IBL irradiance in PBR; traced bounce radiance (denoised) traced.
- `specular` — demodulated: analytic GGX plus prefiltered IBL plus authored
  mirror, divided by `specFactor`, in PBR; traced specular in the tracer.
- `diffFactor`/`specFactor` — the surface model (`lib/surface_model.slang`
  `materialFactors`), identical across modes by construction: measured at
  correlation +1.0000, max difference 5e-4, between `pbr_channels` and the
  trace kernel over the same G-buffer.
- `viewZ` — the terminator sentinel (`kAssembleInfDistance`); a terminating
  pixel carries all radiance in `noiseFree` and the composite adds nothing.

## Ownership

`ScenePipeline` owns the fifteen channel images (double-buffered) and the
composite dispatch. The trace kernel binds them into its set 2 by name each
frame, exactly as it binds the G-buffer block; `pbr_channels` writes seven of
them through the resolve set's storage bindings 2..8 (partially bound — retro
declares none and is untouched by the layout growth). The tracer hands back
only what it alone can produce: NRD's denoised pair, whichever image settled
the direct channel, and the composite's push values. PBR hands back the raw
channels at zero jitter, which makes the composite's sampled reads the raw
texel at the pixel centre — the contract line exactly. Retro allocates no
channels and keeps its own resolve untouched; it is the preservation mode and
shares only `ResolvePushConstants` and the descriptor layouts.

## The rule table, as decided and built

| rule | decision | consequence, measured |
|---|---|---|
| PBR `min(1, light)` clamp before albedo | dropped | tat_m18ab +3.1% brighter; a sand glare patch gone |
| `materialDiffuse` tint on direct | dropped | subsumed by `diffFactor` |
| per-light `kD`, ambient-lobe Fresnel | dropped | the shared factors are the surface model |
| self-illum specular gate | dropped | glowing panels may take highlights |
| emission | tracer's rule: radiance into `noiseFree`, not albedo-multiplied, on `emissiveIntensity` | PBR default 1.0 preserves the authored level |
| sky | one rule on `skyIntensity` | per-mode default 1.0 raster / 2.5 traced |
| fog | shared: the kernel computes the per-surface blend into `noiseFree.a`, the composite applies colour and switch | one `--fog`, honoured by both modes |
| lightmap | one `lightmapIntensity` | per-mode default 1.0 PBR / 0.0 traced — the one documented divergence, now a default |
| parity | provider writes the tracer's shape | channels parity identical to the old resolve at 0.000% |

## Debug views

The radiance channels (8, 9, 11, 15) read the channel images in any mode that
fills them; channel 20 (PBR's re-derived direct) is deleted — 15 is both modes'
answer, each with its own visibility. Retro paints the not-available card off a
flag bit rather than sampling unbound stand-ins. Tracer-only remain 16–19: the
penumbra and the denoiser's own products.

## What stays per mode, deliberately

The occlusion answer (map or ray), the tracer's NEE selection and penumbra,
PBR's SSR tail (RAS-037 owns making it a provider), and each mode's dial
DEFAULTS — never the dials themselves, never the assembly.
