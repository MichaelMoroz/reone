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
| 0.2 | Test the editor's Save buttons end to end | Config *keys* round-trip, but nothing has clicked Save and diffed `reone.cfg`. `saveGraphicsOptions` (`editor.cpp:65`) is a read-modify-write that preserves foreign lines **by position** (`:132-133`), and the game path and audio volumes are not in its key list at all — so the untested surface is the graphics keys themselves plus the 9×9 category overrides, where a dropped or misordered line is silent | P0 | S |
| 0.3 | Record why growing `InstanceMaterial` caused `VK_ERROR_DEVICE_LOST` | Fixed by codex, root cause unread. Not device addresses — `a24b1bd3` removed those from the shaders and the struct now carries bindless `uint` texture ids; the live hazard is **storage-buffer array stride**, named in the source at `slang/tracing/resources.slang:64-65`. Next person to add a field needs this written down | P0 | S |
| 0.4 | Rebuild and smoke-test the Debug configuration | Last built before the demodulation and FSR work. Debug links the checked VMA that catches allocations outliving the device — the one thing that would catch FSR's image lifetimes | P0 | S |
| 0.5 | Verify specular demodulation on chromatic `Rf0` | Untestable until now: every stock material is dielectric, so `specFactor` has zero chroma. The new metalness scale (`metallicScale`, `graphics/options.h:170`) forces it | P1 | S |
| 0.6 | Measure a clean build before/after the build-speed work | Only an incremental number (5.1 s) exists, so the actual saving is unknown | P2 | S |
| 0.7 | ~~Verify the additive shadow-occlusion fix~~ *(superseded, `9b98c37c`)* | The gate this described no longer exists — `VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR` appears nowhere in the tree. `9b98c37c` shipped the fix by a different mechanism: per-instance opacity became two BLAS geometries, opaque and non-opaque (`graphics/vulkan/rayquery.cpp:917-920`), and the surviving predicate is `scene/render/pipeline/rayquery.cpp:215-216` keyed on `surfaceType == 1` and bit **24** (sky), not 25. The dead-bit reasoning is dead too: bit 25 is now live as `kPtMaskBlendedCoverage` (`slang/tracing/resources.slang:27`). The sweep this asked for — feature bits nothing sets, guarding behaviour that looks correct in source — was run and came back clean | — | — |

## 1. Path tracing — correctness

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 1.1 | Residual diffuse demodulation leak: `corr(high-freq)` sits at 0.145, not 0 | Down from 0.271 and visually clean, but non-zero means material still reaches the channel. NRD's floor remap accounts for part; the rest is unexplained | P1 | M |
| 1.2 | Specular `F` vs `Fenv` mismatch | Specular is energy-correct but its per-light Schlick term is not the preintegrated environment term it is demodulated by. Same class of bug diffuse had; deliberately not fixed | P1 | M |
| 1.3 | Diffuse channel outliers: max 25.3 while p99.99 is 1.30 | `1/(1-p)` is unbounded as specular probability approaches 1 at grazing angles. Bounded, unlike the 0/0 it replaced, but a firefly candidate under motion | P2 | S |
| 1.4 | FSR reactive and transparency-and-composition masks are null | AMD: without them FSR "handles these cases as best it can". Additive saber blades and particles are precisely those cases | P1 | M |
| 1.5 | Next-event estimation against the full scene light list | KotOR interiors are many small point lights; pure PT is unusably noisy there | P1 | L |
| 1.6 | ~~Grass placement determinism~~ *(done with grass admission)* | Placement and variant now hash `(faceIndex, clusterIndex)` instead of drawing from the shared generator, so they no longer depend on how many times anything else consumed it first. Forced by admission: once grass is in the BLAS, an RNG-order change is a traced-image change | — | — |
| 1.7 | ~~Camera-facing particles: AS proxy vs rasterised composite~~ *(settled and shipped, `2fd6e713`)* | Particles are admitted as camera-facing quads in the merged geometry; Phase D closed with `225383a2`. The "gates emitter admission to the TLAS" clause was false even when written — admission never waited on this decision | — | — |
| 1.8 | Lightmaps treated as albedo where physically wrong | Revisit once real GI exists; `ptLightmapIntensity` is already graded to 0 | P3 | M |
| 1.10 | The light selection table is evaluated twice per path vertex over all 32 slots | `ptDirectLight` runs `directLightWeight` once to total and again to select. Now that the cutoff no longer short-circuits distant lights, both passes run in full. Caching 32 weights would halve it | P2 | S |
| 1.11 | Sphere lights are sampled over the whole subtended cone, not the visible cap | Directions on the far hemisphere of the emitter are sampled and shadowed as if they lit the surface. Correct for a small emitter, increasingly wrong as the ratio dial grows | P3 | M |
| 1.12 | Blended particles still write three of the four denoiser guides | Reduced to a residual, and superseded in scope: the hybrid decision names this entry explicitly (`vulkan-rt-backend.md` §11.2), and the demodulation-factor half is already fixed — `befa8791` guards that write (`slang/rayquery.slang:202`). What remains is viewZ, normal-roughness and motion, still written unconditionally from the primary hit (`rayquery.slang:171-176`, `:183-195`), plus `setUpscalerGuides` (`:213-215`) which still runs when the primary is a blended quad. A thin camera-facing quad has neither the depth nor the motion of the surface being shaded behind it, so NRD fetches history from the wrong place and smears — visible around the Dantooine vents, and it compounds 1.13 because a NaN in a guide propagates across the frame instead of staying in its pixel. Guides should come from the first non-transmissive surface along the visibility ray. **Treat any fix here as a stopgap**: once raster owns primary visibility the guides come from the raster G-buffer and the question stops existing | P2 | S |
| 1.13 | Smoke particles produce non-finite pixels, and the denoiser spreads them | NaN on main-menu smoke, propagating across the frame once NRD reprojects it. **The suspect this entry named is gone**: `befa8791` rewrote the blended-coverage branch (`slang/rayquery.slang:300-323`) to do no division at all, and the two surviving `/ outputs.specFactor` sites (`:269`, `:271`) are non-blended and floored by `kDemodulationFloor = 0.02` (`slang/tracing/brdf.slang:14`). Also ruled out: the premultiplied-alpha flag is never set for these textures (`fx_smoke01`/`fx_smoke`, DXT5, `Blending::None`), a plain alpha sign flip makes it worse, and `mainTex` resolves to a valid bindless id. **The current lead is the transmission caps disagreeing** — pass-through 16, blended transmission 12, shadow rays uncapped (`cleanup-plan.md:652-674`), so a plume is lit by rays that see a twelfth of it and shadowed by rays that see all of it; the old "12 vs 96 pixel-identical" note tested the wrong axis and should not be read as clearing the caps. Measure with `numpy.isfinite` on a `--dumptargets` `.npy`, not by eye — NaN renders as black, white or garbage depending on the path it takes | **P0** | M |

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
| 1.14 | **A room is only a skybox if it is a cube-like shell, and there is exactly one per level** | Decided 2026-08-02. Sky-room detection currently accepts whatever the module nominates, and the survey behind `00c542e2` shows what that lets in: `m43aa_14a` (Unknown World, `unk_m43aa`) came back as a sky room with **14 meshes, 4067 faces and 7 textures**. That is a modelled backdrop, not a sky. **The classifier should be geometric**: accept a room only if it is a cube-like enclosing shell around the camera - roughly cubic bounds, a small face count, inward-facing. Reject everything else rather than baking it. And **enforce exactly one sky per level**; more than one is a classification failure, not a scene with two skies. **This reopens the option `00c542e2` had to rule out.** The baker was kept because not all nominated sky rooms were six-sided boxes - but once only cube-like shells qualify, every accepted sky *is* a box, so the cubemap can be assembled directly from its six face textures with the box orientation as a sky rotation parameter, and the bake pass deletes outright. `slang/sky.slang` and its entry point then go with it. Two things to settle while doing it: what happens in a module whose nominated sky is rejected - most likely no sky, sampling the fallback cube, which needs checking against how those modules look today; and the **15 of 119 modules the survey never reached** (`stunt_00` and others failed on a `****` string), which have not been classified either way. Survey data: `scratchpad/sky_survey.json` while it lasts | P2 | M |

## 2. Path tracing — quality levers not yet pulled

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 2.1 | Feed NRD `IN_DIFF_CONFIDENCE` / `IN_SPEC_CONFIDENCE` | NRD: "essential to preserve responsiveness… should not rely solely on the anti-lag". Also the documented way to make a long history usable — we run 6 frames because 63 only buys 6% | P1 | M |
| 2.2 | Feed NRD `IN_DISOCCLUSION_THRESHOLD_MIX` | Per-pixel disocclusion threshold. Aimed squarely at foliage edges, where NRD dumps history every frame, without loosening rejection globally | P2 | M |
| 2.3 | Evaluate NRD's SH denoiser path + `NRD_SG_ReJitter` | The only route to restoring the jitter NRD's temporal passes suppress — which is jitter FSR wants. Costs 2 extra RGBA16f targets per channel plus a resolve | P3 | L |
| 2.4 | Channel debug views need their own exposure | Specular divided by `Fenv` (~0.04) is ~20x over range and clips to white, so the view cannot be read at all | P2 | S |
| 2.5 | GPU timing in `IStatistic` | Blocks any credible performance claim; all current numbers are CPU-side frame time | P1 | M |
| 2.6 | **Geometric grass instead of alpha-tested quads** | Grass is punch-through quads, so in a traced frame every ray-triangle hit runs a candidate shader - interpolate UV, sample, compare, decide - which drops out of fixed-function traversal on exactly the geometry rays cross most. Measured on danm14ab at a grassy camera: 3x density cost +25% of the graphics slot and scaled with samples (16 spp went 32.6 -> 46.4 ms) — but **those numbers were taken against the pre-fix clamped cluster pool and are an underestimate**. `c343f273` made the pool grow with the dial, so density now moves the count roughly linearly (0.5 -> 1457, 1.0 -> 2903, 2.0 -> 5806, 4.0 -> 11615 clusters) where before it saturated. Re-measure before quoting a ceiling. Opaque blade geometry stays in hardware, lets the BLAS build with `PREFER_FAST_TRACE` and compaction, and lets traversal commit and stop. Against it: 4-8x the triangles, a bigger BLAS, and a slower BLAS build - and grass clusters materialise as the camera moves, so that build cost is per frame rather than amortised. **Measure the ceiling before building anything.** Mark grass opaque and re-run: the image is wrong, but the frame time is the upper bound on what geometric grass can win, since it removes exactly the any-hit cost and adds none of the triangles. That test needs no new machinery — per-geometry opaque/non-opaque partitioning already exists (`graphics/vulkan/rayquery.cpp:917-920`) and the whole experiment is scriptable now that `grassdensity` sets the dial and the `grass` toggle works. **And do it after hybrid** (`vulkan-rt-backend.md` §11.2), which changes the answer: once raster owns primary visibility, grass is traced only for shadows and secondary bounces, so the primary rays - the ones crossing the most grass at the most pixels - stop existing and a large part of the cost goes away for free. A cheaper middle option exists either way: trim the quads to the opaque region of the texture, which cuts wasted candidate invocations with no new art | P2 | L |

## 3. Path tracing — not yet built

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 3.1 | ~~Deformation compute pass + per-node BLAS~~ *(done, and the second half was abandoned on purpose)* | The pass is `slang/skin.slang` — dangly `:217-221`, saber `:223-228`, skinning `:231-257`. Per-node BLAS did not land and is not wanted: `9b98c37c` replaced 714 structures with one, and a grep for `refit` or `MODE_UPDATE` across `src/` and `include/` returns nothing, so no refit path exists to hang them on. Residual re-filed as 3.5 | — | — |
| 3.2 | Material PBR-ification: authored name→material table, then heuristic BSDF | Order is load-bearing — the authored map must precede the heuristic | P1 | L |
| 3.3 | Deforming shadow casters | `shadow.slang` has only a rigid vertex stage; skinned meshes never cast shadows | P2 | M |
| 3.4 | ~~Shadow passes cull against the view camera, not the light~~ *(fixed 2026-07-28, `49496d2f`)* | Fixed two days before this backlog was compiled, so it was never true of the tree it describes: `libs/scene/graph.cpp:448-455` builds a frustum per cascade and per cube face from `_shadowLightSpace[i]` and hands them over as `VisibilityPolicy::shadowFrusta`. "Affects all three pipelines" was wrong regardless — there is one pipeline implementation, and `RenderMode` has three values. Residual re-filed as 3.6 | — | — |
| 3.5 | Evaluate the dangly spring inside the merge kernel | Re-filed from `c1469287` and from 3.1. The spring is still solved on the CPU and its output uploaded every frame for 653 dangly nodes; moving it into the merge dispatch, which already writes the dynamic slice of the merged buffer, deletes the pass and the per-frame upload together | P2 | M |
| 3.6 | Draw-distance culling still uses the view camera in shadow passes | The frustum test is now per light (3.4), but distance culling is not: `gpuscene.cpp:66-71` measures against `visibility.drawDistanceCamera`, which stays the view camera in a shadow pass. A caster far from the eye and close to the light is dropped after the frustum correctly kept it | P2 | S |

## 4. Build and tooling

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 4.1 | ~~`toolkit.exe` link failure~~ | **Done 2026-07-30.** Had failed since `b991a197`; trained everyone to ignore a red build, which is how `tests` stayed broken across four commits | — | — |
| 4.2 | Decide `ENABLE_FSR` default / Linux support | `ENABLE_FSR ... OFF` (`CMakeLists.txt:43`) is real, but "no anti-aliasing at all" is not: `fxaa {true}` by default (`graphics/options.h:175`) and FXAA runs at `scenepipeline.cpp:1300`/`:1305` in both the PBR and the Retro step lists. The gap is **path tracing only**, which skips the step list entirely (`vulkan.cpp:150`) and so has FSR or nothing — every Linux and default build path-traces un-antialiased, and that is a mode where the aliasing rides on top of MC noise | P1 | M |
| 4.3 | Delete the vendored FSR 3.1.4 SDK from `build/_deps/ffx-src` | **436 MB** of dead weight from the abandoned FSR3 attempt | P3 | S |
| 4.4 | Ninja generator evaluation | MSBuild is the bulk of build time. Blocked on FSR2's external build hard-failing on non-VS generators | P2 | M |
| 4.5 | Runtime resolution change is unfinished | Cameras never re-apply projection; `GUI::_rootOffset` is computed only in `load`. Treat as restart-required until a virtual re-apply exists | P2 | M |
| 4.6 | Retro publishes only its final Output as a render target | "Either backend" is dead (`df1aa375`), and retro is not empty: `scenepipeline.cpp:1537-1541` publishes `{"Output", "output", Color, _frameImage}`. But that is one channel against PBR's five G-buffer targets plus depth and SSAO (`:1542-1557`), so `--dumptargets` can compare the two pipelines' final images and nothing in between, and every intermediate parity number stays PBR-heavy by construction | P2 | M |
| 4.7 | ~~Render-target viewer returns an empty list on Vulkan~~ *(wrong)* | `targets()` returns a populated list: `scene/render/pipeline/vulkan.cpp:173-186` forwards to `graphics/vulkan/scenepipeline.cpp:1872-1877`, whose entries are built at `:1512-1557`, and it is empty only before `_inited`. The real defect was a null `_gbuffer` dereference, since fixed and recorded in place at `scenepipeline.cpp:1518-1522`. The resize half is re-filed, unverified, as 4.10 | — | — |
| 4.8 | Per-frame clear-and-refill of the scene snapshot | `RenderRegistry` is gone, but the pattern survives verbatim under `GpuScene`: `libs/scene/graph.cpp:438` calls `_gpuScene.resetFrame()` (`gpuscene.cpp:127-134`), then `:516` calls `collectInto(_gpuScene)` (`graph.cpp:580-608`). The old 0.445 ms (~9% of frame), and the 1.873 ms of draw walks with it, were **moved rather than removed** (`8d37449d`), so neither figure describes the current tree and both need re-measuring before the cost is scored. The static partition that would remove it is Phase F5 | P3 | M |
| 4.9 | **Compile shaders in the engine with Slang, and drop the build step** | Today `slangc` transpiles to SPIR-V at build time — `src/apps/shaderpack/CMakeLists.txt:67-79`, not `CMakeLists.txt:204`, which is only the `find_program` that locates the compiler; the `dxc` in `_deps` belongs to FSR2 and has nothing to do with this path — and the engine loads the `.spv` from disk (`graphics/vulkan/renderer.cpp:71`). Link Slang instead and compile at startup and on demand: no shader step in the build, and editing a `.slang` shows up next frame. Nice to have rather than needed - the appeal is the iteration loop, not correctness. It also closes the stale-module trap outright rather than mitigating it, but that is a smaller win than it sounds: the trap is already mitigated and documented at `shaderpack/CMakeLists.txt:48-53`. Wants: errors to the console instead of a crash, keep the last good module for a pipeline so a typo does not take the frame down, and a force-recompile. Startup cost is the thing to watch, since it moves from build time to every launch | P3 | M |
| 4.10 | **Unverified:** stale descriptor sets in the render-target viewer across a resize | Carried over from 4.7 as a question rather than a bug. The viewer hands out handles from a list built at init (`scenepipeline.cpp:1512-1557`); nothing has been shown to re-publish them when the pipeline reallocates its targets, and a use-after-free there is exactly what a single-frame capture cannot catch. Nobody has reproduced it, and 4.5 may make it unreachable in practice — resolution change is restart-required today | P3 | S |

## 5. Vulkan parity gaps

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 5.1 | ~~Retro pipeline has no Vulkan counterpart~~ **wrong, closed** | It always had one: `VulkanRenderPipeline` branches on `options.pbr`. Retro and PBR captures of the same module differ by 77% of pixels and 26 levels of mean luminance, and the retro image is correct down to skinned characters and weapons. The factory used to warn "No retro pipeline on Vulkan" and then build the same pipeline anyway; that warning is deleted (`6a681168`) | — | — |
| 5.2 | ~~Movie playback, profiler and console unported to `I2DRenderer`~~ *(all four sub-claims done)* | Console `apps/engine/console.cpp:22,236,244,280`; movie `libs/movie/movie.cpp:23,101`; the profiler is not a renderer problem any more because it no longer renders — the ImGui editor draws it (`editor.cpp:1042`). And the 3D sub-scene behind menu panels never needed nested passes: `Game::renderSceneOffscreen` hoists it out (`libs/game/game.cpp:946-961`) | — | — |
| 5.3 | Delete the two orphaned debug-shader entries | The AABB entry was superseded rather than fixed. `8d37449d` deleted the draws along with `game/debug.{h,cpp}` and the `showaabb`/`showwalkmesh`/`showtriggers` commands, so "never drawn on Vulkan" is now true by construction and getting it back is 7.8, not a port gap. What is left here is cleanup: `slang/common.slang:106,130` are two shaderpack entries with no consumer | P3 | S |
| 5.4 | ~~Resolve alpha and retro post-processing order differ from GL~~ *(unfalsifiable, closed)* | `df1aa375` removed the comparison target, so the claim can no longer be tested or even stated. Nothing is known to be wrong with the Vulkan ordering; if it is ever suspected, it should be re-filed as a claim about Vulkan with its own justification rather than resurrected as a parity gap | — | — |
| 5.5 | ~~Retire the OpenGL backend~~ *(done, `3445fdc1`..`df1aa375`)* | 184 files, 13,420 lines deleted against 346 added. The toolkit was ported by presenting into its wx panel directly rather than the offscreen-plus-readback this entry assumed — SDL3 adopts the panel's native handle, so there is no second render path | — | — |
| 5.6 | ~~Toolkit model preview renders exploded geometry~~ *(done, `a89c6eeb`, `44e11344`)* | Never a rendering bug: the camera sat at a fixed 8 units for every model, so large ones enclosed it and small ones were specks — a 24× range in apparent size. Pre-existing since 44472f0e, unrelated to the Vulkan port. Worth remembering *how* it was found: four hypotheses were proposed and killed from screenshots before anyone made the preview scriptable, after which one capture settled it. The `--open`/`--capture` harness is the durable part | — | — |

## 6. Engine and game systems

Largely untouched by renderer work, and by volume the biggest block of deferred work in the repo.

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 6.1 | 60 of 64 effect `.cpp`, plus 25 header-only effect classes, and 15 of 40 actions are `// TODO: implement` stubs | The single largest body of unimplemented game logic, and larger than the old count admitted: `include/reone/game/effect/` holds 89 headers against 64 `.cpp`, so 25 effect classes have no implementation file at all and do not show up in a stub count | P2 | L |
| 6.2 | `SceneGraph::_nodes` never releases | A module transition leaks the previous module's entire node graph. Blocks id-generation work | P1 | M |
| 6.3 | Combat gaps: no Dueling / Two-Weapon Fighting, single damage effect per weapon, guessed K1 damage bonus, combat history never cleared | Visible rules divergence | P2 | M |
| 6.4 | Serialization gaps: effects/actions not deserialized for placeables and doors, items not deserialized, `LastDisturbed` unimplemented | Save/load correctness | P1 | M |
| 6.5 | Action bar missing Force powers, mines, TSL slots | Visible UI gap | P2 | M |
| 6.6 | Hardcoded 1.7 m line-of-sight / audio listener height, at six sites | Should be appearance-based, and the duplication is why it is not a one-line change: `game/object/area.cpp:78` and `:575`, `game/game.cpp:1483`, `game/object/creature.cpp:613`, `game/script/routine/impl/main.cpp:1347` (a second local constant) and `game/gui/ingame/character.cpp:192` each carry their own copy, so the first work is collapsing them to one accessor | P3 | M |
| 6.7 | ~~`Texture::flush` throws for most pixel formats~~ *(no such method)* | It was `flushGPUToCPU`, and `df1aa375` deleted it with the GL backend. If CPU readback is still wanted it is a claim about the Vulkan path behind `--dumptargets`, which nobody has shown to be broken — re-file it there with evidence rather than carrying this forward | — | — |
| 6.8 | Assorted: low-HP animation variants, NPC selectability, per-frame selection overlay updates, listbox CTD, `s_male02` TSLRCM workaround, AABB NaN handling | Small independent fixes | P3 | M |

## 7. Docs and determinism

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 7.1 | **Raster half solved: it was the FPS readout.** Path tracing is genuinely nondeterministic | Raster capture runs are **bit-exact**. Every differing pixel sat in one 153×13 box in the top-right — the frame-time text at `editor.cpp:1592` ("234.4 FPS 4.27 ms" vs "241.3 FPS 4.14 ms"); `:1583-1588` is the accumulation loop above it, not the draw. Mask raw rows 1040+/cols 1600+ and three runs each of danm14ab, ebo_m12aa and danm13, in both PBR and retro, are **byte-identical**: six groups, six hashes, no exceptions. Suppressing wall-clock readouts under `isCaptureRun` makes that unconditional, and **that is still undone** — `isCaptureRun` exists (`engine.h:125`) but nothing at the draw consults it, so every capture still forges its own diff. Path tracing stays nondeterministic afterwards at 8–64% of pixels, from acceleration-structure build order feeding an order-dependent accumulation loop — that part is real and needs distribution-vs-distribution comparison. This bug is behind several false regression reports during the GL removal; the diagnostics skill's "raster is byte-identical" was right all along and its 0.02% bound for tracing is the part that is wrong | P1 | S |
| 7.2 | ~~`doc/vulkan-rt-backend.md` status block is stale~~ *(done, `8ef7ca32`)* | Now dated 2026-08-01 (`doc/vulkan-rt-backend.md:40`) and describes the pipeline that exists — Vulkan-only, `GpuScene` owning merged world-space geometry, raster in PBR and Retro, path tracing with NRD and FSR | — | — |
| 7.3 | Whole-frame parity baseline for danm14ab frame 900 is invalid | Stale dump plus the removed Slang GL path; no replacement figure exists | P2 | S |
| 7.4 | Open design questions | Emitter granularity (per emitter vs per particle system); compute-skinning ownership. Both sub-questions that had answers are struck: ~~whether walkmeshes and AABB debug geometry should register at all~~ *(no — see 7.8)* and ~~whether grass and particles are ever traced~~ *(both are, `cbba3a17` and `2fd6e713`; Phase D closed with `225383a2`)* | P2 | — |
| 7.5 | **NRD accumulation is nondeterministic, and it was masquerading as tracer noise** | Isolated 2026-07-31 on `danm13` frame 310, `--dev 0`. Mean luminance over repeated runs: denoiser and FSR both off, sd **0.00063**; FSR alone on, sd **0.00051**; **NRD alone on, sd 0.14043** with one run 0.35 clear of the rest; both on, sd 0.01283 and bistable. So NRD is the whole of it, FSR contributes nothing, and FSR partly *masks* NRD's excursions. The tracer itself is deterministic in energy — six decimals — while per-pixel variation of 6-16% remains and is genuine MC and acceleration-structure ordering. Two consequences. **Measurement:** traced comparisons should run `--ptdenoise 0`, which turns a 16-runs-a-side statistical exercise back into a tight number. **Correctness:** REBLUR producing a different result from identical input is a real temporal-stability bug, not a harness artifact. The obvious explanation was something fed to NRD that is not frame-indexed, and **all three named suspects check out negative**: `common.frameIndex = frameNumber` (`graphics/vulkan/nrddenoiser.cpp:357`) is frame-indexed, not wall-clock; the history reset is a deterministic 20-unit camera-distance test (`:328-331`); and nothing timing-derived reaches `SetCommonSettings` at all (`:334-361`). That leaves NRD-internal state or resource reuse across frames, which is the harder half and is why the task stands | **P1** | M |
| 7.6 | ~~Particle admission unobserved~~ *(answered)* | It was working. The TLAS line counts admitted grass clusters, particles and billboards, and the earlier null result was a measurement failure rather than a code failure: 55 saber-spark quads cannot move a whole-frame mean luminance by more than 3e-6. The counts once quoted here — 1482 clusters, 55 particles on danm14ab — have been superseded twice since: `9ed2c573` reports 2956 clusters and **232-237** particles, and `c343f273` made the cluster pool follow the density dial (1.0 → 2903). Kept for the lesson, not the numbers: whole-frame statistics cannot see small localized content, and a count answered in one run what six module probes could not | — | — |
| 7.7 | Isolation fixtures: grass cluster placement, not `scene empty` | Earlier entry claimed `scene empty` was broken because the snapshot logged `entries=0` and captures were black. **Both are correct behaviour** — an empty scene has no admitted objects, the sky is baked at startup rather than being one, and a scene with no lights renders black. The commands work: `warp` + `scene empty` + `grass` admitted the full pool at the fixture's original 80-unit camera. The **2048** figure is superseded — `c343f273` made the pool size follow the density dial, so it starts at `kNumClustersInPool` = 4096 (`node/grass.cpp:40`) and is capped by `kMaxClustersInPool` = 32768 (`:45`). Two real problems remain. Raster culls grass beyond `kMaxClusterDistance` = 32 (`node/grass.cpp:48`; `:42` is now a comment) while the tracer admits the whole pool, so at 80 units raster draws nothing and any comparison is meaningless. And moving the camera to 12 units produced **zero** clusters, which is the part worth diagnosing — cluster placement appears to depend on camera proximity to the surface in a way that fails at close range. Fixtures also need a light, or every capture is black by construction. **The tooling this was waiting on now exists**: `--commands-frame` runs a command list at a chosen frame (`optionsparser.cpp:47`, `engine.cpp:359`, `:482`), so the whole fixture is one headless run | P2 | S |
| 7.8 | **Debug geometry belongs in `GpuScene`, in a region the BLAS does not ask for** | Deleted with `RenderRegistry`, along with `game/debug.{h,cpp}` and the `showaabb`/`showwalkmesh`/`showtriggers` commands, because the flags cannot do anything once the draws are gone. It comes back, and **it comes back inside `GpuScene`** — a separate debug renderer would be a dangling second scene representation, and would have to rebuild world-space merging, transforms and skinning that the core already does. A walkmesh on a moving door then works for free. **Admission does not imply BLAS inclusion.** That was the objection to this and it is wrong: the core publishes regions and the *tracer* supplies its own intersection classification, so debug is simply a region the BLAS build does not consume. Nothing appears in a reflection or casts a shadow, and no consumer needs a filter — the tracer just never asks for that range. **`offMaterial` stops being a blocker.** The plan called it "the one attribute `MergedVertex` structurally cannot express" and used it to argue walkmesh should be deleted rather than merged. But the merged stream already carries a **per-triangle material id**, so walkable versus non-walkable is two materials over one mesh, not a vertex attribute. No widening of `MergedVertex`. **What renders it**: the raster consumer, in its own pass over the debug regions, after the scene resolves. World-space and depth-tested — it renders *in* the scene and is occluded by the scene's objects, so a walkmesh behind a wall is hidden. That needs the scene depth buffer, and **the hybrid decision supplies it**: raster owns primary visibility in every mode (`vulkan-rt-backend.md` §11.2), so a raster depth buffer always exists and the pass is the same pass in traced and rasterised frames. The earlier worry — converting `traced_view_z` back to device depth, or paying for a depth prepass — does not arise. The old shape does not return: `RegisteredDebug` held a `std::function<void()>` per entry, a type-erased callback inside a data snapshot. An immediate-mode `line()`/`box()`/`mesh()` API can still exist, but as a *producer* that admits transient geometry into `GpuScene` for the frame, not as a second renderer. The three flags become admission filters. **Note this is branch divergence from `master`, where the capability exists** | P2 | M |
| 7.9 | **Keep the traced G-buffer as the validation instrument** | The hybrid decision (`vulkan-rt-backend.md` §11.2) retires traced camera rays, and **the one piece of the traced primary path that survives is a debug shader emitting a ray-traced G-buffer**. It is not a leftover — it is the check that the two renderers describe the same scene, and it is the only check for that which does not depend on judging an image. The dump already exists (`2fd6e713`, `a3a2f64d`) and publishes `traced_view_z`, `traced_normal_roughness`, `traced_motion`, `traced_diffuse`, `traced_specular`, `traced_noise_free`, `traced_diff_factor`, `traced_spec_factor`, `traced_device_depth` and `traced_screen_motion` alongside the `g_buffer_*` set (`graphics/vulkan/rayquery.cpp:820-823`) — there is no `traced_albedo`, and any comparison script written against that name silently compares nothing. What is needed is to keep the dump deliberately as the traced primary path is deleted around it, and to state which channels must agree and to what tolerance. Position and depth should agree to the bit once the merge and the vertex stage compute world position identically (Phase F's F0 probe); normals only once the merge adopts raster's inverse transpose. The mode it would eventually live in does not exist yet: `RenderMode` is `Retro`, `PBR`, `PathTracing` (`include/reone/scene/render/pipeline.h:58-63`), so `RTDebug` is a planned mode, not a current one. **Deleting the traced visibility walk without first pinning this down loses the ability to validate the merged scene at all** | **P1** | S |

## 8. Performance — from the Nsight trace of 2026-07-30

Read the trace before picking anything here. Frame 17.75 ms, of which
`rayquery:primaryRay` is **5.70 ms** — so roughly **12 ms, two thirds of the frame, is spent
outside the trace**, in 555 command-buffer events recorded before it. Throughput confirms it:
RTCORE 10.8%, SM 9.8%, compute-shader warp occupancy 7.2% with 30.2% of warps unallocated in
active SMs. The GPU is very nearly idle while the CPU feeds it a long chain of tiny serialised
work.

**That trace predates `9b98c37c`, and the shape it describes is gone.** It was one iteration
per skinned mesh — bind pipeline, bind two descriptor sets, push constants, dispatch
`vertexCount/64` groups, full pipeline barrier, build one BLAS — repeated across hundreds of
meshes. There is no per-mesh loop now: one dispatch builds the whole scene mesh and one BLAS is
rebuilt from it, which is why 8.1–8.6 no longer exist as work (see below). Read the 12 ms as a
measurement of the old renderer, not a live target; nothing has re-traced the new one.

**Read `renderer-registration-plan.md` §"What the per-frame rebuild costs" and its BLAS table
first.** Its strategy is mostly historical for the same reason — refit-over-rebuild with a
per-object schedule, the dangly motion threshold ("six hundred refits a frame for leaves"),
merged static BLAS, leaner dispatch shape — but the merged-BLAS half is what shipped, and the
costing is still the only written analysis of the trade.

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
| 8.7 | Explain 7.2% occupancy on the primary-ray dispatch | Even the 5.70 ms trace leaves most of the machine idle. Register pressure, bounce-loop divergence, or the ray-cone LOD path. Needs Nsight's shader profiler, not a guess | P2 | M |
| 8.8 | Frame floor outside the renderer | The update slot is a flat ~2.03-2.07 ms across every backend and pipeline, and raster adds ~1.3 ms of queued CPU work. Neither moves with renderer changes | P2 | M |
| 8.9 | ~~One BLAS for the entire scene, with a per-triangle material id~~ *(done, `9b98c37c`)* | One BLAS is built at `graphics/vulkan/rayquery.cpp:892`, one TLAS instance at `:987`, over two geometries split opaque/non-opaque at `:917-920`; the per-triangle material ids are written by the merge kernel (`slang/skin.slang:60,376`). The prose below is kept as the design record — it is where the 86k triangle count, the build-cost and memory tables, and the static/dynamic argument live. Two things inside it were **not** done and are re-filed as 8.14 and 8.15 | — | — |
| 8.10 | SER, once a Slang release emits the instruction again | The raygen pipeline is already the right shape and the device reports real reordering. Blocked on the toolchain, not the design — see above | P2 | M |
| 8.11 | Sky cubemap keeps only 0.73 of the geometry sky's horizontal detail | 1024/face; 2048 only reaches 0.77 for 4× the memory, so the residual is resampling and filtering, not resolution. The sky is visibly softer than it was | P2 | M |
| 8.12 | The merge dispatch evaluates a binary search per vertex and per triangle | `findVertexObject` / `findTriangleObject` in `skin.slang`. A precomputed per-vertex object id would remove both | P3 | S |
| 8.13 | `HitGeometry` and `SurfaceShading` are the remaining large live state | The material record is out of the traversal registers; these two are what is left across the bounce loop. Occupancy is still only 22.7% | P2 | M |
| 8.14 | BLAS compaction is not implemented | Re-filed out of 8.9, which argued for it twice and never built it: nothing in `graphics/vulkan` sets `ALLOW_COMPACTION` or queries `CompactedSize`, and every memory figure quoted below is a *compacted* one, so our structure is larger than the table says. `vulkan-rt-backend.md` calls it "worth it, and cheap once the build path works", and the build path works | P2 | S |
| 8.15 | Verify AMD's Vulkan BLAS build path before trusting the per-frame full rebuild | Re-filed out of 8.9, and it is now load-bearing rather than a caveat: the design shipped as one full rebuild every frame, so the 223 ms vs 30.2 ms Vulkan/D3D12 gap in the Tellusim data — 4.6 ms scaled to our 86k triangles — would consume the whole frame budget on that hardware. Measure it on current drivers; if it holds, AMD needs the static/dynamic split that one-BLAS-per-frame deliberately gave up | P2 | M |

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
cheap once the build path works" — it is still not done, and is now **8.14**); and **AMD spends
roughly twice NVIDIA's memory per triangle**,
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
  open rather than assuming one structure everywhere. **The commitment was made anyway** —
  `9b98c37c` shipped the single per-frame rebuild — so this is now **8.15** and overdue.
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
CPU-side cost. The registration plan's own candidate for it — flattening the registry's `textures`
`unordered_map` into bindless slots, "the single biggest per-frame cost in the registry today" —
no longer applies: that structure does not exist, and the cost has to be re-found in `GpuScene`.

## 9. Raster and registry — fewer, larger operations

The traced frame is not the only thing paying per-object costs; the raster path and the snapshot
feeding both do too. **`RenderRegistry` is gone — it appears in no source file — so read every
"registry" below as `GpuScene`**, which inherited the behaviour rather than replacing it. Two
figures from `renderer-registration-plan.md` set the scale: `c_drdwar` publishes **30 entries** —
`belly`, `chest`, `neck`, `head`, `l_upperarm` and so on — which is 30 draw calls for one droid,
and `c_khounda` publishes 23 entries eight times over. Meanwhile the snapshot is cleared and
refilled from scratch every frame — `graph.cpp:438` `resetFrame()`, then `:516` `collectInto()` —
which that plan calls *"correct by construction… a stopgap, not the end state"*, and which is
still exactly what happens (4.8).

The unifying move is the same one the traced path already has designed for BLAS: merge the static
set once and stop touching it per frame. §"Merging static geometry" in the registration plan works
through the costs — world-space pre-transform so nothing is shared, a primitive-to-material table
because the instance custom index no longer identifies one object, and a rebuild whenever the
static set actually changes. Everything there applies to a raster mega-draw too, which is why
these should be planned together rather than solved twice.

| # | Task | Why it matters | Pri | Eff |
|---|---|---|---|---|
| 9.1 | Verify `Material::staticObject` against reality | The plan's framing — an advisory flag turned load-bearing — is out of date: the admission path already declines to trust it, in as many words ("an authored room hint, not the admission proof required to retain geometry", `render/pipeline/rayquery.cpp:221`), and publishes everything as dynamic. So nothing currently depends on it being right. The open question is the one after that: whether it can be trusted enough to found a static merge on, which still needs static-and-never-moves separated from static-and-currently-still | P1 | S |
| 9.2 | Skip static objects in the per-frame snapshot rebuild | `GpuScene` clears and refills everything each frame (`graph.cpp:438`, `:516`); the static set by definition did not change. An incremental snapshot with a static partition removes most of the work and is the precondition for 9.3 and 9.4. Same entry as 4.8 seen from the raster side | P1 | M |
| 9.3 | Bring raster onto the merged buffer the tracer already builds | The traced half is done — `9b98c37c` merges the whole scene into one world-space buffer with per-triangle material ids. Raster still walks per-object draws over its own data, so the merge is paid for and used once. Drawing the static region with multi-draw-indirect or a handful of calls, indexing the same material table through `SV_PrimitiveID`, needs no new geometry work. This is Phase F5 | P1 | L |
| 9.4 | Collapse per-body-part registration | 30 entries for one droid is 30 draw calls — one `MeshSceneNode` to one `executeDraw` (`graph.cpp:585-599`). The "and 30 TLAS instances" half is no longer true: there is one instance for the whole scene (`rayquery.cpp:987`), so the tracer already got this for free and the cost is raster-only. That also kills the trade the plan described — instance count against shared BLAS — leaving a straightforward draw-call reduction | P2 | M |
| 9.5 | Per-pass instance re-copy in `drawScene` | `visible` then `batch` copies the instance set twice per pass; scales badly with the grass plan | P2 | S |

Note the measurement problem, which is the same one as §8: **4.8** records the per-frame snapshot
rebuild at 0.445 ms (~9% of frame), a figure taken before `8d37449d` moved the work, and the
raster profile in §"Frame time" shows PBR and Retro within 2-4% of each other despite very
different GPU work — because the graphics slot measures CPU time recording the frame, not the GPU
executing it. Nothing here can be scored honestly until **2.5** lands GPU timing.

---

## Already done, docs not yet updated

The planning docs still list these as future work. They landed on the `path-tracing` branch; the
`vulkan-rt-backend.md` status block that misdescribed them has since been corrected (7.2), so what
follows is a record rather than a correction owed:

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
