# Backlog

Everything outstanding, in one place. Compiled 2026-07-30 from `doc/vulkan-rt-backend.md`,
`doc/renderer-registration-plan.md`, `doc/vulkan-remaining-plan.md`,
`doc/vulkan-opengl-remaining-difference.md`, `.claude/skills/reone-diagnostics/SKILL.md` and
source TODOs, plus the unverified work left by the FSR/demodulation session.

**Priority** is about consequence, not enthusiasm:

- **P0** — something is wrong or unverified *right now*, and believing otherwise costs time later.
- **P1** — blocks other work, or is a correctness bug users would see.
- **P2** — real improvement, nothing blocked on it.
- **P3** — worth doing eventually; no cost to deferring.

**Effort**: S = under a session, M = a session or two, L = multi-session.

---

## 0. Unverified — claimed done, never measured

These are the dangerous ones. Each is work that exists in the tree and is *believed* to function
because it compiled, which this session repeatedly proved is not evidence.

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 0.1 | Run the FSR convergence harness and record the numbers | Claimed three times, verified zero. Baseline to beat: settled mean\|d\| 0.30, edge 1.59, decay 17x | P0 | S |
| 0.2 | Test the editor's Save buttons end to end | Config *keys* round-trip, but nothing has clicked Save and diffed `reone.cfg`. A save that drops the game path or audio volumes is worse than none | P0 | S |
| 0.3 | Record why growing `InstanceMaterial` caused `VK_ERROR_DEVICE_LOST` | Fixed by codex, root cause unread. The struct carries buffer device addresses, so a layout mismatch dereferences garbage pointers. Next person to add a field needs this written down | P0 | S |
| 0.4 | Rebuild and smoke-test the Debug configuration | Last built before the demodulation and FSR work. Debug links the checked VMA that catches allocations outliving the device — the one thing that would catch FSR's image lifetimes | P0 | S |
| 0.5 | Verify specular demodulation on chromatic `Rf0` | Untestable until now: every stock material is dielectric, so `specFactor` has zero chroma. The new metalness scale forces it | P1 | S |
| 0.6 | Measure a clean build before/after the build-speed work | Only an incremental number (5.1 s) exists, so the actual saving is unknown | P2 | S |
| 0.7 | Verify the additive shadow-occlusion fix | Fix is written but **not built or rendered** — the link failed on a locked `engine.exe` and the capture that followed used the stale binary. See below | P0 | S |

### 0.7 — additive surfaces were casting solid shadows

`ptShadowTransmittance` skips additive surfaces correctly:
`if (material.surfaceType == kPtSurfaceUnlitTransparent) continue;`. But that line only executes
for surfaces the ray query yields as *candidates*, and a candidate is only yielded when the TLAS
instance is non-opaque. The gate for that was

```cpp
if ((material.featureMask & ((1u << 25) | (1u << 26))) != 0)
    instance.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
```

and **bit 25 is never set anywhere** — that test was its only reference in the codebase. Bit 26 is
punch-through. Additive surfaces are identified by `surfaceType = 1` and carry no feature bit, so
any additive surface that was not also punch-through stayed opaque in the TLAS: the hardware
committed it without consulting the shader, `CommittedStatus()` returned `COMMITTED_TRIANGLE_HIT`,
and the function returned 0.0. A saber blade cast a solid black shadow.

The comment above the gate already stated the correct intent — "Additive surfaces always transmit…
Both must therefore reach candidates" — so the logic was right and the predicate was testing a dead
bit. Now keyed on `surfaceType == 1`.

Why it stayed hidden: the primary ray was unaffected. `ptTraceNearest` handles additive from the
*committed* hit, so the visibility walk stepped past blades correctly either way. Only shadow rays,
which depend on the candidate loop, were wrong.

**Worth a sweep for the same shape.** A feature bit that nothing sets, guarding behaviour that
looks correct in the source, is invisible to review and to any test that does not exercise the
exact path. Check whether bits 25 and any other unset bits in `featureMask` are referenced
elsewhere.

## 1. Path tracing — correctness

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 1.1 | Residual diffuse demodulation leak: `corr(high-freq)` sits at 0.145, not 0 | Down from 0.271 and visually clean, but non-zero means material still reaches the channel. NRD's floor remap accounts for part; the rest is unexplained | P1 | M |
| 1.2 | Specular `F` vs `Fenv` mismatch | Specular is energy-correct but its per-light Schlick term is not the preintegrated environment term it is demodulated by. Same class of bug diffuse had; deliberately not fixed | P1 | M |
| 1.3 | Diffuse channel outliers: max 25.3 while p99.99 is 1.30 | `1/(1-p)` is unbounded as specular probability approaches 1 at grazing angles. Bounded, unlike the 0/0 it replaced, but a firefly candidate under motion | P2 | S |
| 1.4 | FSR reactive and transparency-and-composition masks are null | AMD: without them FSR "handles these cases as best it can". Additive saber blades and particles are precisely those cases | P1 | M |
| 1.5 | Next-event estimation against the full scene light list | KotOR interiors are many small point lights; pure PT is unusably noisy there | P1 | L |
| 1.6 | Grass placement determinism (hash of face + cluster index) | A reflection showing a differently-populated hillside reads as a tracing bug | P2 | M |
| 1.7 | Camera-facing particles: AS proxy vs rasterised composite | Undecided, and gates emitter admission to the TLAS | P2 | M |
| 1.8 | Lightmaps treated as albedo where physically wrong | Revisit once real GI exists; `ptLightmapIntensity` is already graded to 0 | P3 | M |
| 1.10 | The light selection table is evaluated twice per path vertex over all 32 slots | `ptDirectLight` runs `directLightWeight` once to total and again to select. Now that the cutoff no longer short-circuits distant lights, both passes run in full. Caching 32 weights would halve it | P2 | S |
| 1.11 | Sphere lights are sampled over the whole subtended cone, not the visible cap | Directions on the far hemisphere of the emitter are sampled and shadowed as if they lit the surface. Correct for a small emitter, increasingly wrong as the ratio dial grows | P3 | M |

### 1.9 — point lights became spheres *(done, `085c5893`)*

Kept because the reasoning is the calibration's justification, and because the fitted constant is
worth being able to re-derive.

`slang/tracing/lighting.slang` had inherited two raster behaviours, one outright bug, and one hack
of our own — and they turned out to be a single fix:

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

**The unification.** Ω = 2π(1 − cos θ) with θ = asin(saturate(R/d)) is simultaneously the falloff
and the shadow cone, and inverse-square falls out of its far field with correct saturation up close
and no singularity at d = 0 — Ω caps at 2π.

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
inside the bounding radius is ∫₀¹ x²/(1+x)² dx = **3/2 − 2ln2 ≈ 0.1137056**. Exact as the emitter
shrinks to a point, ~1% off at ratio 0.2.

A free two-parameter fit was tried first and is instructive: it returns ratio ≈ 0.59 with 29% rms
error, because reproducing Odyssey's *flat near field* requires an emitter nearly as large as the
influence radius. Fitting the shape is the wrong objective — the shape is the bug. Only the scale
should be fitted.

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
discards, recovered a further 0.21 ms. See 1.10 for the remaining halving available.

**An earlier revision of this section claimed** `ptPointAngularSize` "treats these as sphere lights
for shadow sampling". It did not; it was a constant, and that was the whole point.

## 2. Path tracing — quality levers not yet pulled

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 2.1 | Feed NRD `IN_DIFF_CONFIDENCE` / `IN_SPEC_CONFIDENCE` | NRD: "essential to preserve responsiveness… should not rely solely on the anti-lag". Also the documented way to make a long history usable — we run 6 frames because 63 only buys 6% | P1 | M |
| 2.2 | Feed NRD `IN_DISOCCLUSION_THRESHOLD_MIX` | Per-pixel disocclusion threshold. Aimed squarely at foliage edges, where NRD dumps history every frame, without loosening rejection globally | P2 | M |
| 2.3 | Evaluate NRD's SH denoiser path + `NRD_SG_ReJitter` | The only route to restoring the jitter NRD's temporal passes suppress — which is jitter FSR wants. Costs 2 extra RGBA16f targets per channel plus a resolve | P3 | L |
| 2.4 | Channel debug views need their own exposure | Specular divided by `Fenv` (~0.04) is ~20x over range and clips to white, so the view cannot be read at all | P2 | S |
| 2.5 | GPU timing in `IStatistic` | Blocks any credible performance claim; all current numbers are CPU-side frame time | P1 | M |

## 3. Path tracing — not yet built

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 3.1 | Deformation compute pass + per-node BLAS (dangly before skinned) | 653 dangly vs 61 skinned nodes, and 544 of 586 transparent draws | P1 | L |
| 3.2 | Material PBR-ification: authored name→material table, then heuristic BSDF | Order is load-bearing — the authored map must precede the heuristic | P1 | L |
| 3.3 | Deforming shadow casters | `shadow.slang` has only a rigid vertex stage; skinned meshes never cast shadows | P2 | M |
| 3.4 | Shadow passes cull against the view camera, not the light | Casters outside the view frustum vanish from the shadow map. Affects all three pipelines | P1 | M |

## 4. Build and tooling

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 4.1 | ~~`toolkit.exe` link failure~~ | **Done 2026-07-30.** Had failed since `b991a197`; trained everyone to ignore a red build, which is how `tests` stayed broken across four commits | — | — |
| 4.2 | Decide `ENABLE_FSR` default / Linux support | FSR is the *only* AA now that the TAA is deleted, and it is OFF by default — so every Linux and default build renders with no anti-aliasing at all | P1 | M |
| 4.3 | Delete the vendored FSR 3.1.4 SDK from `build/_deps/ffx-src` | ~200 MB of dead weight from the abandoned FSR3 attempt | P3 | S |
| 4.4 | Ninja generator evaluation | MSBuild is the bulk of build time. Blocked on FSR2's external build hard-failing on non-VS generators | P2 | M |
| 4.5 | Runtime resolution change is unfinished | Cameras never re-apply projection; `GUI::_rootOffset` is computed only in `load`. Treat as restart-required until a virtual re-apply exists | P2 | M |
| 4.6 | Retro pipeline exposes no render targets on either backend | `--dumptargets` is empty for it, so every parity number is PBR-only | P2 | M |
| 4.7 | Render-target viewer returns an empty list on Vulkan | Stale descriptor sets on resize are a use-after-free no single-frame capture catches | P2 | M |
| 4.8 | Per-frame registry rebuild costs 0.445 ms (~9% of frame) | Unresolved whether removing it is worth the complexity | P3 | M |

## 5. Vulkan parity gaps

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 5.1 | ~~Retro pipeline has no Vulkan counterpart~~ **wrong, closed** | It always had one: `VulkanRenderPipeline` branches on `options.pbr`. Retro and PBR captures of the same module differ by 77% of pixels and 26 levels of mean luminance, and the retro image is correct down to skinned characters and weapons. The factory used to warn "No retro pipeline on Vulkan" and then build the same pipeline anyway; that warning is deleted (`6a681168`) | — | — |
| 5.2 | Movie playback, profiler and console unported to `I2DRenderer` | Also the 3D sub-scene behind menu panels, skipped because passes cannot nest | P2 | L |
| 5.3 | Debug AABBs never drawn on Vulkan | `retroAABBFragment` exists but nothing selects it and `aabbVertex` is not in the shaderpack list | P3 | S |
| 5.4 | Resolve alpha and retro post-processing order differ from GL | Invisible to RGB diffs but feeds compositing | P3 | S |
| 5.5 | ~~Retire the OpenGL backend~~ *(done, `3445fdc1`..`df1aa375`)* | 184 files, 13,420 lines deleted against 346 added. The toolkit was ported by presenting into its wx panel directly rather than the offscreen-plus-readback this entry assumed — SDL3 adopts the panel's native handle, so there is no second render path | — | — |
| 5.6 | ~~Toolkit model preview renders exploded geometry~~ *(done, `a89c6eeb`, `44e11344`)* | Never a rendering bug: the camera sat at a fixed 8 units for every model, so large ones enclosed it and small ones were specks — a 24× range in apparent size. Pre-existing since 44472f0e, unrelated to the Vulkan port. Worth remembering *how* it was found: four hypotheses were proposed and killed from screenshots before anyone made the preview scriptable, after which one capture settled it. The `--open`/`--capture` harness is the durable part | — | — |

## 6. Engine and game systems

Largely untouched by renderer work, and by volume the biggest block of deferred work in the repo.

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 6.1 | ~60 of 64 effect classes and ~15 of 42 actions are `// TODO: implement` stubs | The single largest body of unimplemented game logic | P2 | L |
| 6.2 | `SceneGraph::_nodes` never releases | A module transition leaks the previous module's entire node graph. Blocks id-generation work | P1 | M |
| 6.3 | Combat gaps: no Dueling / Two-Weapon Fighting, single damage effect per weapon, guessed K1 damage bonus, combat history never cleared | Visible rules divergence | P2 | M |
| 6.4 | Serialization gaps: effects/actions not deserialized for placeables and doors, items not deserialized, `LastDisturbed` unimplemented | Save/load correctness | P1 | M |
| 6.5 | Action bar missing Force powers, mines, TSL slots | Visible UI gap | P2 | M |
| 6.6 | Hardcoded 1.7 m line-of-sight / audio listener height | Should be appearance-based | P3 | S |
| 6.7 | `Texture::flush` throws for most pixel formats | Blocks any path needing readback of those formats | P2 | M |
| 6.8 | Assorted: low-HP animation variants, NPC selectability, per-frame selection overlay updates, listbox CTD, `s_male02` TSLRCM workaround, AABB NaN handling | Small independent fixes | P3 | M |

## 7. Docs and determinism

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 7.1 | **Raster half solved: it was the FPS readout.** Path tracing is genuinely nondeterministic | Raster capture runs are **bit-exact**. Every differing pixel sat in one 153×13 box in the top-right — the frame-time text at `editor.cpp:1583-1588` ("234.4 FPS 4.27 ms" vs "241.3 FPS 4.14 ms"). Mask raw rows 1040+/cols 1600+ and three runs each of danm14ab, ebo_m12aa and danm13, in both PBR and retro, are **byte-identical**: six groups, six hashes, no exceptions. Suppressing wall-clock readouts under `isCaptureRun` makes that unconditional. Path tracing stays nondeterministic afterwards at 8–64% of pixels, from acceleration-structure build order feeding an order-dependent accumulation loop — that part is real and needs distribution-vs-distribution comparison. This bug is behind several false regression reports during the GL removal; the diagnostics skill's "raster is byte-identical" was right all along and its 0.02% bound for tracing is the part that is wrong | P1 | S |
| 7.2 | `doc/vulkan-rt-backend.md` status block is stale | Dated 2026-07-26 and says the scene pipeline does not exist, while the tracer, NRD, FSR and this backlog all describe a working one | P1 | S |
| 7.3 | Whole-frame parity baseline for danm14ab frame 900 is invalid | Stale dump plus the removed Slang GL path; no replacement figure exists | P2 | S |
| 7.4 | Open design questions | Emitter granularity (per emitter vs per particle system); whether walkmeshes and AABB debug geometry should register at all; compute-skinning ownership; whether grass and particles are ever traced | P2 | — |

| 7.5 | **The traced frame is bistable, not noisy** | Sixteen runs of `danm13` path tracing, `--dev 0`, frame 310, on three consecutive builds. Mean luminance does not scatter — it lands in one of two states about **0.025 apart**, and the *low* state is the same on every build: 59.9783 / 59.9790 / 59.9777. What changes is how often the high state is hit: 7/16 before Phase B, 6/16 after it, **12/16 after Phase C**. Neither phase changed a shader and raster stayed byte-identical throughout, so this is one pre-existing defect whose frequency moves with CPU timing. That reframes the problem: it is not acceleration-structure nondeterminism to be tolerated statistically, it is a bug with two outcomes, and the low state is probably the correct one. Find what differs between them — likeliest a first-frame ordering race, capacity growth landing on a different frame, or a merge/consume barrier that usually happens to hold. Fixing it restores a real bar for traced comparisons, which today need 16 runs a side to say anything | **P1** | M |

## 8. Performance — from the Nsight trace of 2026-07-30

Read the trace before picking anything here. Frame 17.75 ms, of which
`rayquery:primaryRay` is **5.70 ms** — so roughly **12 ms, two thirds of the frame, is spent
outside the trace**, in 555 command-buffer events recorded before it. Throughput confirms it:
RTCORE 10.8%, SM 9.8%, compute-shader warp occupancy 7.2% with 30.2% of warps unallocated in
active SMs. The GPU is very nearly idle while the CPU feeds it a long chain of tiny serialised
work.

The shape of that chain is one iteration per skinned mesh: bind pipeline, bind two descriptor
sets, push constants, dispatch `vertexCount/64` groups, full pipeline barrier, build one BLAS.

**Read `renderer-registration-plan.md` §"What the per-frame rebuild costs" and its BLAS table
first.** Most of the strategy already exists there — refit-over-rebuild with a per-object
schedule, the dangly motion threshold ("six hundred refits a frame for leaves"), merged static
BLAS, leaner dispatch shape. The items below are what the trace adds to that, not a replacement
for it.

**The new finding is the 12 ms.** That plan budgets the *traced frame* and records the target as
met at 4 spp — 1.68 ms at the Ebon Hawk, 2.74 ms at the Taris cantina. Nothing in it accounts
for two thirds of a frame spent in command submission before the trace begins. Whether that is
scene-dependent (this trace is an exterior with grass, not an interior), a regression, or simply
never measured, is unresolved and worth settling before optimising anything: the plan also warns
that every earlier traced-frame figure was ~95% stats-counter serialisation, so this codebase has
form for measuring the wrong thing.

### Session of 2026-07-30: danm14ab went 14.66 → 5.38 ms, and 8.2–8.6 are gone

Five commits, each measured either side. The wall-clock harness is
`(t(900) − t(300)) / 600` after a discarded warm-up, three samples.

| commit | change | danm14ab | ebo_m12aa |
|---|---|---:|---:|
| — | before | 14.66 | 10.06 |
| `9b98c37c` | one BLAS, one dispatch, two buffers | 7.91 | 8.54 |
| `363c8770` | material record out of the traversal registers | 6.79 | 7.08 |
| `0079b2d8` | sky becomes the ray-miss case | 5.79 | 6.87 |
| `791363fe` | raygen ray tracing pipeline | **5.31** | **6.98** |
| ~~`395a7afe`~~ | ~~Shader Execution Reordering~~ — reverted, `b3fe1f3b` | — | — |

### SER does not work with the pinned compiler, and the win was noise

Recorded because everything about this failure looked like success.

**vcpkg slangc 2026.7.1 — the compiler the build actually uses — accepts `ReorderThread`, warns
that it is upgrading the profile to include `spvShaderInvocationReorderNV`, exits 0, and emits the
capability with no `OpReorderThreadWithHintNV` in the module.** The Vulkan SDK's *older* slangc
2025.17.2 emits both from identical source and flags. Two slangc binaries on this machine disagree
and the newer one is the broken one, so testing with the wrong binary proves the opposite.

The only thing that shipped was three extra validation VUIDs: `spirv-val` rejects capability 5388
because it does not know the extension, and the module declared a capability nothing used.

**The measured speedup was noise.** One unchanged build measures ebo_m12aa across 6.26–7.09 ms over
five samples — a 12.2% spread containing both the 6.98 "before" and the 6.20 "after". Three samples
either side could not see it. **Two numbers differing by less than the spread of either are not a
result**, which applies to the rest of this table too: treat a difference under about 10% on a
single module as unproven.

**`spirv-dis` cannot be trusted on these modules.** It aborts at word 4 with `Invalid capability
operand: 5388`, so any disassembly-based check reads a truncated module and confirms whatever
absence it was testing for. Walking the instruction stream is what settled it. `OpReorderThreadWithHintNV`
is **5280**; 5279 is the HitObject form.

**8.2, 8.3, 8.4, 8.5 and 8.6 no longer exist as work.** They were all about the
shape of the per-mesh loop — batching its builds, hoisting its binds, scheduling
its refits. There is no per-mesh loop: one dispatch builds the whole scene mesh
and one BLAS is rebuilt from it. 8.1 is also stale, since the 12 ms of command
submission it asks about was that loop.

**Occupancy is the lever that keeps paying.** Nsight, frame-level, headless via
`ngfx --activity "GPU Trace Profiler" --auto-export`: compute warp occupancy
17.44% → 22.68% for the register change alone, with warps launched and average
warp latency both flat to within 0.2%. Nothing got faster; more warps fit. The
same lever is why the RT pipeline helped and why SER helped on interiors.

**Workgroup size is not a lever — measured, null.** 8×8 (64 threads) beats every
256-thread shape by 20–40% on all three modules, and 16×16, 32×8 and 8×32 are
within noise of each other, so it is thread count and not aspect ratio. That is
consistent with register-limited scheduling: a 256-thread block must reserve
eight warps of registers before it can be scheduled and cannot retire until its
slowest ray finishes.

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 8.10 | SER, once a Slang release emits the instruction again | The raygen pipeline is already the right shape and the device reports real reordering. Blocked on the toolchain, not the design — see above | P2 | M |
| 8.11 | Sky cubemap keeps only 0.73 of the geometry sky's horizontal detail | 1024/face; 2048 only reaches 0.77 for 4× the memory, so the residual is resampling and filtering, not resolution. The sky is visibly softer than it was | P2 | M |
| 8.12 | The merge dispatch evaluates a binary search per vertex and per triangle | `findVertexObject` / `findTriangleObject` in `skin.slang`. A precomputed per-vertex object id would remove both | P3 | S |
| 8.13 | `HitGeometry` and `SurfaceShading` are the remaining large live state | The material record is out of the traversal registers; these two are what is left across the bounce loop. Occupancy is still only 22.7% | P2 | M |
| 8.6 | Implement the refit schedule the plan already specifies | Deforming BLAS are rebuilt every frame where the plan calls for refit per frame plus occasional staggered rebuild, and for dangly a motion threshold using displacement already computed in `_dangly.vertices`. Design is done; this is execution | P1 | M |
| 8.7 | Explain 7.2% occupancy on the primary-ray dispatch | Even the 5.70 ms trace leaves most of the machine idle. Register pressure, bounce-loop divergence, or the ray-cone LOD path. Needs Nsight's shader profiler, not a guess | P2 | M |
| 8.8 | Frame floor outside the renderer | The update slot is a flat ~2.03-2.07 ms across every backend and pipeline, and raster adds ~1.3 ms of queued CPU work. Neither moves with renderer changes | P2 | M |
| 8.9 | One BLAS for the entire scene, with a per-triangle material id | See below. Supersedes most of 8.2/8.5/8.6 if it works | P1 | M |

### 8.9 — the whole scene is 86k triangles

Measured on `danm14ab`, logged in the TLAS line: **1048 instances, 86k triangles, 61 skinned,
653 dangly**, TLAS build 56-60 us. That is roughly one modern character mesh, in total, for an
entire module. The two-level structure is buying nothing at this scale — the hardware traverses
a top level of a thousand boxes to reach less geometry than a single BLAS holds comfortably.

The registration plan already argued for merging the *static* set, calling it "asking the
hardware to traverse a top-level structure of hundreds of boxes to reach what is really one
rigid scene", but it was written without a triangle count and hedged accordingly. 86k removes
the hedge, and suggests going further than static: at this size, merging *everything* and
rebuilding per frame is plausible, which would delete the per-mesh skin barrier, the per-mesh
BLAS build and the refit schedule in one move.

The blocker the plan names is identity — merged, the instance custom index no longer says which
object was hit. **That is cheap here because this is a ray-query pipeline: one compute shader,
no shader binding table, no hit groups.** So it needs no dispatch-side machinery at all, just a
per-triangle material index buffer looked up with `CommittedPrimitiveIndex()`. At 86k triangles
that table is ~344 KB.

**The shape this wants: one persistent world-space buffer, partitioned static / dynamic, and two
BLAS over it.**

- One vertex buffer for the whole scene in world space, split into a static region and a dynamic
  region. The static region is written **once at module load** and never touched again — no
  per-frame upload, no per-frame copy, no re-registration.
- The dynamic region is written each frame by the skinning and dangly compute passes writing
  **directly into their slice**, rather than into per-mesh buffers that are then read by separate
  BLAS builds. That removes the copy and the per-mesh barrier in one move.
- **One BLAS or two is the open question, and it is a build-cost / trace-quality trade.** Two —
  static built once at load, dynamic rebuilt per frame — makes the static half free. But it is
  worse at trace time, and probably materially so here:
  - The builder never sees the whole scene, so it cannot make a globally optimal split. The
    registration plan says exactly this about merging: it "gives the builder the whole static set
    at once, which is where it can do its best work".
  - The two structures' bounds **overlap heavily**, because foliage is spatially interleaved with
    the terrain it stands on rather than occupying a separate region. A ray crossing the overlap
    traverses both trees.
  - Every top-level instance costs a ray transform into object space. Merged world-space
    geometry costs none.

  One BLAS rebuilt in full every frame keeps traversal optimal and deletes the refit-quality
  problem outright — a rebuild never drifts from the pose it was built for.

### Build cost: published figures say rebuild-everything is affordable

Tellusim's acceleration-structure benchmarks, linearly scaled to our 86k triangles:

| GPU / API | Their 4.21M build | Scaled to 86k | Their refit | Scaled to 86k |
|---|---|---|---|---|
| 2080 Ti (D3D12) | 16.9 ms | **0.35 ms** | 3.7 ms | 0.076 ms |
| 6700 XT (D3D12) | 30.2 ms | 0.62 ms | 4.6 ms | 0.094 ms |
| 6700 XT (Vulkan) | 223 ms | **4.6 ms** | 4.6 ms | 0.094 ms |
| Apple M1 (Metal) | 395 ms | 8.1 ms | 29.8 ms | 0.61 ms |

**A full `PREFER_FAST_TRACE` rebuild of the entire scene costs about a third of a millisecond on
a 2080 Ti**, several generations behind the development 5090. Against a 5 ms frame target that is
noise, and it buys optimal traversal, no refit drift, no static/dynamic split, no motion
threshold and a top level of one instance. **Rebuild everything, every frame, one structure** —
unless the two caveats below bite.

### Memory cost: also a non-issue, which removes the other objection

zeux.io, *Measuring acceleration structures* (2025-03-31), measures BLAS **memory** on Bistro
(1.754M triangles, fp16 positions, `PREFER_FAST_TRACE`, compacted, Vulkan 1.4) across a wide
spread of hardware:

| | bytes / triangle | our 86k scene |
|---|---|---|
| NVIDIA RTX 3050 / 4090 | 25.7 - 26.5 | **~2.2 MB** |
| AMD RDNA4 | 47.9 | ~4.1 MB |
| AMD RDNA3 | 57.0 | ~4.9 MB |

The registration plan's stated cost for merging is memory — "vertices must be pre-transformed to
world space, so nothing is shared and the merged buffer is as large as the static set… memory and
a build, against traversal". At this scale that trade does not exist: the structure is single-digit
megabytes even on the worst hardware in the table, and the un-shared world-space vertex buffer is
of the same order. Merging costs essentially nothing in memory here.

Two further notes from that source: **compaction is worth doing** (those figures are post-compaction,
via `VK_KHR_acceleration_structure`, and `vulkan-rt-backend.md` already lists it as "worth it, and
cheap once the build path works"); and **AMD spends roughly twice NVIDIA's memory per triangle**,
which is a hardware format difference rather than anything actionable, but is worth knowing before
sizing pools on an AMD target.

**Do not conflate the two sources.** zeux measures memory only — it covers neither build time,
granularity overhead, refit, nor Vulkan-versus-D3D12 — so the build figures above remain Tellusim's,
at a different scene and scale, and both are extrapolations rather than measurements of this
renderer. Item 2.5 (GPU timing) is still what turns any of this into fact.

Two things in the Tellusim data matter as much as its headline:

- **Many small structures are worse, and measurably.** 2401 BLAS of 1.5K triangles refit in
  7.0 ms, while 81 BLAS of 52K — *more* total geometry — refit in 3.7 ms. Per-structure overhead
  dominates at fine granularity, and at 714 structures we are squarely in the bad regime. This is
  independent evidence for merging beyond the traversal argument.
- **AMD's Vulkan build path is pathological in this data: 223 ms against 30.2 ms for the same
  work on the same GPU under D3D12, a 7.4x gap.** Scaled to our scene that is 4.6 ms, which would
  consume the entire frame budget. We are Vulkan-only, so this is a real risk and not a footnote.
  Verify it on current drivers before committing to per-frame full rebuilds; if it holds, AMD
  needs the static/dynamic split even where NVIDIA does not, and the design must keep that option
  open rather than assuming one structure everywhere.
- The per-triangle material table partitions the same way — static half uploaded once, dynamic
  half only where it actually changes.
- The material records themselves get the same treatment. Today all 1048 are pushed and uploaded
  every frame; the static ones are identical every frame and belong in a write-once region.

**Measured split: of 86k triangles, 41k are dynamic** (skinned, dangly or saber). Nearly half,
not a tail — the guess that instance-heavy foliage would be triangle-light was wrong, so the
per-frame dynamic rebuild is a real 41k-triangle build rather than a rounding error. Either way it is overwhelmingly better than what happens now: **714 separate acceleration
structures rebuilt or refitted per frame** become one build - of 41k triangles if the static half
is split off, or 86k if the whole scene is rebuilt for the better tree.

**The same buffer serves the raster path.** A merged world-space vertex buffer with a
per-triangle material id is exactly what a raster mega-draw needs — the static region drawn with
multi-draw-indirect or a handful of calls instead of hundreds, indexing the same material table
through `gl_DrawID` / `SV_PrimitiveID` rather than `CommittedPrimitiveIndex()`. So **9.3 and 8.9
are one work item, not two.** That doubles the payoff and is the strongest argument for doing it:
neither path has to keep its own copy of the scene, and the world-space pre-transform that
merging forces is a cost paid once for both.

What still has to be answered before building it:

- **Is 86k typical?** One module is not a sample. Ahto City and the Taris streets may be far
  heavier, and dangly-dense exteriors are the worst case. Log it across a warp loop first.
- **Rebuild cost per frame for the whole set**, against the current 653 refits plus 61 skinned
  builds. A full rebuild of 86k triangles is likely cheaper than what happens now, but that is
  a prediction, not a measurement.
- Note the counter sums per *instance*, so shared BLAS geometry is counted repeatedly. That is
  the right number for deciding on a merge, but distinct triangle count is lower.
- Skinned and dangly vertices must land in the merged buffer in world space, which means the
  skinning compute writes into it directly rather than into per-mesh buffers.

Related and already listed: **2.5** (GPU timing in `IStatistic`) is the blocker for all of this —
every number above came from an external tool, and the registration plan has been owing
timestamps around TLAS build and trace dispatch since 2026-07-28. **4.8** is the same class of
CPU-side cost, and the registration plan names flattening the registry's `textures`
`unordered_map` into bindless slots as "the single biggest per-frame cost in the registry today".

## 9. Raster and registry — fewer, larger operations

The traced frame is not the only thing paying per-object costs; the raster path and the registry
feeding both do too. Two figures from `renderer-registration-plan.md` set the scale: `c_drdwar`
registers **30 entries** — `belly`, `chest`, `neck`, `head`, `l_upperarm` and so on — which is 30
draw calls for one droid, and `c_khounda` registers 23 entries eight times over. Meanwhile the
registry is cleared and refilled from scratch every frame, which that plan calls *"correct by
construction… a stopgap, not the end state"*.

The unifying move is the same one the traced path already has designed for BLAS: merge the static
set once and stop touching it per frame. §"Merging static geometry" in the registration plan works
through the costs — world-space pre-transform so nothing is shared, a primitive-to-material table
because the instance custom index no longer identifies one object, and a rebuild whenever the
static set actually changes. Everything there applies to a raster mega-draw too, which is why
these should be planned together rather than solved twice.

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 9.1 | Verify `Material::staticObject` against reality | Prerequisite for everything below and for the merged BLAS. The plan flags it as advisory-turned-load-bearing, and warns it is "a correctness bug if the set is less static than the flag claims". Also needs static-and-never-moves separated from static-and-currently-still | P1 | S |
| 9.2 | Skip static objects in the per-frame registry rebuild | The registry clears and refills everything each frame; the static set by definition did not change. An incremental registry with a static partition removes most of the work and is the precondition for 9.3 and 9.4 | P1 | M |
| 9.3 | One merged static vertex/index buffer, drawn as few calls | Same world-space merge the traced path wants. Once static geometry lives in one buffer it can be drawn with multi-draw-indirect or a handful of calls instead of hundreds, and the traced side gets its merged BLAS from the same data | P1 | L |
| 9.4 | Collapse per-body-part registration | 30 entries for one droid is 30 draw calls and 30 TLAS instances. The plan splits this: genuinely skinned characters merge for free, rigid assemblies trade instance count against the BLAS sharing they currently enjoy — "the trade is real and needs measuring" | P2 | M |
| 9.5 | Per-pass instance re-copy in `drawScene` | `visible` then `batch` copies the instance set twice per pass; scales badly with the grass plan | P2 | S |

Note the measurement problem, which is the same one as §8: **4.8** records the per-frame registry
rebuild at 0.445 ms (~9% of frame) but the plan says to "revisit only on measurement", and the
raster profile in §"Frame time" shows PBR and Retro within 2-4% of each other despite very
different GPU work — because the graphics slot measures CPU time recording the frame, not the GPU
executing it. Nothing here can be scored honestly until **2.5** lands GPU timing.

---

## Already done, docs not yet updated

The planning docs still list these as future work. They landed on the `path-tracing` branch and
`doc/vulkan-rt-backend.md` should be corrected (see 7.2) rather than left to mislead:

- NRD REBLUR integration, all five staged steps — optional CMake component, demodulated
  diffuse/specular split with guides, dispatch wrapper, composite pass.
- Tonemap moved out of the tracer mega-kernel into a composite pass, and then again into
  `pt_tonemap.slang` behind the upscaler.
- `--taajitter` deliberately on for path tracing.
- Motion vectors verified and consumed — world-space for NRD, UV-space screen motion for FSR,
  written from unjittered matrices at both ends.
- FSR 2.2.1 upscaler at NativeAA, replacing the hand-rolled TAA, which is deleted rather than
  left switchable. FidelityFX is MIT, so the licence question in §13.1 is settled.
- Bindless textures and a GPU material buffer indexed by TLAS instance.
- Ray-tracing device groundwork, rigid BLAS, per-frame TLAS, ray-query tracing.
- The `toolkit.exe` link failure.
