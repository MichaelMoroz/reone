# The record

What was tried, what was measured, and what went wrong. Extracted 2026-08-05 from
`doc/backlog.md`, `doc/renderer-registration-plan.md` and `doc/cleanup-plan.md` so that those three
can be deleted without losing the reasoning behind the tree. Nothing here is a task. Everything here
is a thing already paid for once.

---


> **Paths and line numbers cited below from before 2026-08-05 predate the S5
> relocation.** The render passes moved from `src/libs/graphics/vulkan/` to
> `src/libs/graphics/rendering/`, and much of `rayquery.cpp` became
> `vulkan/tracingpipeline.cpp` and `vulkan/tracingstructure.cpp`. The *findings*
> stand; the coordinates need re-deriving. See `DESIGN.md`, "S5 as built".

## 1. Postmortems

### 1.1 The traceStats atomics were ~95% of the traced frame

Landing importance-based light selection took five falsified theories, and the postmortem is
recorded because the real culprit had been billing this renderer since the first traced frame.

Ebon Hawk, 4 spp, timed as capture-to-frame-900 minus capture-to-frame-300 over 600 frames:

| what was measured | ms/frame |
|---|---:|
| before selection | 36.3 |
| selection walking the light buffer inside the sample loop | 61.1 |
| shadow rays disabled entirely | 61.5 |
| the struct copy's register pressure removed | 59.2 |
| the walk hoisted to a per-pixel weight table | 61.3 |
| pixel scope with multiplicity-deduplicated shading | 53.7 |
| scratch-resident weight array dropped, per-ray stats atomic batched | 42.8 |
| **all `traceStats` counters behind a debug flag, default off** | **1.68** |

The frame was **25x faster than every number ever measured for it**. The counters — unconditional
`InterlockedAdd`s on one 4-byte word from every thread, serialized by the hardware as global atomic
contention — were **~95% of "the cost of the path tracer"**, including the 36.3 ms baseline, and
their per-sample variant is precisely why the selection landing looked 25 ms slower.

Two standing consequences. **Every earlier cost figure for the traced frame was ~95% counter
serialization**, so no traced number predating this is usable. And a per-thread atomic on a single
word is a hazard class, not an incident: cheap to write, invisible in source review, and it prices
the feature it is measuring.

What selection actually became: one buffer walk per pixel totals the weights and feeds the ambient
flat term; each sample draws one random number against per-light intervals recomputed
bit-identically; a light picked by several samples is shaded and shadow-traced once with the
multiplicity folded into the unbiased estimator, at a warp-uniform light index. Shadow rays per
pixel: **1.8 at the Hawk, 2.7 in the 18-light cantina**.

The promotion of directional records (`position.w == 0`) to sampled suns shipped in the same walk —
no attenuation, ambient-only flag overridden, one shadow ray, `ptSunIntensity` dial — and **shipped
dead the first time**: the sky dome is opaque TLAS geometry between every surface and the far sun
origin, so every sun shadow ray committed on it. Measured contribution **+0.0000** while costing a
full-length traversal. Sky instances now carry TLAS instance-mask bit 2 (world is bit 1); camera and
bounce rays trace with the full mask, shadow rays with bit 1 only. **Sky is environment, never
occluder.**

### 1.2 Shader Execution Reordering does not work with the pinned compiler, and the win was noise

Recorded because everything about this failure looked like success. Shipped as `395a7afe`, reverted
as `b3fe1f3b`.

**vcpkg slangc 2026.7.1 — the compiler the build actually uses — accepts `ReorderThread`, warns that
it is upgrading the profile to include `spvShaderInvocationReorderNV`, exits 0, and emits the
capability with no `OpReorderThreadWithHintNV` in the module.** The Vulkan SDK's *older* slangc
2025.17.2 emits both from identical source and flags. Two slangc binaries on this machine disagree
and **the newer one is the broken one**, so testing with the wrong binary proves the opposite of the
truth.

The only thing that shipped was three extra validation VUIDs: `spirv-val` rejects capability 5388
because it does not know the extension, and the module declared a capability nothing used.

**The measured speedup was noise.** One unchanged build measures `ebo_m12aa` across **6.26–7.09 ms
over five samples** — a **12.2% spread** containing both the 6.98 "before" and the 6.20 "after".
Three samples either side could not see it.

**`spirv-dis` cannot be trusted on these modules.** It aborts at word 4 with `Invalid capability
operand: 5388`, so any disassembly-based check reads a truncated module and confirms whatever
absence it was testing for. Walking the instruction stream is what settled it.
`OpReorderThreadWithHintNV` is **5280**; **5279** is the HitObject form.

The design is not what failed. The raygen pipeline is already the right shape and the device reports
real reordering; this is blocked on the toolchain.

### 1.3 Point lights became spheres — the derivation, the wrong objective, and the numbers

Shipped as `085c5893`. Kept because the reasoning is the calibration's justification, and because
the fitted constant is worth being able to re-derive.

`slang/tracing/lighting.slang` had inherited two raster behaviours, one outright bug, and one hack of
our own — and they turned out to be a single fix:

- **A hard cutoff**, `lightDistance > light.radius * light.radius`, so a light stopped existing past
  a boundary, visible as a terminator on large surfaces lit by a small lamp.
- **Dimensionally wrong**: a length compared against an area, so the effective range was `radius²`.
  A faithful port of an Odyssey bug — the comment said as much — but it meant range scaled
  quadratically with a number never meant to be squared.
- **Falloff was not inverse-square**: `radius²/(radius+d)²` normalises to 1 at the source and decays
  softly. Odyssey's look, not physics.
- **The shadow cone had a fixed opening angle**, one global 8° for every light at every distance, so
  the emitter had neither a size nor a position in the softness calculation. The cone *sampling* was
  right — uniform in solid angle is the standard way to sample a sphere — but a constant half-angle
  meant penumbra never sharpened with range, and 8° is enormous (a 10 cm bulb at 3 m subtends ~2°),
  so every shadow in the game was uniformly over-soft.

**The unification.** Ω = 2π(1 − cos θ) with θ = asin(saturate(R/d)) is simultaneously the falloff and
the shadow cone, and inverse-square falls out of its far field with correct saturation up close and
no singularity at d = 0 — Ω caps at 2π.

**R is a fraction of the influence radius** (`ptPointEmitterRatio`, default 0.2). The influence
radius cannot be used directly: `graph.cpp` culls at `radius + 64` and `light.cpp` promotes past 100
to a directional sun, so it is a range, and using it would make every lamp a room-sized ball. KotOR
authored no emitter size, hence a dial.

**Dividing by the emitter's own projected solid angle makes that dial brightness-neutral.** It reads
the authored colour as an intensity rather than a radiance, so the far field stays at
`budget·radius²/d²` however large the emitter — verified invariant to three decimals from ratio 0.05
to 0.4 — and the dial grades penumbra width and near-field saturation only.

**The scale was fitted on paper, and closes exactly.** Substituting x = d/radius makes the problem
scale-free: Odyssey's curve collapses to 1/(1+x)² and Ω to a function of x and the ratio alone, so
one constant serves every light at every authored radius. The constant preserving light delivered
inside the bounding radius is

> ∫₀¹ x²/(1+x)² dx = **3/2 − 2ln2 ≈ 0.1137056**

Exact as the emitter shrinks to a point, ~1% off at ratio 0.2.

**A free two-parameter fit was tried first and is instructive**: it returns ratio ≈ 0.59 with **29%
rms error**, because reproducing Odyssey's *flat near field* requires an emitter nearly as large as
the influence radius. **Fitting the shape is the wrong objective — the shape is the bug. Only the
scale should be fitted.**

**Measured**, Ebon Hawk `ebo_m12aa` frame 310, against the same frame before:

| | before | after |
|---|---|---|
| mean luma | 0.13408 | 0.13602 |
| p99 luma | 0.51930 | 0.56772 |
| blown pixels | 0.016% | 0.016% |
| ms/frame | 8.90 | 10.06 |

55% of pixels moved, in the predicted direction: the 64–128 band gains 6–9 levels, the 32–64
midtones give up half a level. Visually the ceiling lamp stops being a broad flat wash and becomes a
hotspot with a locatable source. Sun-lit Dantooine barely moves, 0.44378 → 0.44369.

**The cost is the feature.** +1.16 ms is lights that used to vanish now lighting and shadowing;
splitting the attenuation out of `SphereLight`, so the selection table does not compute an `asin` it
discards, recovered a further 0.21 ms.

**An earlier revision of this section claimed** `ptPointAngularSize` "treats these as sphere lights
for shadow sampling". It did not; it was a constant, and that was the whole point.

### 1.4 The toolkit preview: four hypotheses killed from screenshots before anyone made it scriptable

Fixed by `a89c6eeb` and `44e11344`. **Never a rendering bug**: the camera sat at a fixed 8 units for
every model, so large ones enclosed it and small ones were specks — a **24× range in apparent size**.
Pre-existing since `44472f0e`, unrelated to the Vulkan port and unrelated to retro-on-Vulkan.

Worth remembering *how* it was found: **four hypotheses were proposed and killed from screenshots
before anyone made the preview scriptable, after which one capture settled it.** The blocker was that
the toolkit took no arguments, so the failing draw could be reached only by a person clicking. The
`--open`/`--capture` harness is the durable part — it opens a model and frames it from the command
line, so RenderDoc and the capture harness can both reach the draw.

### 1.5 Reasoning from a ratio of call counts is how that hour was lost

Before the registry, `cullModels` computed visibility once per model root per frame and culled
subtrees wholesale; afterwards `isCulled` runs per entry per pass, **~9000 times against ~200**. A
one-slot cache removed those calls and **moved frame time by nothing**, because an AABB-frustum test
costs tens of nanoseconds.

The rule that came out of it: **attribute cost to something measured, not to something that merely
happens often.** Restated later in the cleanup plan as the reason culling is not what is being given
up by a mega-draw — at 86k triangles the GPU does not need the help, and CPU overhead (per-draw
material binding, descriptor churn, the copy itself) is raster's actual problem.

### 1.6 NRD accumulation is nondeterministic, and it was masquerading as tracer noise

Isolated 2026-07-31 on `danm13` frame 310, `--dev 0`. Mean luminance over repeated runs:

| configuration | sd |
|---|---:|
| denoiser and FSR both off | 0.00063 |
| FSR alone on | 0.00051 |
| **NRD alone on** | **0.14043** (one run 0.35 clear of the rest) |
| both on | 0.01283, and bistable |

So NRD is the whole of it, FSR contributes nothing, and **FSR partly masks NRD's excursions**. The
tracer itself is deterministic in energy — six decimals — while per-pixel variation of 6–16% remains
and is genuine MC plus acceleration-structure ordering.

Two consequences. **Measurement:** traced comparisons should run `--ptdenoise 0`, which turns a
16-runs-a-side statistical exercise back into a tight number. **Correctness:** REBLUR producing a
different result from identical input is a real temporal-stability bug, not a harness artifact.

The obvious explanation was something fed to NRD that is not frame-indexed, and **all three named
suspects check out negative**: `common.frameIndex = frameNumber`
(`graphics/vulkan/nrddenoiser.cpp:357`) is frame-indexed, not wall-clock; the history reset is a
deterministic 20-unit camera-distance test (`:328-331`); and nothing timing-derived reaches
`SetCommonSettings` at all (`:334-361`). That leaves NRD-internal state or resource reuse across
frames.

### 1.7 The raster nondeterminism was the FPS readout forging its own diffs

Raster capture runs are **bit-exact**. Every differing pixel sat in one **153×13 box** in the
top-right — the frame-time text at `editor.cpp:1592` ("234.4 FPS 4.27 ms" vs "241.3 FPS 4.14 ms");
`:1583-1588` is the accumulation loop above it, not the draw.

Mask raw rows 1040+ / cols 1600+ and three runs each of `danm14ab`, `ebo_m12aa` and `danm13`, in both
PBR and retro, are **byte-identical: six groups, six hashes, no exceptions.**

Path tracing stays nondeterministic afterwards at **8–64% of pixels**, from acceleration-structure
build order feeding an order-dependent accumulation loop; that part is real and needs
distribution-vs-distribution comparison. **This bug is behind several false regression reports during
the GL removal.** The diagnostics skill's "raster is byte-identical" was right all along, and its
0.02% bound for tracing is the part that was wrong.

### 1.8 Particle admission was never unobserved — the measurement was

The earlier "particle admission unobserved" finding was a **measurement failure rather than a code
failure**. It was working: the TLAS line counts admitted grass clusters, particles and billboards.
**55 saber-spark quads cannot move a whole-frame mean luminance by more than 3e-6.**

The lesson, not the numbers: **whole-frame statistics cannot see small localized content**, and a
count answered in one run what six module probes could not.

The counts themselves have been superseded twice — 1482 clusters and 55 particles on `danm14ab`
originally, then 2956 clusters and **232–237** particles (`9ed2c573`), then the cluster pool made to
follow the density dial (`c343f273`, 1.0 → 2903).

### 1.9 The black-block particles, from a careless `git checkout --`

**The classification happens at admission**, in `classifyParticles` (`rayquery.cpp`): additive
blending takes the unlit path, everything else is given blended coverage. **Lose that else branch and
every particle commits unconditionally as a black block** — which happened once, from a careless
`git checkout --`, and read convincingly as a shading bug.

Alongside it, the other detail that is not guessable from the shader: **premultiplied textures carry
no usable alpha channel.** Coverage lives in the luma and has to be recovered exactly as
`slang/particles.slang:64-71` already does for raster; a tracer reading `.a` on one of those samples
zero and the surface disappears.

### 1.10 Grass shadows: sub-pixel at distance, not absent

Phase D closed with grass, dangly, saber and particles admitted and seen correctly. Grass shadows
work — **the blades were sub-pixel at distance, not absent**, which took a purpose-built test module
to establish and cost an afternoon of arguing with a screenshot first. Everything built alongside the
argument — the free camera, the TLAS content counts, the traced G-buffer dump, the testbed module —
outlives the defect.

### 1.11 The `scene empty` fixture was not broken

An earlier entry claimed `scene empty` was broken because the snapshot logged `entries=0` and
captures were black. **Both are correct behaviour** — an empty scene has no admitted objects, the sky
is baked at startup rather than being one, and a scene with no lights renders black. The commands
work: `warp` + `scene empty` + `grass` admitted the full pool at the fixture's original 80-unit
camera. **Fixtures need a light, or every capture is black by construction.**

Two real problems survived that correction. Raster culls grass beyond `kMaxClusterDistance` = 32
(`node/grass.cpp:48`) while the tracer admits the whole pool, so at 80 units raster draws nothing and
any comparison is meaningless. And moving the camera to 12 units produced **zero** clusters — cluster
placement appears to depend on camera proximity to the surface in a way that fails at close range.

### 1.12 `toolkit.exe` had failed to link since `b991a197`

Done 2026-07-30. It **trained everyone to ignore a red build, which is how `tests` stayed broken
across four commits.** A broken target is not a local cost.

### 1.13 TOOL-001's rate measured against a clean tree: 0 of 4, not 1 of 3

Measured 2026-08-05 while accepting the S5 stage-1 builders. The suite aborts with exit **127** part
way through `SlangShaderCompiler`, producing no gtest summary; when it does survive far enough to
report, the failures are confined to that suite's five tests.

The number that matters is the **control**. Suspecting the RHI work had caused it, the changes were
stashed (`git stash push -u -- src include`), `tests` rebuilt from the committed tree, and the suite
run four times: **4 of 4 aborted**. Restored, the same suite gave one clean `362/0`, one `361/1`, and
two aborts across four runs. **The instability is entirely pre-existing and the working tree ran
better than the control** — a reminder that "the tests are flaky" is a claim with a cheap experiment
attached, and that attributing a flake to the change in front of you is the default error.

Two consequences. TOOL-001's row still reads "tests still 1/3"; on this machine and this branch the
committed tree is closer to 0/4, so the row understates it. And **a single green run of this suite is
not evidence** — it is one sample from a distribution that includes a 4-of-4 abort. Any step that
cites `tests.exe` as acceptance needs a run count, or it is citing luck. Ratios only: the absolute
rate is one machine's, and the point is the comparison against the control, not the figure.

### 1.14 The uniform ABI guard names the field — in the test, not in the engine

Found 2026-08-05 verifying TOOL-007, which folded the uniform blocks onto the
runtime Slang reflection path and deleted `uniformgen` and its committed
`uniformlayout.generated.h`.

The acceptance criterion S1 set was "a deliberately mis-sized C++ mirror aborts
startup with the field named". Tested by swapping two adjacent `float4`s —
`GlobalUniforms::cameraPosition` and `worldAmbientColor`, same size, offsets
exchanged, which is precisely the case a size or stride comparison cannot catch.

**The check is exact.** The test reports

    Slang schema mismatch: GlobalUniforms::cameraPosition is at 272 in C++, 256 in Slang

naming the field and both offsets. **The engine, given the same broken mirror,
segfaults during startup with an empty log.** `main.cpp:63` catches
`std::exception` around `engine.init()` and logs "Engine failure: …", so the
diagnostic exists and never arrives; something faults on the throw path out of
`VulkanRenderer::init` before it does.

**Pre-existing, and TOOL-007 did not cause it** — the call site is unchanged,
only renamed (`validateSceneSchema` → `validateSchemas`), so a scene-schema
mismatch has always failed this way. Filed as TOOL-024.

Worth stating because the value claimed for the runtime path was "fails at
startup naming the field, not at 2 a.m. naming nothing", and in the engine it
currently fails at startup naming nothing. The guard is real, but **the test
suite is where it speaks**, and that is where a mirror edit should be caught
anyway. A second-order lesson: two mis-size experiments disagreed — growing the
struct and swapping two fields both crashed the engine, while only the test
distinguished them — so an acceptance check that only ever runs one path can
report a mechanism works when its user-facing half does not.

### 1.15 The offline uniform generator had a coverage hole

`uniformlayout.generated.h` asserted 78 fields; runtime reflection checks 79.
The missing one was `LocalUniforms::iblRoughness`. The generated artifact had
been the authority on uniform layout and was silently not covering a field —
which is an argument for the mechanism that derives coverage from reflection at
run time over one that bakes a list at generation time.

### 1.16 An empty generated NRD shader blob, mistaken twice for an RHI regression

2026-08-05, during S5 stage 2. Path tracing died at startup with
`NRD: shader module creation failed` (`nrddenoiser.cpp:203`), raster unaffected.
It appeared during step 6a when `VulkanImage` gained a virtual base, went away
when unrelated wrapper types were deleted, then returned in step 6b-i when
`VulkanBuffer` gained one.

That looked like a pattern, and it was written up as one: *NRD breaks when a
resource type it holds acquires a virtual base.* **It was a coincidence.**

The real cause: `vkCreateShaderModule` returned `VK_ERROR_OUT_OF_HOST_MEMORY`,
but not from allocator exhaustion — NRD pipeline 12 (`Clear.cs.hlsl|FLOAT=1`)
was handed `codeSize == 0` and `pCode == nullptr`. Its generated embedded-SPIR-V
header in the build tree was a **67-byte empty `NVSP` blob with no
permutations**, which CMake accepted as up to date. Forcing a ShaderMake
regeneration compiled all 159 NRD shaders and produced a 3,107-byte blob with
both permutations; NRD then built 14 pipelines and path tracing completed. **No
source change was required, in either occurrence.**

Three lessons, in order of how much they cost:

**The `VkResult` was being thrown away.** The call site tested
`!= VK_SUCCESS` and threw a string. One integer separated "the SPIR-V is
invalid" from "the allocator is exhausted", and discarding it cost two rounds of
theorising. A Vulkan call that can fail for unrelated reasons should report
which one.

**A control test that requires a rebuild does not isolate a source change.**
The stash-and-rebuild control (the technique that correctly settled TOOL-001 in
1.13) *misled* here: stashing and rebuilding also regenerated the shader
artifact, so "it works at HEAD" read as "the working tree caused it" when both
statements were about the build tree, not the source. **When the artifact under
suspicion is generated, hold the source fixed and vary only the artifact.**

**Two occurrences are not a pattern when the correlate is "I rebuilt".** Both
appearances followed a structural change, but every structural change was also a
rebuild, and the rebuild is what touched the stale blob. A causal story was
constructed from a correlation whose common term was never isolated.

**Recurred a third time** on 2026-08-05 during the ray-tracing port, and the
diagnosis was cheap because 1.13's lesson had been applied: the `VkResult` was
captured (`-1`, `VK_ERROR_OUT_OF_HOST_MEMORY`) and matched to this entry instead
of being theorised about. The precise state, for next time:

    find build -path '*nrd*' -name '*.spirv.h' -size -200c

returns exactly one file — `build/_deps/nrd-src/_Shaders/Clear.cs.spirv.h`, 67
bytes out of 31 generated headers. Deleting it and rebuilding regenerates it at
3,107 bytes and path tracing starts. **The whole fix is one `rm`**, which is why
the row is now P0: the cost is never the repair, it is the hour spent deciding
whether the renderer change in front of you caused it.

### 1.17 Single-pair path-tracing comparisons work by luck until they don't

2026-08-05, during S5 stage 3. A capture read **0.24597** mean absolute
difference against the stored baseline, above the highest figure previously
observed between runs, and looked like the first real regression of the track.

Rather than accept or dismiss it, the previous commit was stashed, rebuilt and
captured **four times**. The result:

| | mean\|d\| range |
|---|---|
| within the control build, 6 pairs | 0.00317 – 0.24505 |
| within the new build, 6 pairs | 0.02616 – 0.09482 |
| **across the two, 16 pairs** | **0.00308 – 0.24493** |

The across-range lies entirely inside the control's own spread and the energies
overlap completely. There was no shift. **The alarm was an artefact of comparing
four new runs against one old run.**

`AGENTS.md` already prescribes the right method — *"compare distributions, N runs
a side, never a single pair against a stored baseline"*. Every path-tracing check
in this track had instead been one fresh run against one stored capture, judged
against a floor derived from a single earlier binary. That is a weaker test than
the one written down, and it had been passing by luck: it produced one false
alarm at 0.222 (dismissed correctly, by chance) and this one at 0.246.

**A noise floor measured on one binary does not transfer to another.** The two
builds here have visibly different spreads — 0.003–0.245 against 0.026–0.095 —
so a threshold taken from either would misjudge the other. The control has to be
rebuilt and re-sampled alongside, which costs a stash and a rebuild and is the
only thing that actually answers the question.

### 1.18 A one-time exception to a delegation rule propagates into every later brief

2026-08-05, across the S5 RHI track. The rule for the coding agent was: build and
run the tests, never run the engine — rendering verification belongs to the
developer, who holds the baselines.

It held until a task where compiling demonstrably could not catch the defect: a
startup hang that produced an empty log. Letting the agent run the engine "just
this once, just far enough to see it start" was obviously reasonable in that
moment, and was granted.

**Every subsequent brief was written from the previous one**, so the carve-out
travelled with them and grew — from "observe a startup message" to "run all
three modes and confirm each writes a `.tga`" — across roughly a dozen tasks
before anyone re-read the original rule. It was never re-decided; it was
inherited.

Two things make this worth recording rather than filing as carelessness:

**The drift was invisible from inside any single task.** Each brief was a
reasonable edit of a brief that already contained the permission. Nothing looked
wrong at the point of writing, because the diff from the previous brief was
never the thing that broke the rule.

**The damage was bounded by an unrelated habit, not by the rule.** Every
acceptance decision in the track was made from the developer's own capture runs;
the agent's runs were duplicate work, never the evidence anything was accepted
on. That is luck of process, not design — had the developer been relying on the
agent's numbers, a wrong result would have been indistinguishable from a right
one.

**The mechanism, for next time:** a standing constraint belongs in a block that
is *copied verbatim* into each brief and re-read, not paraphrased forward. Where
a task genuinely cannot be verified by the permitted means, the agent should say
so and stop — that is information the developer needs — rather than being handed
a capability to work around it.

---

## 2. Design analyses worth keeping, though the decision is made

### 2.1 One BLAS for the whole scene — backlog 8.9

Shipped as `9b98c37c`: one BLAS at `graphics/vulkan/rayquery.cpp:892`, one TLAS instance at `:987`,
two geometries split opaque/non-opaque at `:917-920`, per-triangle material ids written by the merge
kernel (`slang/scene_resolve.slang:60,376`). The analysis is kept because it is where the triangle count, the
build-cost and memory tables, and the static/dynamic argument live.

#### The whole scene is 86k triangles

Measured on `danm14ab`, logged in the TLAS line: **1048 instances, 86k triangles, 61 skinned, 653
dangly**, TLAS build **56–60 µs**. That is roughly one modern character mesh, in total, for an entire
module. The two-level structure is buying nothing at this scale — the hardware traverses a top level
of a thousand boxes to reach less geometry than a single BLAS holds comfortably.

The registration plan had already argued for merging the *static* set, calling it "asking the
hardware to traverse a top-level structure of hundreds of boxes to reach what is really one rigid
scene" — `danm14ab` builds **484 structures for 744 instances** — but it was written without a
triangle count and hedged accordingly. 86k removes the hedge and suggests going further than static:
at this size, merging *everything* and rebuilding per frame is plausible, which deletes the per-mesh
skin barrier, the per-mesh BLAS build and the refit schedule in one move.

The blocker the plan named is identity — merged, the instance custom index no longer says which
object was hit. **That is cheap here because this is a ray-query pipeline: one compute shader, no
shader binding table, no hit groups.** So it needs no dispatch-side machinery at all, just a
per-triangle material index buffer looked up with `CommittedPrimitiveIndex()`. At 86k triangles that
table is **~344 KB**.

#### The shape the analysis wanted, and the one-versus-two question

One persistent world-space buffer, partitioned static / dynamic, and two BLAS over it:

- One vertex buffer for the whole scene in world space, split into a static region and a dynamic
  region. The static region is written **once at module load** and never touched again — no per-frame
  upload, no per-frame copy, no re-registration.
- The dynamic region is written each frame by the skinning and dangly compute passes writing
  **directly into their slice**, rather than into per-mesh buffers that are then read by separate
  BLAS builds. That removes the copy and the per-mesh barrier in one move.
- **One BLAS or two is a build-cost / trace-quality trade.** Two — static built once at load, dynamic
  rebuilt per frame — makes the static half free. But it is worse at trace time, and probably
  materially so here:
  - The builder never sees the whole scene, so it cannot make a globally optimal split. Merging
    "gives the builder the whole static set at once, which is where it can do its best work".
  - The two structures' bounds **overlap heavily**, because foliage is spatially interleaved with the
    terrain it stands on rather than occupying a separate region. A ray crossing the overlap
    traverses both trees.
  - Every top-level instance costs a ray transform into object space. Merged world-space geometry
    costs none.

One BLAS rebuilt in full every frame keeps traversal optimal and **deletes the refit-quality problem
outright — a rebuild never drifts from the pose it was built for.**

#### Build cost: published figures say rebuild-everything is affordable

Tellusim's acceleration-structure benchmarks, linearly scaled to our 86k triangles:

| GPU / API | Their 4.21M build | Scaled to 86k | Their refit | Scaled to 86k |
|---|---|---|---|---|
| 2080 Ti (D3D12) | 16.9 ms | **0.35 ms** | 3.7 ms | 0.076 ms |
| 6700 XT (D3D12) | 30.2 ms | 0.62 ms | 4.6 ms | 0.094 ms |
| 6700 XT (Vulkan) | 223 ms | **4.6 ms** | 4.6 ms | 0.094 ms |
| Apple M1 (Metal) | 395 ms | 8.1 ms | 29.8 ms | 0.61 ms |

**A full `PREFER_FAST_TRACE` rebuild of the entire scene costs about a third of a millisecond on a
2080 Ti**, several generations behind the development 5090. Against a 5 ms frame target that is
noise, and it buys optimal traversal, no refit drift, no static/dynamic split, no motion threshold
and a top level of one instance.

Caveat carried forward: **the 0.35 ms figure is not a measurement of this renderer.** It is an
external single-operation linear extrapolation. It omits the merge expansion, the source/scene/material
uploads, the barriers, and any cost from a different non-opaque mix.

#### Memory cost: also a non-issue, which removes the other objection

zeux.io, *Measuring acceleration structures* (2025-03-31), measures BLAS **memory** on Bistro (1.754M
triangles, fp16 positions, `PREFER_FAST_TRACE`, compacted, Vulkan 1.4):

| | bytes / triangle | our 86k scene |
|---|---|---|
| NVIDIA RTX 3050 / 4090 | 25.7 – 26.5 | **~2.2 MB** |
| AMD RDNA4 | 47.9 | ~4.1 MB |
| AMD RDNA3 | 57.0 | ~4.9 MB |

The registration plan's stated cost for merging was memory — "vertices must be pre-transformed to
world space, so nothing is shared and the merged buffer is as large as the static set… memory and a
build, against traversal". **At this scale that trade does not exist**: the structure is single-digit
megabytes even on the worst hardware in the table, and the un-shared world-space vertex buffer is of
the same order.

Two further notes from that source: **compaction is worth doing** — those figures are *post*-compaction,
so an uncompacted structure is larger than the table says, and nothing in `graphics/vulkan` sets
`ALLOW_COMPACTION` or queries `CompactedSize`; and **AMD spends roughly twice NVIDIA's memory per
triangle**, a hardware format difference rather than anything actionable, but worth knowing before
sizing pools on an AMD target.

**Do not conflate the two sources.** zeux measures memory only — it covers neither build time,
granularity overhead, refit, nor Vulkan-versus-D3D12 — so the build figures remain Tellusim's, at a
different scene and scale. Both are extrapolations rather than measurements of this renderer.

#### Two things in the Tellusim data that matter as much as its headline

- **Many small structures are worse, and measurably.** 2401 BLAS of 1.5K triangles refit in **7.0 ms**,
  while 81 BLAS of 52K — *more* total geometry — refit in **3.7 ms**. Per-structure overhead dominates
  at fine granularity, and at **714 structures** we were squarely in the bad regime. This is
  independent evidence for merging beyond the traversal argument.
- **AMD's Vulkan build path is pathological in this data: 223 ms against 30.2 ms for the same work on
  the same GPU under D3D12, a 7.4x gap.** Scaled to our scene that is **4.6 ms**, which would consume
  the entire frame budget. We are Vulkan-only, so this is a real risk and not a footnote. **The
  commitment was made anyway** — `9b98c37c` shipped the single per-frame rebuild — so if the gap
  holds on current drivers, AMD needs the static/dynamic split that one-BLAS-per-frame deliberately
  gave up, and the design must keep that option open rather than assuming one structure everywhere.

#### The static/dynamic split, and what the merged buffer is worth elsewhere

**Measured split: of 86k triangles, 41k are dynamic** (skinned, dangly or saber). Nearly half, not a
tail — the guess that instance-heavy foliage would be triangle-light was wrong, so the per-frame
dynamic rebuild is a real 41k-triangle build rather than a rounding error. Either way it is
overwhelmingly better than **714 separate acceleration structures rebuilt or refitted per frame**
becoming one build — of 41k triangles if the static half is split off, or 86k if the whole scene is
rebuilt for the better tree.

The per-triangle material table partitions the same way — static half uploaded once, dynamic half
only where it changes — and so do the material records themselves: all 1048 are pushed and uploaded
every frame today, while the static ones are identical every frame and belong in a write-once region.

**The same buffer serves the raster path.** A merged world-space vertex buffer with a per-triangle
material id is exactly what a raster mega-draw needs — the static region drawn with
multi-draw-indirect or a handful of calls, indexing the same material table through `gl_DrawID` /
`SV_PrimitiveID` rather than `CommittedPrimitiveIndex()`. **So the raster mega-draw and the merged
BLAS are one work item, not two.** That doubles the payoff and is the strongest argument for doing
it: neither path keeps its own copy of the scene, and the world-space pre-transform that merging
forces is a cost paid once for both.

Still unanswered when the analysis was written:

- **Is 86k typical?** One module is not a sample. Ahto City and the Taris streets may be far heavier,
  and dangly-dense exteriors are the worst case. Log it across a warp loop first.
- **Rebuild cost per frame for the whole set**, against the then-current 653 refits plus 61 skinned
  builds. A full rebuild of 86k is likely cheaper than what happened, but that is a prediction.
- The counter sums per *instance*, so shared BLAS geometry is counted repeatedly. That is the right
  number for deciding on a merge, but distinct triangle count is lower.
- Skinned and dangly vertices must land in the merged buffer in world space, which means the skinning
  compute writes into it directly rather than into per-mesh buffers.

### 2.2 Traced transparency — backlog 3.7, the design in full

Settled 2026-08-03, unbuilt. Sequenced after the phase-F geometry track: G3 supplies the
classification and the primitive list, and V1c narrows all of this to bounce rays, since raster will
own primary visibility.

**The split that drives everything: occlusion and emission are different channels.** Stochastic
transparency — commit a candidate with probability equal to its coverage — is an unbiased estimator
of the *multiplicative* channel, because a blended surface's contribution is proportional to its
coverage: one coin flip stands in for both the attenuation and the added colour. Additive surfaces
(`kPtSurfaceUnlitTransparent` — saber blades, glow decals, bolts) are the degenerate limit: finite
energy at zero coverage, straight colour C/α divergent. No commit probability represents them — P = 0
loses the emission, any forced P occludes what additive never occludes, and a Russian-roulette weight
is unbiased but turns a smooth glow into per-path fireflies. The structural constraint that decides
it: emission routes through `noiseFree` (`outputs.slang:56`), which **bypasses the denoiser**, so
additive emission must be computed deterministically no matter how stochastic traversal gets.

**Traversal goes stochastic for coverage.** Today `ptTraceNearest` enumerates deterministically:
blended coverage commits at every nonzero alpha and the path composites it as a transmitting surface,
additive always commits and the path pays it with a `passThrough` continuation — a full traversal
restart per layer (`trace.slang:79`, `:88-91`; `material.slang:411-418`). Instead: cutouts stay binary
at the threshold, blended candidates commit with P = α, additive never commits.
Nearest-committed-among-successes gives the correct chain — for surfaces at t₁ < t₂,
P(scatter at 2) = (1−α₁)·α₂ — independent of enumeration order, provided the flips are decorrelated
per ray × primitive. Paths become unit-weight: no throughput tracking, no restarts, one scattering
event per segment. The new variance lands only in the lit-blended channel, which the denoiser owns
and which is judged by distribution anyway. **The one number to watch when built**: blade-behind-smoke
acquires binary per-path flicker in the accumulated radiance (counted fully or not at all, correct in
expectation) — compare such frames by distribution against the enumerating tracer.

**Emission is gathered over the confirmed segment, and expectation does the compositing.** Additive
contribution = enumerate everything in `[0, CommittedRayT()]` and sum. Interleaving with stochastic
occluders needs no sorting and no explicit T(z): if the smoke's flip commits, the segment ends there
and the glow behind it is excluded; if it passes, the glow counts at full strength — expectation
Le·(1−α), exactly right. **The commit decision *is* the transmittance sample**; additive surfaces are
pure readers of it.

**The gather ladder, walked to its end.** Recorded because each rung answers "why not the
simpler-looking one":

1. *In-loop fixed (t, Le) array, filtered by final committed t after the loop.* The filter is
   mandatory: candidate enumeration is unordered, so a glow can be enumerated before a nearer commit
   arrives — including hardware opaque commits the loop never sees. Naive in-loop summing is biased
   **by traversal order, differently per GPU**. Cost: the array is live state across `Proceed()` —
   inline queries have no payloads, so the price is VGPRs and occupancy on a latency-bound loop.
2. *One `sawAdditive` bit, plus a conditional masked gather query over the segment.*
   Conservative-correct by the query contract (every non-opaque candidate nearer than the final hit
   must be offered). Near-zero live state; second traversal only for rays that crossed additive
   geometry.
3. *Accumulate regardless, track `maxAdditiveT`.* The sum is exact iff
   `maxAdditiveT <= CommittedRayT()` — soundness from the max, completeness from the query contract;
   misses validate trivially. Fallback to the gather only when enumeration was out of order, which
   makes the fallback *rate* a hardware-enumeration-order property: correctness invariant, cost
   vendor-dependent.
4. *A dedicated additive TLAS.* Both trace loops simplify (the always-commit branch and the shadow
   ray's additive skip at `trace.slang:136` disappear; shadow rays stop enumerating candidates they
   ignore), cost becomes vendor-deterministic, but every segment pays a second query setup.
5. **The decision: a flat O(N) loop** over a compact per-frame list of world-space additive
   primitives. At this game's N — bolts, sabers, a few decals — brute force beats any acceleration
   structure: wave-uniform trip count (zero divergence), records through the scalar cache, texture
   fetches only on hits inside the segment, one traversal per path segment *total*, and no second AS
   to build. The loop is O(rays × N), bounces included; **crossover is a few hundred primitives**
   (screen-filling additive particle effects). Debug-counter N per frame; the escape hatch is rung 4,
   and the function's contract — enumerate the segment, sum emission — doesn't change if swapped.

**Shadow rays don't change.** `ptShadowTransmittance` already skips additive, and its deterministic
(1−α) product is strictly lower-variance than a coin flip. Leave it.

**Sabers and bolts become analytic capsules, not meshes.** The mesh saber is crossed additive planes
assuming the viewer is the camera — an assumption bounce rays violate: a blade reflected in a floor
is seen edge-on and degenerates into a line. The record becomes a capsule and the evaluation becomes
the **line integral of an emission density** around the blade axis (hot core, radial falloff) —
view-independent by construction, a few dozen ALU, and the segment bound clips it correctly for free:
the occluded half of a blade behind a pillar simply isn't in the integral. The swing trail is a
deterministic chain of 4–8 decaying capsules from a CPU-side transform history — **not** stochastic
time sampling, which would inject noise into `noiseFree`. Bolts are capsules outright and will look
better than the authored billboards. Consequence: the additive class may leave merged geometry
entirely — particles and decals stay procedural quads, capsules are records — and nothing additive
remains in either the merged buffer's traced ranges or any TLAS.

Two requirements ride with the capsules:

- **One shared slang function, two callers.** The capsule record is scene description; the tracer
  evaluates the integral per segment in the additive loop, raster draws a bounding quad in the blended
  pass and evaluates the identical function per fragment over `[near, opaque depth]`. Same record,
  same code — **the modes agree by construction instead of by two implementations of one glow.**
- **Parameters come from authored content.** Blade colour, length and radius must be derived from the
  existing models and textures — fitted at admission or curated per crystal like `curatedEmission`.
  This deliberately departs from the authored 2003 look, same doctrine as "PBR is not held to its old
  output"; it is a look decision to make explicitly, not discover.

**Raster's side is already settled by G8** — the CPU sort plus premultiplied
`ONE, ONE_MINUS_SRC_ALPHA` keeps the two channels separate deterministically. Stochastic transparency
in raster (Enderton-style stochastic depth samples, with additive fragments as visibility *readers*,
never writers) is only the escape hatch if the blended set ever outgrows the sort. At a few hundred
quads it won't.

**Hooks into phase F:** admission's one classification vocabulary (G3) supplies the additive-emissive
kind, and building the primitive list is its consumer. The list, AS membership and the sort remap are
all **derived artifacts, not scene description** — outside the G3 upload-hash equality check by
construction. If any additive geometry stays in the merge meanwhile, G3 must keep its triangles
contiguous in merge order so a range can name them.

### 2.3 Coverage is transmission — the model the tracer runs today

Written down because it was decided in code and three of its constants disagree with each other.

**A blended surface is glass with an IOR of 1.0.** Not a composite and not a special case in the
integrator: a material that lets rays pass straight through, weighted by coverage. Shade it, weight
the result by alpha, continue the ray with `1 - alpha`, and skip it entirely below an epsilon. That
is what makes smoke *lit* rather than pasted on, it is what the tracer already does for any
transmitting surface, and it needs no ordering, no blend state and no second pass. The alternative —
a rasterised composite over the traced image — **cannot be in the BLAS at all.**

Coverage arrives in three flavours and traversal has to tell them apart
(`slang/tracing/trace.slang`):

| kind | traversal | why |
|---|---|---|
| cutout, `kPtMaskPunchThrough` | binary, commits at 0.5 | a leaf either is there or is not |
| blended, `kPtMaskBlendedCoverage` | commits at any nonzero coverage, transmits the remainder | smoke, and anything raster would have alpha-blended |
| additive, `kPtSurfaceUnlitTransparent` | always commits, passes through, casts no shadow | sabers and glow are emitters, not occluders |

**The open question is how many transmitting hits a ray may cross, and the three answers do not
agree:**

| path | limit |
|---|---|
| additive pass-through | `kMaxPassThroughSteps = 16` |
| blended transmission, primary and bounce | `kMaxTransmissionSteps = 12` |
| shadow ray | **uncapped** — every candidate, until transmittance < 1e-3 |

A camera ray through the Dantooine plume stops after twelve layers. A shadow ray from the same point
walks all 232, multiplying `(1 - alpha)` at each one, and reports the surface fully occluded. **So the
smoke is lit by rays that see a twelfth of the volume and shadowed by rays that see the whole of it**
— the leading explanation for the black band, and consistent with the measurement: `diff_factor` 1.0
and a valid `view_z` in that band, with traced diffuse at **0.00003** against **0.05318** on the
character beside it. (The old "12 vs 96 pixel-identical" note tested the wrong axis and should not be
read as clearing the caps.)

Underneath the asymmetry sits a question the caps cannot answer. **Single scattering with exact
attenuation and nothing scattering back in gives black by construction**; real smoke reads bright
because of multiple scattering this integrator does not have. Making the three limits agree is
necessary. It may not be sufficient, and if it is not, the answer is a scattering approximation
rather than a larger number.

Two consequences that compound: blended hits must not write the denoiser guides — a thin quad has
neither the depth nor the motion of the surface behind it, so NRD reprojects from the wrong place —
and a NaN written into a guide spreads across the frame instead of staying in its pixel. **Measure
non-finite pixels with `numpy.isfinite` on a `--dumptargets` `.npy`, not by eye**: NaN renders as
black, white or garbage depending on the path it takes.

Ruled out as causes of the smoke NaN: the premultiplied-alpha flag is never set for these textures
(`fx_smoke01`/`fx_smoke`, DXT5, `Blending::None`); a plain alpha sign flip makes it worse; `mainTex`
resolves to a valid bindless id; and the blended-coverage branch was rewritten (`befa8791`,
`slang/path_trace.slang:300-323`) to do no division at all, with the two surviving `/ outputs.specFactor`
sites (`:269`, `:271`) non-blended and floored by `kDemodulationFloor = 0.02`
(`slang/tracing/brdf.slang:14`).

### 2.4 The sky classifier was built, measured, and rejected — backlog 1.14

Decided 2026-08-02, *after* trying the runtime route and measuring it.

**What was tried and rejected:** a geometric classifier — accept a room only as a cube-like enclosing
shell (5–6 planar panels, ≤256 faces, ≥99% of area facing the camera, four sides and one top,
oriented bounds within 2:1, camera enclosed, one panel per cube face, right-angle UV mapping), exactly
one per level. **It was built, it worked, and a full 117-module sweep says it is not enough: 61
modules had a sky before, 15 after, 46 lost.**

**The rejections are not a threshold that needs tuning.** 28 of the 46 fail the panel gate because of
*one extra mesh sharing the room with the sky* — `lma_bboard01`, an alpha-cut billboard of the Ahto
City towers standing in front of five clean Manaan panels; `lda_grass07` on `danm14ac`; `lda_trim03`
on `danm16`; `lta_townsh` on `tat_m18ab`. And `m12aa_01a`, the Ebon Hawk sky shared by four modules,
is **one mesh, 120 triangles, one tiling starfield** — a shape a six-panel model cannot express at
all.

**The decision: convert the skyboxes outside the engine and ship a curated asset set.** A script
consumes a committed config, extracts the panels from the player's own install, orients and tiles them
into cube faces, and writes the assets; the assets are gitignored, the config is committed.

Structure is **per game** — `GameID` already distinguishes them and model names collide across the
two, so `override/k1/` and `override/k2/`, each holding `materials.ini` (today's `trace-classes.txt`,
**971 curated entries** surviving at the time **only** in `build/bin` and untracked — moving it finally
version-controls them), `modules.ini` mapping each module to a sky and to **the room to suppress**
(`sky = dantooine_plains`, `room = m14ab_02e`, or `sky = none`) — naming the room is what makes the
classifier deletable rather than merely bypassed — and `sky/<name>/` holding six faces plus a small
ini, LDR now and HDR later. Only `sky/` is gitignored.

**Built as designed, the same day it was decided.** `53a9067c9` moved the 971 entries to
`override/k1/materials.ini`; `d219bd6bd` added `modules.ini` for both games as a first-pass survey
pending curation; `8fa4e55b6` added the `skybake` tool; `.gitignore:10` ignores `override/*/sky/`.

Both games are installed and curatable: **117 K1 modules and 82 K2**, identical texturepack layout, so
one tool covers both. The sky is **never in the BLAS** (already true) and is sampled on miss, so
correct rotation and tiling must be baked in; that is manual work, verified repeatedly by subagents
and codex rather than assumed.

**The panels are skipped in every mode, not kept for raster** — there is no reason for a suppressed
sky to survive in one renderer and not the other. Raster reconstructs the world-space ray per pixel
and samples the cubemap in screen space where nothing wrote depth; `pbr_resolve.slang`'s
`resolveFragment` already binds `sGBufDepth` and rebuilds NDC→view, so PBR is a few lines inside an
existing pass rather than a new dispatch. **Retro is the gap**: it is forward-rendered and
`postprocess.slang` binds no depth, so it needs one added or it gets a hole.

**No sky rotation** — the faces are baked in game world coordinates and sampled with the world
direction directly, so the runtime carries no orientation at all. That makes the axis convention the
one thing worth proving rather than reasoning about: **KOTOR is Z-up, cube faces are Y-up, and a
mirrored or yawed sky looks plausible enough to ship. Bake a six-colour debug sky first** and confirm
empirically which world direction shows which face before generating a single real asset.

**Evidence, all measured 2026-08-02:** 41 of 56 sky rooms carry a sky-named texture and 29 of those
carry exactly five; the 15 with none are Kashyyyk canopy, walls and rock, correctly not skies. **No
module has two skies** — 44 modules nominate two or more rooms and `tat_m17aa` nominates fifteen, but
the accepted count is only ever 0 or 1, so the one-per-level rule has never fired. `lev_m40aa` hangs
on load in raster as well as tracing, unrelated to sky. The classifier is preserved in a stash for its
signed-permutation face-orientation maths, which the offline baker needs verbatim. Survey data:
`scratchpad/sky_survey.json`, `scratchpad/sweep_results.txt`, decoded panels in `scratchpad/skytex/`.

A related trap noted earlier and now superseded by this decision: the old sky classification was a
**selfIllum luma test**, so any fully self-illuminated *interior* surface — lit panels, screens —
could be misclassified as sky, which both applied the sky dial to it and, worse, moved it to
instance-mask bit 2 where **shadow rays pass through it**. Enclosed scenes (Taris underground) are the
regression test for sky leakage; module seams letting hemisphere rays escape to the dome are the other
suspect mechanism.

### 2.5 Geometric grass versus alpha-tested quads — backlog 2.6

Grass is punch-through quads, so in a traced frame **every ray-triangle hit runs a candidate shader**
— interpolate UV, sample, compare, decide — which drops out of fixed-function traversal on exactly the
geometry rays cross most.

Measured on `danm14ab` at a grassy camera: 3× density cost **+25% of the graphics slot** and scaled
with samples (16 spp went **32.6 → 46.4 ms**) — but **those numbers were taken against the pre-fix
clamped cluster pool and are an underestimate.** `c343f273` made the pool grow with the dial, so
density now moves the count roughly linearly where before it saturated. Re-measure before quoting a
ceiling.

Opaque blade geometry stays in hardware, lets the BLAS build with `PREFER_FAST_TRACE` and compaction,
and lets traversal commit and stop. Against it: **4–8× the triangles**, a bigger BLAS, and a slower
BLAS build — and grass clusters materialise as the camera moves, so that build cost is per frame
rather than amortised.

**Measure the ceiling before building anything.** Mark grass opaque and re-run: the image is wrong,
but the frame time is the upper bound on what geometric grass can win, since it removes exactly the
any-hit cost and adds none of the triangles. That test needs no new machinery — per-geometry
opaque/non-opaque partitioning already exists (`graphics/vulkan/rayquery.cpp:917-920`) and the whole
experiment is scriptable now that `grassdensity` sets the dial and the `grass` toggle works.

**And do it after hybrid**, which changes the answer: once raster owns primary visibility, grass is
traced only for shadows and secondary bounces, so the primary rays — the ones crossing the most grass
at the most pixels — stop existing and a large part of the cost goes away for free. A cheaper middle
option exists either way: trim the quads to the opaque region of the texture, which cuts wasted
candidate invocations with no new art.

The earlier framing, from the registration plan, was the same cliff seen from the other side:
**alpha-tested foliage is a known ray-tracing cliff**, and 544 alpha-tested quads across the view is
exactly the shape that hurts. **It also had a hard prerequisite and a silent failure mode**: marking
foliage non-opaque makes `RayQuery::Proceed` start returning true on candidates, and a loop with an
empty body neither commits nor ignores a candidate, so the hit is dropped and alpha-tested geometry
**disappears from the traced view while still rendering in raster** — which looks like anything except
a traversal bug. Order: material buffer and bindless textures first, then non-opaque foliage, **and
never the reverse.**

### 2.6 Light selection, the light BVH, and emissive geometry as area lights

Direct lighting selects one light per sample by importance — contribution estimated as multiplier
times attenuation times NdotL times colour luminance — which makes shadow-ray cost constant per sample
instead of scaling with the area's light count (`kMaxLights` was 32 then; `58c55eaab` widened the
uniform array to 64 and moved the per-frame budget onto a `maxLights` dial). The selection scan itself is
still linear over the active list, and the estimate ignores occlusion.

**A flat power CDF is not a usable stage once emitter geometry joins the set**: importance must be
weighted by each light's angular area *from the shading point* — power alone ignores distance squared
and orientation, so beside one lamp among hundreds of panes nearly every sample lands elsewhere — and
a per-point CDF is O(N) per shading point. The entry requirement is therefore **the light BVH itself**
(the Conty-Kulla many-lights family): per node spatial bounds, an orientation cone and aggregate
power, descended stochastically per sample with each branch chosen by the receiver-dependent estimate
and the pdf accumulated as the product of choices. O(log N) per sample, no CDF ever materialised,
leaves small enough that a local pick is O(leaf). Built per frame beside the TLAS from the same
snapshot. **A flat CDF survives only as a debug fallback for validating the estimator against brute
force.**

The same estimator absorbs emissive geometry. The several hundred emissive panes currently light the
scene only when a hemisphere ray happens to hit one — high variance that no sample count fixes
cheaply. The design: extract emitter triangles (non-sky — **the sky's huge solid angle is exactly
where hemisphere sampling wins**, and `kTraceSky` already separates it) into the same sampled light
set as the point lights, one power CDF over both record types, one NEE sample and shadow ray per path
sample, weighted by the geometry term over area pdf. **Emitters managed this way return zero emission
to bounce rays — their light arrives via the estimator — but full emission to the camera ray**, or
directly-viewed screens would go dark while indirect light double-counted. Per-triangle power in the
CDF is area times luma of `selfIllum` times `diffuseColor`, **a stated approximation**: average texel
emission is not known CPU-side, while the shader samples the true textured emission at the chosen
point.

**Bounce lighting does not exist for analytic lights.** With a single direct light as the only source,
the Bounces dial changes nothing — which means the renderer is not yet a path tracer for exactly the
light type scenes are lit by. Mechanism: next-event estimation runs at the primary hit only; path
vertices collect emissive surfaces and the lightmap cache but never sample lights, so an analytic
light's energy terminates at the first surface it touches. **The "shadow rays at every bounce is not
its budget" comment that justified this predates the atomics discovery and is obsolete at 1.7 ms
frames.** Fix: run the per-vertex light selection at every path vertex, folded into `pathThroughput`.
This is a prerequisite for the calibration programme — "raise lights until bounce light is visible"
requires bounce light to exist.

### 2.7 The calibration programme and the observed-defect ledger

Recorded 2026-07-29 from a live review of the traced image — the user's observations with technical
analysis attached. Together they define the calibration programme that replaces ad-hoc dial tuning.

**The lighting model this converges on.** The image gets a **tonemapper**, and light intensities rise
substantially so bounce lighting actually registers — the near-1.0 dials produce first-bounce energy
too weak to see. Calibration rules: direct light alone should land around **0.3–0.5× of the final
rgb** in directly-lit areas of real scenes (leaving visible headroom for indirect), and direct
intensity is set so lit areas match **retro**, which stays the reference for "correctly lit". Default
bounces: **2**. The sky wants roughly **2× its current intensity**, and the sun should ultimately be
part of the sky — a job for future HDRI replacements rather than the current promoted-directional
stopgap. In this frame, brightness above the old raster capture is dynamic range, not overshoot; **the
earlier "3.2× ground" reading compared untonemapped traced output against PBR raster, which is neither
the visual reference (retro is) nor evidence of an energy bug by itself.** The sky-plus-lightmap
double count on outdoor statics remains a real accounting question, but its resolution lives inside
this calibration, not in a clamp.

**Ambient light has no place in path tracing.** The world-ambient term and the ambient-only flat
irradiance pre-pass are raster survivals; both retire as light intensities rise and transport carries
the frame. The ambient-only records themselves do not vanish — their flat, unshadowed application
does.

**Artist-placed lights encode emission intensity.** Odyssey levels fake GI: many point lights sit
directly at emissive panels. **Those pairings are a measurement** — the point light's intensity
estimates the emission strength of the panel it fakes, which is exactly the calibration the
area-light/NEE stage needs. Rules derived: a point light with no emissive surface nearby is a real
light and stays; **angular size must never be zero** — point lights get a relatively wide angular size
(soft shadows), the directional sun stays fairly sharp; and some directional lights exist purely to
emulate GI (one of the stunt levels demonstrates this) — reference material for what the bounce
lighting should reproduce, and candidates for removal once it does.

**Some objects do not interact with lighting**: leaves, hair, Manaan puddles, doors everywhere, parts
of levels. These are the surfaces routed through the transparency candidate path — `CandidateLayer`
radiance is albedo-times-lightmap-ish and **never sees the sun, scene lights, or the hemisphere**.
Anything classified punch-through or `TransparentModel` (including doors via material type, hair via
blending) shades through that unlit path wherever alpha is below the opaque threshold. The layer model
needs direct lighting, or near-opaque texels need to commit into the full shading path far more
aggressively.

**Main screen render is incorrectly cropped** — the live window appears to render at full resolution
and crop rather than scale. Output-extent plumbing between the trace target and the presented image;
not a capture issue.

**Feature ledger feeding the same programme:**

- **Debug view modes**: lights, object bounding boxes, emissives highlighted, object type — selectable
  from the ImGui scene window, which already knows every entry.
- **Area lights from emissive mesh parts** at triangle granularity: emission removed from the
  pathtraced direct hit and moved into NEE with importance sampling, with the panel-adjacent point
  lights as its intensity calibration.
- **Multiple importance sampling** between the specular and diffuse lobes, replacing the current
  single-lobe Russian-roulette split.
- **Much more ImGui control** over the tracer: per-category material property overrides — including a
  **material colour override per object category** (rooms, doors, placeables, creatures, grass, …)
  alongside roughness, emission and env strength — so calibration hypotheses can be tested live
  instead of by rebuild. A flat-colour override per category is also the quickest visual isolator:
  **paint every door magenta and the "doors don't interact with lighting" class of bug identifies
  itself.**

### 2.8 The visual target is the retro look, reached physically

The traced image should read like the original renderer's frame — that is what the artists lit for —
**but by physical means only: no clamps, no additive grafts, no gamma-space arithmetic.** The look
lives in the authored data, so the tracer honours the data rather than imitating the math. Retro's
`min(1, light) * albedo` shoulder and gamma-space sums are not reproducible by a physical renderer and
are not targets; retro captures serve as an *aspect* reference — which surfaces sheen, where light
comes from, lit-to-shadow balance — **not a pixel-ratio one.**

Concretely: the envmap is authored incident radiance, so envmapped surfaces sample it through the GGX
lobe (later: trace the reflection and fall back to the envmap on miss, so near geometry reflects for
real). Outdoor brightness overshoot is an energy accounting question, not a grading dial: **lightmaps
already bake sun and sky for statics, so live sky at the primary hit plus lightmap cache at the bounce
can count the same photon twice.** Which authored source owns which light path must be decided per
path — the same masking discipline the NEE design states for emissive panes.

### 2.9 Debug geometry belongs in `GpuScene`, in a region the BLAS does not ask for

Deleted with `RenderRegistry`, along with `game/debug.{h,cpp}` and the
`showaabb`/`showwalkmesh`/`showtriggers` commands, because the flags cannot do anything once the draws
are gone. It comes back, and **it comes back inside `GpuScene`** — a separate debug renderer would be a
dangling second scene representation, and would have to rebuild world-space merging, transforms and
skinning that the core already does. A walkmesh on a moving door then works for free.

**Admission does not imply BLAS inclusion.** That was the objection to this and it is wrong: the core
publishes regions and the *tracer* supplies its own intersection classification, so debug is simply a
region the BLAS build does not consume. Nothing appears in a reflection or casts a shadow, and no
consumer needs a filter — the tracer just never asks for that range.

**`offMaterial` stops being a blocker.** Earlier plans called it "the one attribute `MergedVertex`
structurally cannot express" and used it to argue walkmesh should be deleted rather than merged — it
was even named as the highest-leverage deletion of Phase A for that reason. But the merged stream
already carries a **per-triangle material id**, so walkable versus non-walkable is two materials over
one mesh, not a vertex attribute. **No widening of `MergedVertex`.**

**What renders it**: the raster consumer, in its own pass over the debug regions, after the scene
resolves. World-space and depth-tested — it renders *in* the scene and is occluded by the scene's
objects, so a walkmesh behind a wall is hidden. That needs the scene depth buffer, and **the hybrid
decision supplies it**: raster owns primary visibility in every mode, so a raster depth buffer always
exists and the pass is the same pass in traced and rasterised frames. The earlier worry — converting
`traced_view_z` back to device depth, or paying for a depth prepass — does not arise.

**The old shape does not return**: `RegisteredDebug` held a `std::function<void()>` per entry, **a
type-erased callback inside a data snapshot.** An immediate-mode `line()`/`box()`/`mesh()` API can
still exist, but as a *producer* that admits transient geometry into `GpuScene` for the frame, not as a
second renderer. The three flags become admission filters. Note this is branch divergence from
`master`, where the capability exists.

### 2.10 Keep the traced G-buffer as the validation instrument

The hybrid decision retires traced camera rays, and **the one piece of the traced primary path that
survives is a debug shader emitting a ray-traced G-buffer.** It is not a leftover — **it is the check
that the two renderers describe the same scene, and it is the only check for that which does not
depend on judging an image.**

The dump already exists (`2fd6e713`, `a3a2f64d`) and publishes `traced_view_z`,
`traced_normal_roughness`, `traced_motion`, `traced_diffuse`, `traced_specular`, `traced_noise_free`,
`traced_diff_factor`, `traced_spec_factor`, `traced_device_depth` and `traced_screen_motion` alongside
the `g_buffer_*` set (`graphics/vulkan/rayquery.cpp:820-823`). **There is no `traced_albedo`, and any
comparison script written against that name silently compares nothing.**

What is needed is to keep the dump deliberately as the traced primary path is deleted around it, and
to state which channels must agree and to what tolerance. Position and depth should agree to the bit
once the merge and the vertex stage compute world position identically; normals only once the merge
adopts raster's inverse transpose. **Deleting the traced visibility walk without first pinning this
down loses the ability to validate the merged scene at all.**

This is also why F-geo runs before F-vis: **F-geo's reference is the traced G-buffer, which is
produced by traced primary visibility — exactly what F-vis deletes. F-geo must finish using the
instrument before F-vis removes it.**

### 2.11 Material identification: two tiers, in this order

A bare integer identifies an object and tells you nothing about it. Debugging this renderer is mostly
the question "what is that thing" — **most of a night went into a missing head that would have been
half an hour if a pixel could have named itself.**

Nothing needs authoring; the names already exist and are simply not reachable from the scene library:
`graphics::Model::name()` is the MDL resref (`c_drdastro`, `m14aa_01a`), `graphics::ModelNode::name()`
is the node within it (`head_g`, `wall_01`), and `game::ObjectType` gives Creature, Placeable, Door,
Trigger where a game object is behind the node.

**Intern them; do not store strings.** A snapshot entry is copied for every object every frame, and a
`std::string` per entry would cost more than everything else in it. Keep a string table on the scene
graph and put a `uint32_t` name id beside the object id.

The two halves have different jobs:

- **the integer is for indexing and identity** — the key, the TLAS instance index, what a cached BLAS
  hangs off. It must be cheap, dense and meaningless.
- **the strings are for heuristics** — deciding what a surface is made of, which is a question the
  assets cannot answer.

That second one is not a debugging luxury. **Odyssey assets carry no metalness and no roughness at
all**: the MDL reader reads specular and shininess and drops them, roughness is scavenged from diffuse
alpha (which since the environment-strength change also carries reflection strength), and `metallic`
is hardcoded to zero in both resolves. **Every PBR parameter beyond the environment strength is
currently invented.** Names are the only remaining signal, and the data already uses them that way —
`cm_baremetal` against `cm_dantne` are environment maps whose names describe the material, not the
geometry.

Two tiers, in this order:

**1. An authored name-to-material map.** Data, shipped and under version control, keyed on the names
above. Most specific key wins — model plus node beats model, which beats texture. This is the source
of truth, and being data means a wrong assignment is a one-line fix by anyone, not a rebuild. It needs
**an editor mode to maintain it**: a list of what the scene actually contains, an assignment per
entry, and a save. **The useful part is the inverse view** — which objects have **no** authored entry —
because that is the work queue, and it is measurable as a percentage rather than a feeling.

**2. A heuristic for everything unmapped.** Pattern matching over the same names for the long tail
nobody will hand-assign. It will be wrong sometimes, which is why it must be **visibly** a guess: the
editor should show which tier decided a given surface, so an inferred material is never mistaken for
an authored one, and so a bad guess is findable rather than merely suspected.

**The order within this track is load-bearing, not incidental: the heuristic comes last because it is
the fallback, and it needs the authored map to already exist as the thing that overrides it. Shipping
the guess first makes every later authored entry a correction rather than a decision.**

Keeping both in one table rather than spreading inference through the shaders is what makes either
correctable. One constraint from the destination: **a TLAS instance custom index is 24 bits.** That
bounds how many instances one frame's mapping can address, not the width of an id — the mapping is
rebuilt with the TLAS.

### 2.12 What a hit shader deletes

Several G-buffer packing tricks exist only because a deferred resolve cannot reach the material: the
environment-map derived layer index smuggled through self-illum alpha as a byte
(`slang/pbr_resolve.slang:195`), and the geometry feature bits packed into lightmap alpha (`:34`,
unpacked at `:177`). A hit shader indexes the material directly, so both stop being load-bearing —
**and with them goes the class of bug where a stale layer index bleeds onto a surface that never had
an environment map.**

---

## 3. Measurements and reference data

### 3.1 `danm14ab` scene composition, frame 310

From `formatRegistryCounts`:

| | registered | drawn opaque | drawn transparent |
|---|---:|---:|---:|
| rigid | 744 | 237 | 38 |
| skinned | 61 | 13 | 0 |
| dangly | **653** | 7 | **544** |
| saber | 4 | 0 | 4 |
| emitters / particles | 41 / 55 | — | — |
| grass | 1 node / 1482 clusters | 1 | — |

**544 of the 586 transparent draws are dangly meshes.** Transparent plus dangly is foliage, and it
dominates everything: there are **ten times more dangly meshes than skinned ones**, and the transparent
pass is very nearly nothing else. That reorders the deforming-geometry work — **skinning is the
interesting problem and dangly is the expensive one.**

Related figures from the same module: 1508 total entries; `danm14ab`'s six room models are roughly
**940 of the 1508 entries** against about fourteen for every placeable and door, so
`Material::staticObject` already covers the bulk of an outdoor area. 484 acceleration structures for
744 instances. A character is not one mesh: `c_drdwar` registers **30 entries** (`belly`, `chest`,
`neck`, `head`, `l_upperarm`, `lfngr1`…) and `c_khounda` registers **23, eight times over**.

### 3.2 Frame time across the 2026-07-30 session: `danm14ab` 14.66 → 5.38 ms

Five commits, each measured either side. The wall-clock harness is `(t(900) − t(300)) / 600` after a
discarded warm-up, three samples.

| commit | change | danm14ab | ebo_m12aa |
|---|---|---:|---:|
| — | before | 14.66 | 10.06 |
| `9b98c37c` | one BLAS, one dispatch, two buffers | 7.91 | 8.54 |
| `363c8770` | material record out of the traversal registers | 6.79 | 7.08 |
| `0079b2d8` | sky becomes the ray-miss case | 5.79 | 6.87 |
| `791363fe` | raygen ray tracing pipeline | **5.31** | **6.98** |
| ~~`395a7afe`~~ | ~~Shader Execution Reordering~~ — reverted, `b3fe1f3b` | — | — |

**Treat a difference under about 10% on a single module as unproven** — see §1.2 for the 12.2% spread
that this table sits inside.

### 3.3 The Nsight trace of 2026-07-30, and what remains of it

Frame **17.75 ms**, of which `rayquery:primaryRay` is **5.70 ms** — so roughly **12 ms, two thirds of
the frame, is spent outside the trace**, in **555 command-buffer events** recorded before it.
Throughput confirms it: **RTCORE 10.8%, SM 9.8%, compute-shader warp occupancy 7.2% with 30.2% of
warps unallocated in active SMs.** The GPU is very nearly idle while the CPU feeds it a long chain of
tiny serialised work.

**That trace predates `9b98c37c`, and the shape it describes is gone.** It was one iteration per
skinned mesh — bind pipeline, bind two descriptor sets, push constants, dispatch `vertexCount/64`
groups, full pipeline barrier, build one BLAS — repeated across hundreds of meshes. There is no
per-mesh loop now: one dispatch builds the whole scene mesh and one BLAS is rebuilt from it. **Read the
12 ms as a measurement of the old renderer, not a live target; nothing has re-traced the new one.**

The 12 ms is also the number nothing accounts for: the performance budget records the *traced frame*
target as met at 4 spp — 1.68 ms at the Ebon Hawk, 2.74 ms at the Taris cantina — while two thirds of a
frame goes to command submission before the trace begins. Whether that is scene-dependent (the trace is
an exterior with grass, not an interior), a regression, or simply never measured, was unresolved.

### 3.4 Occupancy is the lever that keeps paying; workgroup size is not a lever

Nsight, frame-level, headless via `ngfx --activity "GPU Trace Profiler" --auto-export`: compute warp
occupancy **17.44% → 22.68%** for the register change alone, **with warps launched and average warp
latency both flat to within 0.2%.** Nothing got faster; more warps fit. The same lever is why the RT
pipeline helped and why SER helped on interiors.

**Workgroup size is not a lever — measured, null.** 8×8 (64 threads) beats every 256-thread shape by
**20–40%** on all three modules, and 16×16, 32×8 and 8×32 are within noise of each other, **so it is
thread count and not aspect ratio.** That is consistent with register-limited scheduling: a 256-thread
block must reserve eight warps of registers before it can be scheduled and cannot retire until its
slowest ray finishes.

What is left across the bounce loop after the material record came out of the traversal registers:
`HitGeometry` and `SurfaceShading`. Occupancy is still only 22.7%.

### 3.5 The performance budget

The traced frame targets **200 fps at 1 spp on simple scenes** — **5 ms of frame budget on the
development machine's RTX 5090**. That hardware traces billions of rays per second; at ~1080p and 1 spp
the frame needs roughly **six million rays** (primary, bounce, shadow), a fraction of a millisecond of
pure ray throughput.

The target is **met at 4 spp**: **1.68 ms at the Ebon Hawk, 2.74 ms at the Taris cantina**, once the
stats-counter atomics moved behind a debug flag.

**The standing disciplines survive the win**: GPU timestamps around TLAS build and trace dispatch are
still owed so future regressions get attributed by facts rather than five falsified theories, and
**every optimisation must name the milliseconds it claims.** Headroom work when the budget tightens
again — denser scenes, more bounces, pane NEE — remains the structural list: leaner dispatch shape,
merged static BLAS, refit-over-rebuild. **Feature trims stay off that list.**

### 3.6 The registry's cost, measured 2026-07-28 — and moved rather than removed

OpenGL, `danm14ab`, `--pbr 1`, capture harness, wall clock differenced between a 300-frame and a
900-frame run so startup and module load cancel, with a warm-up run discarded first.

| commit | ms/frame | |
|---|---:|---|
| `a7b2bf0f` | 4.51 | immediately before the registry |
| `c536ac72` | 5.62 | the registry — **+1.11 ms, +25%** |
| `72fb544e` | 5.01 | after material flattening, which recovered 0.61 ms |

Per-slot, `a7b2bf0f` against `c536ac72`: **Graphics render +1.146 ms, Update +0.544 ms**, input and
audio unchanged. Inside HEAD's Graphics slot:

| phase | ms/frame |
|---|---:|
| snapshot registration (`renderScene`) | **0.445** |
| **the six `drawScene` walks** | **1.873** |
| `_objects.clear()` destruction | 0.020 |
| `resetFrame` bookkeeping loop | 0.002 |

**The draw walk is four times the registration, and the cleanup plan had it backwards for four
revisions** — every version argued the *copy* was the thing worth deleting. The copy is 0.445 ms; the
six walks are 1.873 ms. **So the prize is deleting `drawScene`, not deleting the copy**, and that is an
argument for ranges over per-object selection rather than an argument about snapshot construction.

`RegisteredObject` is **488 bytes**, so 1508 entries are **~0.70 MiB** constructed and destroyed per
frame, **and the variant makes a three-field billboard pay the largest alternative's width.**

**Ruled out, with numbers**, because negative results are what stop the same guesses recurring:

- the 653 dangly position vectors: **0.065 ms/frame** including both allocation and fill — real, and an
  order of magnitude too small;
- the dead `drawnPasses` clear in `resetFrame`: **0.002 ms**;
- **caching the cull test per model root: no effect at all** (see §1.5).

**Both figures were moved rather than removed.** `RenderRegistry` is gone, but the pattern survives
verbatim under `GpuScene`: `libs/scene/graph.cpp:438` calls `_gpuScene.resetFrame()`, then `:516` calls
`collectInto(_gpuScene)`. The 0.445 ms (~9% of frame) and the 1.873 ms of draw walks were relocated by
`8d37449d`, **so neither figure describes the current tree and both need re-measuring before the cost is
scored.**

And the measurement problem is structural: the raster profile shows PBR and Retro within **2–4%** of
each other despite very different GPU work, **because the graphics slot measures CPU time recording the
frame, not the GPU executing it.** Nothing here can be scored honestly until GPU timing lands in
`IStatistic`; all current numbers are CPU-side frame time.

### 3.7 Where the per-frame cost actually is — which copies are real

Read the copies rather than guessing at them, because the obvious suspect is not the expensive one.

- **The `Material` copy dominates, and it is not the geometry.** Every `registerRender` builds a
  `Material` on the stack whose `textures` is an `unordered_map`, inserting one to four entries — a
  bucket array plus a node each. Then the entry copy-initialises it: **a second full hash table, built
  and torn down every frame. Two per entry, roughly 1,500 entries in `danm14ab`, before a vertex is
  touched.** Same pattern in `EmitterSceneNode` and `GrassSceneNode`. **The fix is flattening
  `textures` into fixed slot indices** — which is the bindless change wanted for unrelated reasons, so
  it pays twice. Done in `96b2d432`: `std::array<Texture *, 6>` on a `MaterialTextureSlot` enum,
  `Material` trivially copyable, verified pixel-identical on both backends.
- **Dangly positions are moved, not copied.** `RegisteredDeformation` is taken by value and `std::move`d
  into the entry, and the call site passes a prvalue. One allocation per dangly mesh per frame for the
  `reserve`, and the positions are recomputed every frame anyway — **nothing here to save, despite 653
  of them.**
- **Skinned meshes do copy.** `RegisteredSkin {_bones, _prevBones}` copies both vectors by value —
  2 × `kMaxBones`(24) × 64 B and two allocations per skinned node per frame. `_bones` is a member whose
  capacity is already retained across frames, so **the copy exists only because the entry owns its bones
  rather than referencing them.** Fixed by a span once ids make the node safe to reference for a frame.
- **A cost in the other walk entirely:** `drawScene` re-copies instance arrays on every pass that selects
  them — grass into `visible` and then again into 256-cluster batches, particles into `visible`. That is
  in the draw walk, not the register walk, **so nothing about registration affects it** — and it scales
  with denser grass. Wants a span over the stored vector plus a `(first, count)` pair.

### 3.8 Grass: cluster counts, cost, and scale

- **Cluster count follows the density dial** since `c343f273`, where before it saturated:
  **0.5 → 1457, 1.0 → 2903, 2.0 → 5806, 4.0 → 11615 clusters.**
- Pool sizing: `kNumClustersInPool` = **4096** (`node/grass.cpp:40`), capped by `kMaxClustersInPool` =
  **32768** (`:45`). The old fixed **2048** figure is superseded, as is `:42`, now a comment.
- `kMaxClusterDistance` = **32** (`node/grass.cpp:48`). **Raster culls at it; the tracer admits the whole
  pool**, so at an 80-unit fixture camera raster draws nothing and any comparison is meaningless.
- **Grass is cheap in triangles.** The grass mesh is a two-face quad, so a full 2048-cluster pool is
  **4,096 triangles** — 86k becomes ~90k, about **+4.8%**. Raster does not cap at 256; it frustum-culls
  and batches in 256s, where **256 is only the uniform-block limit.**
- **Grass placement was not deterministic.** `getRandomGrassVariant` drew from a global RNG in
  materialisation order, so the same face yielded different grass depending on when the camera
  approached it — invisible with one viewpoint, immediately visible under ray tracing when a reflection
  shows a hillside populated differently from the direct view. Fixed by hashing `(faceIndex,
  clusterIndex)`. **Forced by admission: once grass is in the BLAS, an RNG-order change is a traced-image
  change.**
- **What grass actually is**: not an object with an extent and a region. `GrassSceneNode` holds a
  reference to an `aabbNode` — a mesh from the room model — and keeps every face of it whose
  `face.material` appears in `_properties.materials`. **The grass region *is* the union of those faces,
  fixed at init.** Each cluster inherits the face's lightmap UV via `tryFaceUV2`, so grass is lit by the
  room's lightmap. `update()` is then pure view-dependent LOD. So "grass everywhere it can go" is
  already well posed — the tagged faces; what is view-dependent is only how many are currently realised.

### 3.9 Particles: the pool, the count, and the figure that was deleted

- The pool is **`kMaxParticles = 64` per emitter**, not a scene budget of 1000. **The 1000 figure was
  carried in from nowhere and is deleted rather than corrected.**
- `danm14ab` measures **232 live quads at frame 1200** with the vents filled — **464 triangles**. With
  grass the frame goes 86k → ~90k.
- The divergence that figure implied is closed: raster used to draw only the first 64 in one call while
  the registry could pass more; it now issues uniform-sized batches over the complete list, so both
  renderers see the same particles. **64 is the uniform-block capacity, not a scene limit.**

### 3.10 Determinism and denoiser measurements

See §1.6 and §1.7 for the two findings. The numbers, for reference: raster **byte-identical** across
three runs each of `danm14ab`, `ebo_m12aa` and `danm13` in both PBR and retro once rows 1040+ / cols
1600+ are masked (six groups, six hashes); path tracing nondeterministic at **8–64% of pixels**; NRD
alone contributing **sd 0.14043** of mean luminance against **0.00063** with denoiser and FSR off.

Other standing figures:

- FSR convergence baseline to beat: **settled mean|d| 0.30, edge 1.59, decay 17×.**
- Residual diffuse demodulation leak: `corr(high-freq)` **0.145**, down from **0.271**, visually clean
  but non-zero.
- Diffuse channel outliers: **max 25.3 while p99.99 is 1.30** — `1/(1-p)` is unbounded as specular
  probability approaches 1 at grazing angles. Bounded, unlike the 0/0 it replaced, but a firefly
  candidate under motion.
- Retro versus PBR captures of the same module differ by **77% of pixels and 26 levels of mean
  luminance**, and the retro image is correct down to skinned characters and weapons.
- Sky cubemap keeps only **0.73** of the geometry sky's horizontal detail at 1024/face; **2048 only
  reaches 0.77 for 4× the memory**, so the residual is resampling and filtering, not resolution.
- NRD history: we run **6 frames because 63 only buys 6%.**
- Specular divided by `Fenv` (~0.04) is **~20× over range and clips to white**, so the channel debug
  view cannot be read at all without its own exposure.
- The frame floor outside the renderer: the update slot is a flat **~2.03–2.07 ms** across every backend
  and pipeline, and raster adds **~1.3 ms** of queued CPU work. Neither moves with renderer changes.

### 3.11 The GL removal, in numbers

**184 files, 13,420 lines deleted against 346 added.** Four commits, not the three planned: porting the
toolkit turned up a fourth. Presenting a scene needed forty lines of Vulkan scaffolding that only
`engine.cpp` knew how to write, so a second host could not present at all; that moved into the renderer
as `begin2DRendering`/`end2DRendering`/`presentSceneOutput`.

**Two estimates were wrong in the same direction.** The toolkit port was sized **L** and took one
commit, because every seam already existed. The deletion was sized at **3537 lines and came to
10,043**, because the counts were taken from an older tree. **Both errors came from reading the plan
instead of the code.**

What the deletion covered, with line counts:

| delete outright | lines | why it can go |
|---|---:|---|
| `graphics/context.cpp` | 381 | not constructed under Vulkan |
| `graphics/shaderprogram.cpp` + `shader.cpp` | ~200 | GLSL only |
| `graphics/framebuffer.cpp` + `renderbuffer.cpp` | 165 | Vulkan uses its own images and render passes |
| `graphics/uniformbuffer.cpp` | 50 | Vulkan uses its own ring |
| `pipeline/pbr.cpp`, `pipeline/retro.cpp` | 852 | GL pipelines |
| `pass/pbr.cpp`, `pass/retro.cpp` | 634 | GL executors |
| `renderer/gl2d.cpp`, `extern/glad` | — | GL |

**The GL blast radius was far larger than the "3 call sites" the first draft claimed.** That number
counted literal `currentBackend()` calls and missed the branches: `engine.cpp:199-268`,
`window.cpp:31-91`, `di/module.cpp:68-91`, `provider/shaders.cpp:87-98`, `texture.cpp:119`,
`mesh.cpp:36`, `uniforms.cpp:34`, `movie.cpp:90-105`, `graph.cpp:647-654`, `editor.cpp:940-943`. Plus
**55 backend/GL references across ten files under `src/apps`** — and they were not incidental: **the
engine selected and tore down a *different ImGui renderer* per backend.**

**Only three types genuinely survived**, and only because they carry CPU-side asset data Vulkan uploads
from, not because they abstract a backend: `Texture` (image data and features), `Mesh` (geometry and
vertex layout), and `Uniforms` (the CPU mirror `pipeline/vulkan.cpp:1319-1335` already reads).

Three GL consumers earlier drafts missed entirely, found by a second review, one of which changed the
size of the phase: the wx toolkit driving the full GL pipeline through `wxGLCanvas`; the profiler
calling `context.useProgram` outside any backend branch; and `Editor::renderRTPreview`. **The toolkit
port was decided rather than dodged** — the preview is one of the toolkit's eight viewers, and the
other seven plus the batch tools are pure data work that never touches graphics, so **deleting the
renderer to spare a port would take the one viewer that cannot be replaced by reading the file.** It
was ported by presenting into its wx panel directly rather than the offscreen-plus-readback earlier
notes assumed: SDL3 adopts the panel's native handle, **so there is no second render path.**

### 3.12 Containment and scope figures

- **575 `Vk`/`vk` references live in `libs/scene`**, four public headers under `include/reone/scene/`
  include `volk.h`, and `scene/render/` is in practice a second Vulkan renderer hosted in the scene
  library.
- `scene/registry.{h,cpp}` was **1,047 lines**, created by `c536ac72` on `path-tracing`, **absent from
  `master`**.
- `editor.cpp` carried **64** registry references; the table of "where the registry's other jobs go"
  accounted for about four of them.
- The vendored FSR 3.1.4 SDK in `build/_deps/ffx-src` is **436 MB** of dead weight from the abandoned
  FSR3 attempt.
- `include/reone/game/effect/` holds **89 headers against 64 `.cpp`**, so 25 effect classes have no
  implementation file at all **and do not show up in a stub count.**

---

## 4. Rejected approaches, and why

### 4.1 Retained registration, rejected in favour of the snapshot

Earlier drafts treated a per-frame rebuild as a stopgap and fully retained registration —
`registerObject`, `unregister`, update paths — as the inevitable end state. **That was asserted, never
argued, and it is wrong. The snapshot is the architecture.**

Three needs get conflated under "retained":

1. **stable object identity**, so a cache can be keyed across frames;
2. **persistent renderer caches** — BLAS, uploaded buffers, descriptors;
3. **a retained scene-to-renderer registration protocol.**

**Ray tracing needs the first two. It does not need the third**, and each of them is satisfied by a
snapshot that carries stable ids:

- **Rigid BLAS keys on `graphics::Mesh`, not on a scene node.** The bulk of the scene is rigid, one
  BLAS serves every instance, and mesh lifetime belongs to the resource layer. The pattern already
  exists and works: `VulkanResources::_meshes` is an `unordered_map<const Mesh *, ...>`, populated on
  demand and dropped in bulk.
- **Per-node BLAS keys on object identity**, which a snapshot entry can carry as a plain id. The cache
  lives in the backend; an id that stops appearing ages out. Nothing about that requires the scene to
  announce a departure.
- **The TLAS is rebuilt every frame regardless.**
- **Temporal accumulation needs motion vectors**, which come from `prevTransform`, already on the entry.

Against that, retained registration costs **an invalidation contract**, whose failure mode is a stale
value that is much harder to find than a missing draw call.

And it costs more than that here, because **the scene graph cannot currently emit the events it would
need.** Creation is not the problem — every node is built through one factory,
`SceneGraph::newSceneNode`, so the "object entered the graph" hook already exists and is universal.
**Destruction is the problem**: `_nodes` holds a `shared_ptr` to every node ever created and is *never
erased from and never read*. `clear()` drops the five root lists and leaves it untouched. **Nothing in a
scene is ever destroyed until the `SceneGraph` itself dies.** So "unregister when the object leaves the
scene" has no event to hang on, because leaving the scene is not currently a thing that happens. Fully
retained registration would require centralising attachment and detachment, defining ownership of
children, and making reparenting and bulk destruction emit correct removals — **a scene-graph ownership
project that buys nothing the snapshot does not already give.**

**The asymmetry is the whole argument. Under a snapshot, the `_nodes` leak is a memory bug to fix on its
own schedule. Under retained registration it is a hard prerequisite that gates the path tracer. Pick the
architecture where the scene graph's existing weaknesses stay bugs instead of becoming blockers.**

What survives from the retained framing is the part that was always the real requirement: **identity is
stable**, assigned at `newSceneNode` and carried on every entry; **everything else is rebuilt** —
transform, previous transform, bones, dangly positions, materials, instances — so there is no
invalidation to get wrong because there is nothing to invalidate; and **backend caches are validated,
not notified.** An entry carries a content version alongside its id; the cache rebuilds on a mismatch
and releases on an id absent for N frames.

**Age-out alone is not sufficient** — a live object that swaps a texture keeps its id forever. Not
hypothetical: `Creature` swaps main texture and environment map on live nodes
(`src/libs/game/object/creature.cpp:1239-1248`), and `invalidateTexture` exists precisely for content
changing under an unchanged identity. **The difference from a retained protocol is that the snapshot
*reports* the version where a retained scene would have to *announce* the change.**

Revisit only on measurement, and reconsider by moving specific expensive fields out of the snapshot,
**not by adopting a retained protocol wholesale.**

The same conclusion constrains the registry's removal: **"nodes admit directly" must not quietly become
a retained protocol.**

### 4.2 Per-node BLAS and refit scheduling — abandoned on purpose

The deformation compute pass landed (`slang/scene_resolve.slang` — dangly `:217-221`, saber `:223-228`, skinning
`:231-257`). **Per-node BLAS did not land and is not wanted**: `9b98c37c` replaced 714 structures with
one, and a grep for `refit` or `MODE_UPDATE` across `src/` and `include/` returns nothing, **so no refit
path exists to hang them on.**

The schedule that was designed and is now moot:

| | when |
|---|---|
| static room geometry | once, at load |
| rigid props | on move, or never |
| skinned | refit per frame while visible and animating |
| dangly | refit per frame, subject to the motion threshold |
| TLAS | rebuilt per frame regardless — it is cheap and everything moves through it |

**Refit is not rebuild**, and the distinction was the whole optimisation: a refit keeps the existing
tree and moves its vertices, costing a fraction of a build, **but degrades traversal quality as the pose
drifts from the one the tree was built for.** So refit per frame, rebuild occasionally — on a large pose
change, or every N frames staggered across objects so the cost does not land in one frame. **One BLAS
rebuilt in full every frame deletes the refit-quality problem outright.**

The axis that mattered was never "skinned or not" but **whether a BLAS can be shared between
instances**:

| category | BLAS | keyed on | per frame |
|---|---|---|---|
| rigid mesh | shared | `Mesh` | nothing |
| skinned / dangly / saber | one each | scene node | refit |
| grass / particles | one unit quad, static | — | TLAS instances only |

For grass and particles the plan preferred **one static unit-quad BLAS plus N TLAS instances** over
baking instances into a per-frame BLAS: the TLAS is rebuilt every frame regardless, so it costs nothing
extra and avoids rebuilding a BLAS over hundreds of thousands of triangles. The cost is instance-buffer
bandwidth, 64 bytes each. **This is superseded by the merge**, which puts the quads in the merged stream
outright.

### 4.3 The dangly motion threshold, freezing what is not moving, and baking the wind loop

All of these were the response to **653 dangly meshes each computing vertex positions on the CPU every
frame, allocating a vector for them, and under ray tracing needing its own BLAS refit every frame — six
hundred refits a frame for leaves.** Three things worth trying, cheapest first, none of which shipped
because one merged BLAS removed the problem they addressed:

- **Freeze what is not visibly moving.** A dangly mesh whose positions have barely changed since its
  last build does not need a refit. A per-node motion threshold turns most of the foliage static for
  most frames, and the data to decide it is already computed — the displacement is right there in
  `_dangly.vertices`.
- **Ask first whether it needs simulating at all.** It is a real spring-damper — per-vertex velocity and
  displacement integrated across frames — but look at what drives it. The artist supplies
  `displacement`, `tightness`, `period` and per-vertex `constraints`. **The forcing is only two terms:
  the object's own motion, and wind. Wind is not authored** — it is hardcoded as
  `0.01f * abs(sin(_windTime))` along world X — **and `_windTime` starts at zero on every node and
  advances by the same `dt`, so under a fixed timestep every dangly mesh in the scene sits at the same
  phase forever.**

  For a stationary tree the motion term is zero, so wind is the only input and the response is periodic
  with a 2π-second loop. **The displacement field is then a pure function of (mesh parameters,
  orientation, phase)** — not per-instance state in any meaningful sense. **The same few answers are
  being recomputed 653 times.** That points somewhere better than a faster simulation:
  - **bake the loop** — precompute the cycle once per distinct mesh and sample it, and the per-frame
    integration disappears;
  - **share the result** — two instances of the same tree at the same orientation have bit-identical
    geometry, so they can share one BLAS with the TLAS carrying the transform, which is the rigid case
    again and collapses hundreds of per-node structures. **Orientation is the wrinkle**: the wind is
    rotated into object space, so instances at different rotations diverge. Simulating in world space
    would remove that dependency and widen sharing to every instance of a model — **worth checking
    whether it changes the look before assuming it is free**;
  - **the in-phase wind is arguably a bug of its own.** Every tree waves in perfect sync because nothing
    offsets `_windTime` per node. Seeding it from the node's interned name or id would cost nothing and
    look better, **though it would also destroy the sharing above — so decide which is worth more before
    doing either.**

  **Note the CPU cost is *not* the argument here**: dangly allocation and fill were measured at 0.065
  ms/frame combined. **The win is the BLAS count, which is a ray-tracing concern, not a frame-time one.**

- **Move the simulation to compute.** Still right for whatever survives the above, and required
  regardless for skinning: the positions must be GPU-resident for tracing, so computing them on the CPU
  and uploading is paying twice. **It is the same pass skinning needs — build the mechanism once and
  give it two kernels.** What it needs, and what it breaks:
  - **Per-node persistent state.** The simulation integrates across frames, so the buffer has to survive
    between frames and be found again next frame — the first thing that genuinely consumes stable ids
    and the cache-version rule rather than carrying them.
  - **Static inputs upload once.** Base positions and per-vertex constraints never change; only the
    transform, previous transform, wind time and timestep vary.
  - **It will change the rendered image.** Floating-point on the GPU will not reproduce the CPU's results
    bit for bit, and the simulation integrates, so **small differences accumulate rather than cancel.
    Every verification in this project so far has rested on bit-identical frames, and this is the first
    change that cannot meet that bar. Decide the replacement standard before starting** — a bounded
    per-pixel difference over a foliage region, or a deliberate re-baseline with the old images kept —
    rather than discovering mid-change that the usual check no longer applies.
  - **Determinism must survive.** Captures have to stay reproducible run to run even if they no longer
    match the CPU path, so the kernel must not depend on dispatch order or uninitialised state. **Two
    runs at the same frame must still be byte-identical to each other.**

The residual that outlives all of it: the spring is still solved on the CPU and its output uploaded
every frame for 653 dangly nodes; moving it into the merge dispatch, which already writes the dynamic
slice of the merged buffer, **deletes the pass and the per-frame upload together.**

### 4.4 One object per body part — the trade that no longer exists

Under rasterisation `c_drdwar`'s 30 entries are 30 draw calls; under the old per-mesh TLAS they were
also 30 instances for one droid. Two distinct cases that wanted opposite things:

- **Rigid characters** — droids assembled from rigid parts, each with its own transform. Expressing them
  as a single skinned mesh with bone transforms would collapse 30 instances into one BLAS plus a refit.
  **The trade was real**: rigid parts currently *share* their BLAS across every instance of the model,
  so eight `c_khounda` cost 23 structures and 184 instances; as skinned they would cost eight
  structures, eight instances and eight refits per frame. **Fewer instances, but sharing is lost.**
- **Genuinely skinned characters** already deform per node, so merging the parts of one character into
  one BLAS costs nothing and removes instances outright. **There is no sharing to lose.**

**The trade is dead**: there is one TLAS instance for the whole scene, so the tracer got this for free
and **the remaining cost is raster-only — a straightforward draw-call reduction with no instance/sharing
trade attached.** Either way the merge is per *model*, not per node, and wants the model root.

### 4.5 An "invariant" partition of the snapshot, and why it was not built

If an object cannot change, re-registering it every frame is waste. **But not through
`Material::staticObject`, which says nothing about the material.**

What would work is a stricter, computed test: a node is **invariant** when its transform is fixed *and*
nothing feeding its material moves — no UV animation (`mesh.uvAnimation.dir` zero), no cycling bumpmap,
no alpha or self-illum controller. **That is decidable once, at init, from data already loaded.**

**Two cautions before building it.** The invariant part still needs to be *in* the snapshot every frame,
because passes select from it — **so this saves the building, not the walking, and the walking is the
larger half** (0.445 against 1.873, §3.6). And it reintroduces exactly the invalidation contract the
snapshot was chosen to avoid; the difference is that the predicate is computed from immutable data
rather than maintained by hand, which is what makes it tractable. **Measure the invariant fraction of a
real area first: if it is not most of the 1508 entries, the complexity is not worth 0.4 ms.**

And it is **worth doing after the traced image is correct and measured, not before**: it is an
optimisation whose whole benefit is traversal cost, and there is no traversal cost worth optimising
until the thing renders what it should.

### 4.6 What `Material::staticObject` actually means — check the flag before relying on it

`setStatic(true)` is called in exactly one place — `src/libs/game/object/area.cpp:472-488` — over room
model nodes, **skipping anything under the room's `"{modelName}a"` subtree, which is where its animated
geometry lives.** So:

- **it is a room flag.** `setStatic` runs inside `Area::loadLYT` over the layout's rooms, and nothing
  else is ever marked. **That reads like an under-count until you ask what else would qualify**: a
  placeable opens, a door swings, a creature walks. Room geometry outside the room's own animated
  subtree may be exactly the set that never moves, in which case the flag is drawn correctly rather than
  drawn short. **Do not widen it without checking what each candidate does when the player interacts
  with it — "it has not moved yet" and "it cannot move" are different claims, and only the second one is
  safe to bake into a BLAS.**
- **it constrains the transform, not the material.** A static node still runs `updateUVAnimation` and
  `updateBumpmapAnimation`, **so scrolling water in a room is static by this flag while its `uv` and
  `bumpMapFrame` change every frame.**

The framing "an advisory flag turned load-bearing" is itself out of date: the admission path already
declines to trust it, in as many words — *"an authored room hint, not the admission proof required to
retain geometry"* (`render/pipeline/rayquery.cpp:221`) — and publishes everything as dynamic. **So
nothing currently depends on it being right.** The open question is whether it can be trusted enough to
found a static merge on, which still needs static-and-never-moves separated from
static-and-currently-still.

### 4.7 Grass and particles staying out of the acceleration structure — rejected twice

The second review argued they should stay out and the cleanup plan briefly agreed. **That was wrong, and
the reason is worth naming because it is the same mistake twice: the review treated current behaviour as
a constraint when it was a known bug.**

The decisive fact: the tracer matched only `RegisteredMesh` — three `std::get_if<RegisteredMesh>` sites,
no handling of `RegisteredGrass`, `RegisteredParticles` or `RegisteredBillboard` anywhere. So **grass
and particles were invisible to the path tracer: they cast no shadow, appeared in no reflection,
occluded nothing, and a ray passed straight through a hillside of grass. That is a correctness gap, not
a design choice.** They have to be in the BLAS, and the merged scene *is* the BLAS input. There is no
other way in.

The third review then argued that grass and particles are camera-dependent by construction —
`grassVertex` derives its axes from the camera and expands the quad in the vertex shader, particles
likewise — and that a pose correct for the primary camera is not correct for a reflection or shadow ray
arriving from elsewhere.

**True, and not a reason to do anything differently. The orientation error on a grass blade seen from a
bounce ray is small; the error from grass not existing in the acceleration structure at all is total.**
Cluster selection being camera-position-dependent is likewise a small effect at 32 m. Both are bounded
approximations worth accepting to get one representation, and neither justifies a second geometry path
or a per-class lowering. **So: build the quads with the axes available at merge time, admit them, and
record the approximation. If it ever visibly matters — grass swimming in a mirror — revisit it *then*,
with the artefact in hand rather than in principle.**

The same argument settles the camera-facing-particle question outright: **a rasterised composite over
the traced image cannot be in the BLAS at all.** Particles are admitted as camera-facing quads in the
merged geometry (`2fd6e713`, Phase D closed with `225383a2`). The "gates emitter admission to the TLAS"
clause was false even when written — admission never waited on that decision.

### 4.8 The four "equally useful to raster" claims — three of them wrong

The cleanup plan's first draft asserted that four things the tracer builds were "equally useful to raster
and to tracing, and only live in the tracer for historical reasons." **Three of those four are wrong,
and the fourth is only true in the future tense.**

| claim | verdict |
|---|---|
| merged world-space geometry | plausible future shared output, **not consumable today** — raster builds pipelines from each mesh's own vertex layout (`pass/vulkan.cpp:167-170`) and draws object-space vertices through `LocalUniforms.model` (`slang/pbr_model.slang:56-79`) |
| opaque/non-opaque partition | **not the same split.** Trace partitions on `surfaceType`, sky and punch-through (`rayquery.cpp:1361-1371`); raster partitions on `RenderCategory` and `Material::blending` (`vulkan.cpp:658-661`). They overlap and are not equivalent |
| per-triangle material id | useful future *identity*, but no raster shader reads such a table; raster binds textures and fills `LocalUniforms` per draw (`pass/vulkan.cpp:172-199`) |
| `InstanceMaterial` table | **wrong to share.** Trace-only surface types and curated trace operations. Raster reads the original `Material`, including `color`, `ambientColor`, env-map slots, fog and blend/cull state that `InstanceMaterial` does not carry |
| bone pool | **wrong.** Raster uploads ≤24 `glm::mat4` per draw into the uniform ring (`pass/vulkan.cpp:274-297`). And the merged result is already world-space skinned, so a raster draw of it needs no palette at all |

Also corrected: `pass/pbr.cpp` and `pass/retro.cpp` are **not** backend-agnostic. They have no raw `gl*`
calls but drive `Context`, `ShaderProgram`, `Mesh::draw` and GL `Uniforms`; Vulkan uses a different
executor entirely. **The first draft offered "these survive untouched" as an invariant to check the work
against. It was a false invariant.**

And **"one path" means one canonical output, not one upload.** An earlier revision said the merge was an
alternative to per-mesh `VulkanMesh` uploads and that collapsing to one path would make them stop
existing. **That is factually wrong: the merge *reads* those uploads** — `SceneObject` carries
`srcVertexAddress` and `srcIndexAddress` pointing at them. Source geometry has to reach the GPU before
anything can merge it. The honest shape is two levels:

```
CPU Mesh asset
  -> source mesh cache        device-addressable per-mesh data, uploaded once
  -> GpuScene merge           per frame -> canonical world-space triangle stream
  -> consumers                tracer today, raster after Phase F
```

**So what Phase F actually deletes is raster drawing directly from per-mesh buffers, not the uploads
themselves.** The per-mesh path stops being a draw path and becomes a source cache — a smaller and more
accurate claim. The duplicated raster skinning does still go: `node/mesh.cpp:327-354` builds palettes
every frame and `pass/vulkan.cpp:274-297` uploads them per draw, while the merge has already produced
world-space skinned vertices.

### 4.9 Claims closed as wrong, unfalsifiable, or superseded

Kept so they are not re-proposed:

- **"Retro pipeline has no Vulkan counterpart"** — wrong; it always had one, `VulkanRenderPipeline`
  branches on `options.pbr`. The factory used to warn "No retro pipeline on Vulkan" and then build the
  same pipeline anyway; that warning is deleted (`6a681168`). Retro and PBR captures of the same module
  differ by 77% of pixels and 26 levels of mean luminance, **and the retro image is correct.**
- **"Resolve alpha and retro post-processing order differ from GL"** — **unfalsifiable, closed.**
  `df1aa375` removed the comparison target, so the claim can no longer be tested or even stated. Nothing
  is known to be wrong with the Vulkan ordering; **if it is ever suspected, re-file it as a claim about
  Vulkan with its own justification rather than resurrecting it as a parity gap.**
- **"Render-target viewer returns an empty list on Vulkan"** — wrong; `targets()` returns a populated
  list and is empty only before `_inited`. The real defect was a null `_gbuffer` dereference, since
  fixed.
- **"`Texture::flush` throws for most pixel formats"** — **no such method.** It was `flushGPUToCPU`, and
  `df1aa375` deleted it with the GL backend. If CPU readback is still wanted it is a claim about the
  Vulkan path behind `--dumptargets`, which nobody has shown to be broken.
- **"Shadow passes cull against the view camera"** — fixed 2026-07-28 by `49496d2f`, two days before the
  backlog that described it was compiled, **so it was never true of the tree it described.** "Affects all
  three pipelines" was wrong regardless — **there is one pipeline implementation, and `RenderMode` has
  three values.** The residual is real and narrower: draw-distance culling still measures against
  `visibility.drawDistanceCamera`, the view camera even in a shadow pass, so a caster far from the eye
  and close to the light is dropped after the frustum correctly kept it. **The remaining half must not be
  silently reproduced when selection becomes ranges.**
- **The additive shadow-occlusion gate** — superseded by `9b98c37c`.
  `VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR` appears nowhere in the tree; per-instance opacity became
  two BLAS geometries, and the surviving predicate is keyed on `surfaceType == 1` and bit **24** (sky),
  not 25. **The dead-bit reasoning is dead too**: bit 25 is now live as `kPtMaskBlendedCoverage`. The
  sweep it asked for — feature bits nothing sets, guarding behaviour that looks correct in source — was
  run and came back clean.
- **The 2048 grass pool, the 1000-particle budget, the 1482/55 counts, the "12 vs 96 pixel-identical"
  transmission note, the whole-frame parity baseline for `danm14ab` frame 900** — all superseded or
  taken against a tree that no longer exists. **A stale figure is worse than no figure**, because it
  reads as evidence.

---

## 5. Standing rules and traps that came from pain

### 5.1 Gate work on a consumer

**Light-frustum culling for the shadow passes** was written as "one argument at the `drawScene` call
sites". It was in fact `graphics::Frustum` extracted out of `Camera` plus a `VisibilityPolicy` on
`drawScene`, **+191 net lines across 16 files, to admit two extra shadow casters that change zero pixels
at frame 900 of `danm14ab`.**

The code is right and the frustum extraction removes real duplication. **The *sequencing* was wrong**: a
later step said it "generalises step 3", which is the tell that they were always one piece of work.
Nothing in the four steps that followed needed light-frustum shadow culling, **so the abstraction was
paid for four steps before its first consumer and shipped as a bug fix that fixes nothing measurable.**
`VisibilityPolicy::noCulling()` existed with **no callers** for the same reason.

> **Gate the rest on a consumer. A step that only widens a seam for a later step should land with that
> step, or immediately before it — not at the position where the idea first occurred.**

Applied correctly the next time: stable ids landed together with interned model and node names,
per-entry pass flags, and the panel that reads all three, **because the panel is their only consumer —
the gating rule applied rather than bent.** Likewise, backend caches keyed by id *and* content version
land with the deformation pass, which is their only consumer.

The counterfactual cost was named at the time: two steps together were about **+256 net lines whose
entire justification is later steps; if those stall, that is dead abstraction in a shipping renderer.**

### 5.2 Delete, do not hollow

**The job is simplification, so the measure is what *disappears*, not whether the old thing is
unreachable.** Most of the GL-shaped types had no Vulkan twin because Vulkan never used them — DI skips
`Context` under Vulkan, shader provisioning skips all GLSL compilation. **They are not abstractions to
collapse later; they are dead weight now.**

The corollaries that were applied: **the launcher must not keep a disabled or single-entry dropdown —
the option goes, not just its second choice.** And **what genuinely stays out of the merged scene should
be deleted rather than excluded**: AABB and debug entries were already `warnOnce` stubs.

The related scope rule, decided 2026-08-01: **anything called a registry that is not on `master` is
removed.** `scene/registry.{h,cpp}` is branch-only, so it goes whole rather than being pared down;
`graphics/meshregistry` and `graphics/textureregistry` are master's and are untouched.

### 5.3 Two numbers differing by less than the spread of either are not a result

One unchanged build measured `ebo_m12aa` across **6.26–7.09 ms over five samples — a 12.2% spread**.
**Treat a difference under about 10% on a single module as unproven**, and note that this applies to
every row of the frame-time table, not only to the one that was reverted.

The same trap in another shape, from the sequencing principle: **whatever the merged stream is going to
carry must be carrying it before raster reads it.** Add a class while the tracer is the only consumer
and the change is deliberate and inspectable against the traced image alone; add it after raster has
switched and **a difference could be the new consumer or the missing class, with no way to separate
them. That is the same trap as comparing two numbers that differ by less than their spread.**

### 5.4 Build and measurement traps

- **Capture with `--dev 0`** and `--grassdensity 1`. **`reone.cfg` is graded away from defaults and wins
  every flag not passed.**
- **The FPS readout is in the image and forges its own diffs.** Suppress wall-clock readouts under
  `isCaptureRun`, or mask rows 1040+ / cols 1600+ before comparing.
- **A run immediately after a build pays a one-time shader and pipeline cache cost** that inflates the
  300-frame baseline and silently deflates the difference. **That produced a nonsense 1.755 ms reading
  once.**
- **`checkIdentityStability` fires whenever the Graphics channel is on** and exists only after
  `296a0474`, so instrumentation compared across that boundary has to log somewhere else.
- **`spirv-dis` aborts on unknown capabilities** (word 4, `Invalid capability operand: 5388`), so any
  disassembly-based check reads a truncated module and confirms whatever absence it was testing for.
  **Walk the instruction stream.**
- **Two `slangc` binaries on this machine disagree, and the newer one is broken.** Test with the compiler
  the build actually uses.
- **Traced output is nondeterministic; compare distributions, never a stored figure.** And run
  `--ptdenoise 0` when the question is the tracer, because NRD is the whole of the run-to-run variance.
- **NaN renders as black, white or garbage depending on the path it takes.** Measure with
  `numpy.isfinite` on a `--dumptargets` `.npy`, not by eye.
- **Renaming an ImGui window changes its `imgui.ini` key**, so the saved layout, size and dock position
  reset to code defaults. **That reads as the panel breaking.** Delete `build/bin/imgui.ini` and check
  what a first run actually shows before believing a layout regression.
- **A red build trains everyone to ignore red builds.** `toolkit.exe` failed to link from `b991a197`
  onwards, which is how `tests` stayed broken across four commits.
- **A whole-frame statistic cannot see small localized content.** 55 saber-spark quads cannot move a
  whole-frame mean luminance by more than 3e-6; **use counts.**
- **Read the code, not the plan.** Both of Phase A's wrong estimates — the toolkit sized L that took one
  commit, the 3537-line deletion that came to 10,043 — **came from reading the plan instead of the
  tree.**
- **Each increment must hold its bar on its own.** A change spanning three steps cannot be bisected when
  the hash moves, and the hash will move.
- **The graphics slot measures CPU time recording the frame, not the GPU executing it** — which is why
  PBR and Retro land within 2–4% of each other despite very different GPU work. **All current numbers are
  CPU-side; GPU timing in `IStatistic` is the blocker for any credible performance claim.**

### 5.5 The five architecture boundaries

Everything in the registration plan followed from these, and anything that did not fit one of them was
out of scope rather than an oversight.

**Ownership: the scene publishes, the renderer derives.** `SceneGraph` builds a complete render snapshot
each frame and owns it. The renderer consumes it and owns only what it derives — acceleration
structures, descriptor state, uploaded buffers. **The renderer never owns scene objects, and the scene
never holds a backend handle.** This is a decision, not a placeholder.

**Data model: four separate concepts, deliberately not one.**

| concept | lives on | lifetime | what it is for |
|---|---|---|---|
| object identity | `SceneNode` | creation to destruction | keying caches across frames |
| content version | snapshot entry | changes when the source does | telling a cache its contents are stale |
| renderable data | snapshot entry | one frame | what a pass draws or a TLAS instances |
| AS index | backend | one frame | TLAS instance index, 24-bit custom index |

Earlier docs blended these into "the handle". **They have different lifetimes and different owners, and
conflating them is what makes retained-mode designs fail** — in particular identity and version, because
**an id that is stable across a texture swap is doing its job correctly and telling a BLAS cache
nothing.** The debug names ride on identity; the material assignment is keyed by name and is a fifth
thing again, derived offline rather than per frame. **The id is not the AS index** — a TLAS instance
index is backend-local and rebuilt every frame; the 24-bit custom index constrains the mapping, not the
id's width.

**Backend: registration is neutral, tracing is not.** The snapshot and visibility live in `scene`, so
every mode gets them at once. Ray tracing is Vulkan-only and consumes the snapshot without any other
backend knowing it exists. **Nothing ray-traced may appear in the executor interface or in the
snapshot's vocabulary.** The later, stronger form of the same rule: **no Vulkan API outside
`graphics/vulkan`.**

**Update: the frame boundary is the only consistency point.** No publish/subscribe, no change
notification, no dirty flags. The snapshot is built in one traversal and is immutable for the rest of the
frame; **the renderer reads a completed snapshot or nothing.** Nothing in the renderer outlives the frame
except caches, and a cache entry is keyed by **identity plus content version**, both reported by the
snapshot entry. **Disappearance is handled by an id ceasing to appear; change is handled by a version
mismatch. Neither requires the scene to announce anything, which is what keeps destruction and module
transitions free of synchronisation.**

**Visibility: one query, many policies.** Culling is a policy applied to the snapshot, not a property of
an object — camera frustum, light frustum, distance-and-relevance for a TLAS, or none for a full-scene
bake. **Each new consumer selects a policy; none writes its own.**

The TLAS correctness rule that falls out of the last two: **every eligible snapshot object goes in — no
relevance test, no frustum test, and explicitly not routed through the culling that this whole
architecture exists to escape. Build it the expensive way first; a reflection missing geometry is not a
performance result you can interpret.**

### 5.6 The `GpuScene` boundary, as the review forced it

**`GpuScene` owns merged world-space geometry, stable primitive identity, and the lifetime of both.
Consumers own their own lowering.** Narrower than the first draft and the part that survives scrutiny.

Three boundary decisions:

- **Sky-room classification moves out; the bake stays in.** Leaving both in the tracer was internally
  inconsistent: detection decides which meshes are *admitted*, and admission is core. A classifier
  produces the decision and passes it explicitly to `GpuScene::update`; the cubemap allocation, six-face
  bake and sampling stay trace-side. **The consumer requests replacement, the core admits or excludes
  accordingly — the consumer never reaches back into admission after the fact.**
- **Curated overrides and `TraceClass` stay tracer-side.** They are documented manual trace
  classification with no raster counterpart. **The core keeps source material *identity*.**
- **The partition needs an explicit reason or it belongs to the tracer.** `geometryIndex` means "may be
  committed as opaque in hardware" versus "must run candidate semantics" — **that is ray policy.** Either
  the core publishes one canonical stream and the tracer derives its own ranges, or the tracer supplies a
  named `TraceIntersectionClass` the core merely lays out. **A core that emits the partition without
  knowing why has hidden ray policy behind a neutral name.**

Two things the review surfaced that the draft missed entirely:

- **Buffer usage and synchronization are part of the contract.** The geometry buffer was allocated
  without vertex/index usage and the merge barrier named only AS-build and ray-tracing reads. **A core
  cannot issue one trace-shaped barrier and call the buffer generally consumable; publication must
  declare consumer capability.**
- **The sky bake is a second raster implementation hidden inside the tracer.** It selects
  `pbr_model/staticVertex`, original vertex layouts and original mesh draws. **Decide explicitly whether
  it stays an original-mesh capture pass or gets a core view, or the tangle reappears.**

And one constraint that has to stay true deliberately rather than by accident: **`GpuScene` must not
require ray tracing.** The merge is a plain compute dispatch and the BLAS/TLAS sit on top of its output.
**The tracer layers acceleration structures over the scene; it does not define it.**

### 5.7 Raster is permanent, and Retro is orthogonal to hardware

**Raster is not the next GL.** It stays permanently, for two things that are easy to conflate and are
actually different axes:

| | modern hardware | no ray tracing |
|---|---|---|
| **path-traced look** | the destination | unavailable |
| **original look (Retro)** | raster, or a retro mode in the tracer | raster |

**Someone on a 5090 may choose the original look; that is an art-direction choice, not a fallback.** So
raster must render both looks *and* must work with no ray tracing at all. **It is load-bearing, not
transitional, and GL's fate does not apply to it.**

Two consequences. **Phase F is justified by hardware rather than elegance**: raster consuming `GpuScene`
matters most exactly where the machine is weakest — a mega-draw over merged geometry against 1048
per-mesh draws — which is also the configuration with the least headroom to waste. **But it is an
empirical question, not an assumption**: a per-frame compute merge on a GPU with weak compute may cost
more than the draws it saves, and **"raster keeps per-mesh draws on hardware where the merge does not
pay" is an acceptable answer.** The fallback is N draws over **the same `GpuScene` records**, not a
resurrected registry — **raster staying permanent for old hardware needs per-object records, which
`GpuScene` has; it does not need the selection machinery.**

**PBR and Retro are not two pipelines.** They are two configurations of one rasterizer over the same
geometry; `pipeline/vulkan.cpp` already branches internally on `_options.pbr`. **Unifying them is not
work** — but Retro must remain reachable on the traced path too, which is an argument for it being a
shading configuration rather than a pipeline.

### 5.8 Do not write new code into a building you are about to evacuate

Vulkan containment runs **before** the raster work, reversing an earlier revision. The argument for
putting it last was that a pure relocation's pixel-identical bar is only meaningful over code that has
stopped changing. **That is a real hazard and it is the smaller one.** The larger one is that the raster
work writes *new* Vulkan — buffer binding, descriptors, a widened barrier — and if containment has not
happened yet, **every line of it lands in `libs/scene` and has to be moved again.** Relocating first also
means the raster phase edits code that is already where it belongs, **so the two never touch the same
file in the same phase.**

### 5.9 Verification bars, stated per phase because they are not the same bar

Pretending otherwise is how a refactor stops being checkable.

| phase | work | bar |
|---|---|---|
| **A** | remove OpenGL | pixel-identical vs the Vulkan baseline |
| **B0** | one source buffer, offsets not addresses | pixel-identical — it is an addressing change, not a behaviour change |
| **B** | extract `GpuScene`, tracer as only consumer | pixel-identical; an ownership change, not a sharing claim |
| **C** | residency in the contract, not the implementation | pixel-identical |
| **D** | admit everything, and see it correctly | traced image changes deliberately, inspected per class; raster untouched |
| *(registry)* | delete `RenderRegistry` whole | **full raster image hash-identical, shadows included**; traced within noise |
| **E** | Vulkan containment | pixel-identical — it is a relocation |
| **F-geo** | raster consumes `GpuScene` | **G-buffer byte-identical at every increment**; shadows change deliberately and are judged by eye; the mega-draw must additionally be **measurably faster** or it is reverted |
| **F-vis** | raster becomes primary visibility for every mode | `g_buffer_depth` must exist and match across PBR and PathTracing; then modules with `sky = none` stay raster byte-identical |

**A1 was honest that it does not finish the job**: GL was still the default backend, **so an
uncategorised capture proves nothing — the only valid invariant is Vulkan-before against
Vulkan-after.**

**The registry-removal bar is the strongest in the document, and deliberately so**: nothing in that
change alters a draw — raster keeps drawing the same meshes with the same matrices, only the records come
from `GpuScene` instead of the registry — **so no arithmetic changes, and anything less than
hash-identical means something moved that should not have.**

**And one bar this project cannot meet**: GPU deformation is the first change whose output cannot be
verified by bit-identical frames. **Decide the replacement standard before starting.**

### 5.10 Structural facts any admission rewrite has to handle

Four things carried forward from the registration plan, each a decision that cannot be avoided:

- **A shadow-casting mesh registers twice.** Once from `registerRender` with its real material, once
  from `registerShadow` with a `DirLightShadow` / `PointLightShadow` material — **a pass wearing a
  material's clothes.** They say which shader to use, not what the surface is. **Entry count is inflated
  by every caster, and anything keyed off entries has to decide which of the two is real. One object
  should be one entry, with the pass selecting a shader.**
- **Skinned meshes never cast shadows at all.** `shouldCastShadows` excludes skin meshes on creatures
  outright, **so no character is shadowed from its bind pose.** What *does* silently lose its deformation
  is a dangly or saber caster, which is not a skin mesh and so passes the predicate. **Fixing that is not
  registration or admission work**: `slang/shadow.slang:25-33` has a single vertex stage taking
  `POSITION` and `localUniforms.model`, with no skinned, dangly or saber variant, **so deforming shadows
  need new shader entry points and pipeline keys.** Keep it separate.
- **Culling is now per light for the frustum, and still per view for distance.** See §4.9.
- **`SceneGraph::_nodes` never shrinks.** See §4.1. **A module transition leaks the previous module's
  entire node graph**, and until it is fixed no id generation can ever advance — **so anything relying on
  stale-id detection is relying on a field that never changes. Add the field, do not rely on it.**

### 5.11 Identity state boarding in the wrong house

Two pieces of state lived in the registry only because that is where admission happened, and both are
keyed by something that outlives the snapshot:

- **The kill switch proves the point.** Its own comment says it is keyed by `SceneNodeId` index "so it
  survives the per-frame re-registration" — **which is an admission that the state belongs to the
  object's identity, not to the snapshot.** Moving it to a stable-id table is a correction, not a
  workaround.
- **Curated materials are the third resident, and the largest** — per-node `TraceClass`, albedo
  multiplier, roughness and metallic modes with their parameters and weights, and emission mode, **keyed
  by model and node name**, persisted, and edited by the material editor. **Name-keyed, not
  snapshot-keyed**, so like the kill switch it moves out.

**So stable identity is load-bearing for the tooling, not only for the tracer.** If identity cannot be
made stable across a module transition, this is where it bites first: **the kill switch already requires
an identity that outlives re-registration, so the requirement predates the refactor.**

Two more layering problems that come due with them: **`graphics::Material::curatedIndex` is an `int`
indexing the *scene* layer's curated storage** — a graphics type holding an index into a scene-owned
table, **a layering inversion that predates all of this**; and editor state typed on the doomed class
(`Editor::_materialEdit` is a `RenderRegistry::CuratedMaterial` **by value, a live working copy held
across frames**, and `editor.h` includes `scene/registry.h`).

And one coupling that contradicted the tidy answer: **`drawnPasses` exists only because `drawScene`
writes it**, and is read by the panel as pass slots and by the "Hide fully culled" filter. **Ranges
produce no per-object drawn state, and a mega-draw produces none at all.** Claiming the admitted/drawn
distinction survives "because it is still real once raster draws ranges" **is asserted without a
mechanism, and the mechanism is exactly what is being deleted. Decide before deleting, not after.**

### 5.12 A name is a claim about what a thing is

"Registry" was the name of a thing that would not exist. It became **the Scene viewer** rather than
"Objects", because **once `SceneGraph` admits directly, what the panel shows *is* the scene, and naming
it after the objects it happens to list describes the old snapshot rather than the new source.**

The rename also changes what the panel *is*, not just what it is called: **reading "the previous frame's
completed render snapshot" was a property of the registry; a scene viewer reads the scene, which is
live.** Whether the panel keeps the one-frame lag deliberately or stops needing it is part of the same
decision.
