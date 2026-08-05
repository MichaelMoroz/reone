# Master task list

Read [README.md](README.md) for the ID scheme, states and priorities, and
[GLOSSARY.md](GLOSSARY.md) for the vocabulary. Contradictions a person must
settle, and items with no owner, are in [DECISIONS.md](DECISIONS.md).

Compiled 2026-08-05 from nine planning documents, which were then deleted; see
[README.md](README.md) for where each one's content went. Duplicates have been
merged — where three documents described one job, the row names all three and
keeps the most current description. Items the staleness audit found already done
are in **Closed** at the bottom with their evidence, not deleted.

---

## RAS — raster

The G track owns most of this. [DESIGN.md](DESIGN.md) holds each step's shape and
acceptance criteria; these rows are the index into it.

| ID | Title | St | Pri | Eff | Owner | Blocked by | Provenance |
|---|---|---|---|---|---|---|---|
| RAS-001 | **G6c — PBR evaluates the tracer's material derivation, not stand-ins** | open | P1 | M | phase-f G6c | — | DESIGN.md; retro-vs-PBR deltas 6.1%→57.2% |
| RAS-002 | G6c — retire the Odyssey range cull and falloff from PBR only | decided | P1 | S | phase-f G6c | RAS-001 | DESIGN.md; `pbr_resolve.slang:83-85,235`; decided 2026-08-05 |
| RAS-003 | **G8 — the blended pass and the three alpha kinds** | open | P1 | L | phase-f G8 | — | DESIGN.md; backlog 5.4-adjacent |
| RAS-004 | G8 — classification: use the authored `transparencyHint` as the blended signal | open | P1 | M | phase-f G8 | RAS-003 | FIDELITY #21, #2; `mdlmdxreader.cpp:475` parsed and unread |
| RAS-005 | G8 — per-node alpha fade must actually render | open | P1 | M | phase-f G8 | RAS-004 | FIDELITY #18; `skin.slang:301` hard-codes vertex colour white |
| RAS-006 | G8 — the transparency sort remap (two reads: index and material) | open | P1 | M | phase-f G8 | RAS-003 | DESIGN.md; contradicts reference correction 8 → see DECISIONS |
| RAS-007 | G8 — authored emitter `renderOrder` in the ordering decision | open | P2 | S | phase-f G8 | RAS-006 | FIDELITY #11; 1,904 K1 / 1,014 K2 non-zero |
| RAS-008 | G8 — additive without alpha uses `SRC_COLOR/ONE` | open | P2 | S | phase-f G8 | RAS-003 | FIDELITY #4; phase-f reference correction 7 |
| RAS-009 | G8 — `Punch-Through` emitter blend is dropped | open | P2 | S | phase-f G8 | — | FIDELITY #22; `emitter.cpp:325` tests only `Lighten` |
| RAS-010 | **G9 — the shared output stage: bloom, lens flares, anti-aliasing** | open | P1 | L | phase-f G9 | RAS-003 | DESIGN.md; merges FIDELITY #6, #7, backlog 4.2, vulkan-remaining §3 |
| RAS-011 | G9 — decide `ENABLE_FSR` default and Linux AA | open | P1 | M | phase-f G9 | RAS-010 | backlog 4.2; PT skips the step list so it has FSR or nothing |
| RAS-012 | G9 — path tracing shows no image difference with jitter on vs off | open | P1 | S | phase-f G9 | — | DESIGN.md; must be settled before the jitter gate opens |
| RAS-013 | Bump has never been held to a fixture | open | P2 | S | phase-f G6 | — | DESIGN.md; the one part of G6's scope never discharged |
| RAS-014 | Metal at LOD 0 filters nothing at distance; minified metal may alias | open | P3 | S | none | — | DESIGN.md, left open deliberately |
| RAS-015 | Adopt textured roughness/metalness in one step for both consumers | open | P2 | M | none | RAS-001 | DESIGN.md; both currently derive roughness from diffuse alpha |
| RAS-016 | Sky cubemap keeps only 0.73 of the geometry sky's horizontal detail | open | P2 | M | none | — | backlog 8.11; residual is resampling, not resolution |
| RAS-017 | Collapse per-body-part registration (30 draws for one droid) | open | P2 | M | none | — | backlog 9.4, RECORD.md; raster-only now |
| RAS-018 | Per-pass instance re-copy in the draw walk | open | P2 | S | none | — | backlog 9.5 |
| RAS-019 | SSAO — unowned, decide | open | P3 | M | none | — | DESIGN.md; screen-space approximation of what PT computes properly |
| RAS-020 | SSR — unowned, decide | open | P3 | M | none | — | DESIGN.md |
| RAS-021 | Measure phase F on low-end hardware; per-mesh draws may win | open | P3 | M | none | — | RECORD.md; "raster keeps per-mesh draws" is an acceptable answer |

## STR — scene and structural

The S track. [DESIGN.md](DESIGN.md) part 5 holds the step designs and the seven
decisions already taken.

| ID | Title | St | Pri | Eff | Owner | Blocked by | Provenance |
|---|---|---|---|---|---|---|---|
| STR-001 | **S0 — sub-zone `SceneGraph::update` and `Game::update`** | open | P1 | S | S0 | — | DESIGN.md; 1.25 ms is uninstrumented |
| STR-002 | S0 — the scaling fixture (the no-creep tripwire) | open | P1 | M | S0 | — | DESIGN.md |
| STR-003 | S0 — update the diagnostics skill's calibration line | open | P2 | S | S0 | STR-001 | DESIGN.md; stale by 46×/4× |
| STR-004 | **S2 — nodes own GPU slots (this is R2)** | open | P1 | L | S2 | RAS-003 | DESIGN.md; merges phase-f R2, backlog 4.8, 9.2 |
| STR-005 | S2 — fix `SceneGraph::_nodes` never releasing | open | P1 | M | S2 | — | backlog 6.2; a module transition leaks the whole node graph |
| STR-006 | S3 — skin palettes become event-driven | open | P2 | M | S3 | STR-004 | DESIGN.md; 61 meshes × 128 bones unconditionally |
| STR-007 | S3 — dangly moves into the merge kernel | open | P2 | M | S3 | STR-004 | DESIGN.md; absorbs backlog 3.5; deletes 789 calls/frame |
| STR-008 | S3 — particles simulate on the GPU | open | P2 | L | S3 | STR-004 | DESIGN.md; answers backlog 7.4's granularity question |
| STR-009 | S3 — `settleMeshTransform` dies | open | P2 | S | S3 | STR-004 | DESIGN.md |
| STR-010 | S3 — flare LOS becomes a GPU visibility query | open | P2 | M | S3 | RAS-010 | DESIGN.md; coordinate with G9's flare un-filtering |
| STR-011 | S4 — decide what "static" is provable from | open | P1 | S | S4 | — | backlog 9.1; the admission proof, not the authored hint |
| STR-012 | S4 — static/dynamic partition of merged buffer and material table | open | P1 | L | S4 | STR-004, STR-011 | DESIGN.md; danm14ab 45k of 86k static |
| STR-013 | S4 — remove the per-element binary search in the merge | open | P3 | S | S4 | STR-012 | backlog 8.12 |
| STR-014 | S5 stage 1 — a layout-tracking image type | open | P2 | S | S5 | — | DESIGN.md; collapses 5 helpers + ~65 barrier sites |
| STR-015 | S5 stage 1 — a `RenderPassScope` RAII type | open | P2 | S | S5 | — | DESIGN.md; ~182 preamble lines |
| STR-016 | S5 stage 1 — a pipeline builder covering compute and RT | open | P2 | S | S5 | — | DESIGN.md |
| STR-017 | S5 stage 1 — a descriptor-write builder | open | P2 | S | S5 | — | DESIGN.md; ~1,100 lines out for ~350 in |
| STR-018 | S5 stage 2 — formalise the RHI seam | open | P2 | L | S5 | TRC-020, TOOL-007, STR-014..017 | DESIGN.md; grep gate on `Vk`/`vk`/`vma` outside the RHI |
| STR-019 | S6 — delete the legacy per-mesh path | blocked | P3 | M | S6 | TRC-024 | DESIGN.md; last consumer is the runtime sky bake |
| STR-020 | S6 — delete `Registered*` and the frame-phase surface | blocked | P3 | S | S6 | STR-004 | DESIGN.md |
| STR-021 | S6 — delete the per-draw texture-set path | blocked | P3 | S | S6 | STR-018 | DESIGN.md |
| STR-022 | **Room visibility: the VIS graph is gone** | open | P1 | M | none | — | FIDELITY #17; `.vis` parsed and never read; see DECISIONS |
| STR-023 | Debug geometry returns inside `GpuScene`, in a non-BLAS region | open | P2 | M | none | TRC-020 | backlog 7.8; the per-triangle material id makes `offMaterial` a non-blocker |
| STR-024 | The `admitted` vs `drawn` distinction has no mechanism under ranges | open | P3 | S | none | — | RECORD.md |
| STR-025 | Walkmesh geometry deletion is a game-model refactor | open | P3 | M | none | STR-023 | RECORD.md |
| STR-026 | Frame floor outside the renderer (the ~2 ms update slot) | open | P2 | M | none | STR-001 | backlog 8.8 |
| STR-027 | In-phase wind: every dangly tree moves identically forever | open | P3 | M | none | STR-007 | RECORD.md; `0.01*abs(sin(_windTime))` along world X |
| STR-028 | S5 stage 3 — move the five RHI clients out of `vulkan/` | open | P2 | L | S5 | STR-018 | 2026-08-05 RHI sweep; 4,020 lines leave; `vulkan/` 9,398 → 5,378 `.cpp` lines |
| STR-029 | S5 stage 3 — **no `Vulkan` identifier may appear outside `vulkan/`** | open | P1 | M | S5 | STR-028 | 2026-08-05 RHI sweep; 85 occurrences, 13 type names, incl. `IVulkanSceneCallbacks` and `scene/render/pipeline/vulkan.cpp` |
| STR-030 | S5 stage 3 — parent class outside, `Vulkan*` child inside | open | P2 | M | S5 | STR-028, STR-029 | 2026-08-05 RHI sweep, revised same day; supersedes the interface-collapse reading |

## TRC — path tracing

The PT substage runs after the raster track. [DESIGN.md](DESIGN.md) part 3 holds the
substage design; [RECORD.md](RECORD.md) holds the traced-transparency ladder.

| ID | Title | St | Pri | Eff | Owner | Blocked by | Provenance |
|---|---|---|---|---|---|---|---|
| TRC-001 | **Smoke particles produce non-finite pixels; the denoiser spreads them** | open | P0 | M | none | — | backlog 1.13; lead is the three disagreeing transmission caps (16/12/uncapped) |
| TRC-002 | **NRD accumulation is nondeterministic** | open | P1 | M | none | — | backlog 7.5; sd 0.14043 with NRD vs 0.00063 without; all three suspects negative |
| TRC-003 | Run the FSR convergence harness and record the numbers | open | P0 | S | none | — | backlog 0.1; claimed done three times, verified zero |
| TRC-004 | Record why growing `InstanceMaterial` caused `VK_ERROR_DEVICE_LOST` | open | P0 | S | none | — | backlog 0.3; hazard is storage-buffer array stride |
| TRC-005 | Verify specular demodulation on chromatic `Rf0` | open | P1 | S | none | — | backlog 0.5; needs the metalness scale to force the case |
| TRC-006 | Residual diffuse demodulation leak (corr 0.145, was 0.271) | open | P1 | M | none | — | backlog 1.1 |
| TRC-007 | Specular `F` vs `Fenv` mismatch | open | P1 | M | none | — | backlog 1.2; same bug class diffuse had |
| TRC-008 | Diffuse channel outliers (max 25.3 vs p99.99 1.30) | open | P2 | S | none | — | backlog 1.3; firefly candidate under motion |
| TRC-009 | FSR reactive and transparency/composition masks are null | open | P1 | M | none | — | backlog 1.4; additive blades and particles are exactly these cases |
| TRC-010 | Light selection table evaluated twice per path vertex | open | P2 | S | none | — | backlog 1.10; caching 32 weights halves it |
| TRC-011 | Sphere lights sampled over the whole cone, not the visible cap | open | P3 | M | none | — | backlog 1.11 |
| TRC-012 | Lightmaps treated as albedo where physically wrong | open | P3 | M | none | — | backlog 1.8; revisit once real GI exists |
| TRC-013 | Feed NRD `IN_DIFF_CONFIDENCE` / `IN_SPEC_CONFIDENCE` | open | P1 | M | none | — | backlog 2.1 |
| TRC-014 | Feed NRD `IN_DISOCCLUSION_THRESHOLD_MIX` | open | P2 | M | none | — | backlog 2.2; aimed at foliage edges |
| TRC-015 | Evaluate NRD's SH path + `NRD_SG_ReJitter` | open | P3 | L | none | — | backlog 2.3 |
| TRC-016 | Channel debug views need their own exposure | open | P2 | S | none | — | backlog 2.4 |
| TRC-017 | **Material PBR-ification: authored name→material map, then heuristic** | open | P1 | L | none | — | backlog 3.2, RECORD.md; order is load-bearing |
| TRC-018 | An editor mode to maintain the authored material map | open | P2 | M | none | TRC-017 | RECORD.md |
| TRC-019 | **Traced transparency: stochastic commits, flat additive loop, analytic sabers** | decided | P2 | L | PT substage | RAS-003, TRC-020 | backlog 3.7; design settled 2026-08-03, unbuilt |
| TRC-020 | **V1 — hybridise: raster owns primary visibility (V1a/b/c)** | open | P1 | L | phase-f V1 | RAS-010, TRC-021 | DESIGN.md; **not landed** despite rt-backend claiming otherwise |
| TRC-021 | Pin the traced G-buffer as the `RTDebug` validation instrument | open | P1 | S | none | — | backlog 7.9; must land before V1c deletes it; no `traced_albedo` channel exists |
| TRC-022 | The fog grid, the march, and additive as spheres/capsules | open | P2 | L | PT substage | TRC-019 | DESIGN.md-1150; census 714bd700 |
| TRC-023 | Honour the authored emitter `tinted` flag when baking media | open | P2 | S | PT substage | TRC-022 | DESIGN.md; 1,124 emitters carry it |
| TRC-024 | **V0/V2/V4/V5 — the curated offline sky chain** | open | P2 | L | phase-f V | RAS-010 | backlog 1.14, DESIGN.md; merges four steps + FIDELITY #1 |
| TRC-025 | V0 — sky coverage curation (every entry still marked `# review`) | open | P2 | L | phase-f V0 | TRC-024 | DESIGN.md; 56/117 K1, 46/82 K2 |
| TRC-026 | ReSTIR DI | open | P2 | L | PT substage | TRC-019, TRC-020 | DESIGN.md; current loop is one-sample RIS with no reuse |
| TRC-027 | SHARC radiance cache, then resample into a dense volume | open | P3 | L | PT substage | TRC-026 | DESIGN.md-1185 |
| TRC-028 | Volumetrics consuming the grid (prototype transmittance first) | open | P3 | L | PT substage | TRC-022, TRC-027 | DESIGN.md |
| TRC-029 | Next-event estimation over the full light list / light BVH | open | P1 | L | none | TRC-026 | backlog 1.5, RECORD.md |
| TRC-030 | Emissive geometry into the sampled light set | open | P2 | L | none | TRC-029 | RECORD.md |
| TRC-031 | Geometric grass instead of alpha-tested quads | open | P2 | L | none | TRC-020 | backlog 2.6; measure the ceiling first by marking grass opaque |
| TRC-032 | Explain 7.2% occupancy on the primary-ray dispatch | open | P2 | M | none | TOOL-002 | backlog 8.7 |
| TRC-033 | `HitGeometry` and `SurfaceShading` are the remaining live state | open | P2 | M | none | — | backlog 8.13; occupancy still 22.7% |
| TRC-034 | BLAS compaction is not implemented | open | P2 | S | S4 | — | backlog 8.14; every quoted memory figure is post-compaction |
| TRC-035 | **Verify AMD's Vulkan BLAS build path before trusting per-frame rebuild** | open | P2 | M | S4 | — | backlog 8.15; 7.4× gap would consume the frame budget |
| TRC-036 | Is 86k triangles typical? Log counts across a warp loop | open | P3 | S | none | — | backlog 8.16 |
| TRC-037 | Re-trace the current renderer; the old 12 ms predates the merge | open | P2 | M | none | TOOL-002 | backlog §8 preamble |
| TRC-038 | SER, once a Slang release emits the instruction | blocked | P2 | M | none | Slang toolchain | backlog 8.10; measured speedup was noise |
| TRC-039 | Sky leaks into fully enclosed scenes (Taris underground) | open | P2 | M | none | TRC-024 | RECORD.md |
| TRC-040 | Bounce lighting does not exist for analytic lights | open | P2 | M | none | TRC-029 | RECORD.md; NEE runs at the primary hit only |
| TRC-041 | Some objects do not interact with lighting (leaves, hair, doors) | open | P2 | M | none | — | RECORD.md |
| TRC-042 | Calibration programme: tonemapper, intensities, bounces, sky scale | open | P2 | M | none | — | RECORD.md; recorded 2026-07-29 from a live review |
| TRC-043 | Ambient light has no place in path tracing | decided | P3 | S | none | — | RECORD.md |
| TRC-044 | Multiple importance sampling between specular and diffuse lobes | open | P3 | M | none | — | RECORD.md |
| TRC-045 | A distributable denoiser (A-SVGF) if NRD's licence blocks shipping | open | P3 | L | none | — | rt-backend §12.1 fallback |
| TRC-046 | Ray-traced shadows / AO / reflections replacing the raster equivalents | open | P3 | L | none | TRC-020 | rt-backend §10.3 |

## FID — retro fidelity

`FIDELITY.md` holds all 37 rows with their evidence, statuses
and required proofs. Only the rows with no owner elsewhere are repeated here, so
the two files do not drift. **Everything in that file is in scope; this is the
index, not a subset.**

| ID | Title | St | Pri | Eff | Owner | Blocked by | Provenance |
|---|---|---|---|---|---|---|---|
| FID-001 | Alpha-test reference value (0.5 vs the reference 0.1) | open | P1 | S | phase-f G8 | RAS-003 | FIDELITY #19 |
| FID-002 | The opaque range is untested and reaches it too easily | unproven | P1 | M | phase-f G8 | RAS-003 | FIDELITY #20; needs a content scan first |
| FID-003 | Emitter fidelity set: tint, rotation, size, order, jitter, alignment | open | P2 | L | S3 | STR-008 | FIDELITY #8–#14, #22 |
| FID-004 | Light selection policy (32 global, no per-model cap, no frustum cull) | unproven | P2 | M | none | — | FIDELITY #23; treat 8/3 as one reimplementation's model |
| FID-005 | Dynamic light on lightmapped geometry | unproven | P2 | M | none | — | FIDELITY #24 |
| FID-006 | `SunShadows` / `MoonShadows` parsed and ignored | open | P2 | S | none | — | FIDELITY #25 |
| FID-007 | No shadow or soft-shadow option | unproven | P3 | S | none | — | FIDELITY #26; the ini proves the settings existed, not the technique |
| FID-008 | Fog distance metric is radial, references are planar | unproven | P3 | S | none | — | FIDELITY #27; one-line change once a capture exists |
| FID-009 | Anisotropic filtering on by default (4×) | unproven | P2 | S | none | — | FIDELITY #28; check the retail ini and KVP sampler state first |
| FID-010 | TXI `filter` and `mipmap` flags unparsed | open | P2 | S | none | — | FIDELITY #29 |
| FID-011 | BC textures with no shipped mip chain get none | unproven | P3 | S | none | — | FIDELITY #30 |
| FID-012 | Object draw distance is dead code and the slider does nothing | open | P2 | S | none | — | FIDELITY #31; supersedes backlog 3.6 |
| FID-013 | Grass distance policy (camera vs player, 32 vs 25→100, no dialogue exemption) | unproven | P2 | M | phase-f R3 | — | FIDELITY #32 |
| FID-014 | Destroy fade ignored (`bNoFade`, `fDelayUntilFade`) | open | P2 | S | none | RAS-005 | FIDELITY #33 |
| FID-015 | Meshes with a null diffuse texture are dropped | unproven | P3 | S | none | — | FIDELITY #34; needs a content scan |
| FID-016 | Self-illumination operator: multiply vs add | unproven | P2 | S | none | — | FIDELITY #35; the references disagree |
| FID-017 | Face culling: G-buffer and blended two-sided, shadow pass culls | unproven | P3 | S | none | — | FIDELITY #5 |
| FID-018 | Shadow-caster scope: the authored per-node flag exists and is unread | open | P2 | M | phase-f G7 | — | FIDELITY #16 |
| FID-019 | TSL `n_forcezombie` model load aborts on a non-NUL-terminated name | unproven | P3 | S | none | — | FIDELITY #15 |
| FID-020 | Build the eleven named fixtures | open | P1 | M | none | — | retro fixture set; every other FID row needs one |
| FID-021 | Three content scans before any code change | open | P1 | S | none | — | retro scan list; gates FID-002, FID-010, FID-015 |

## SYS — game systems

Largely untouched by renderer work and by volume the biggest block in the repo.

| ID | Title | St | Pri | Eff | Owner | Blocked by | Provenance |
|---|---|---|---|---|---|---|---|
| SYS-001 | 60 of 64 effect `.cpp`, 25 header-only effects, 15 of 40 actions are stubs | open | P2 | L | none | — | backlog 6.1 |
| SYS-002 | Serialisation gaps: placeable/door effects and actions, items, `LastDisturbed` | open | P1 | M | none | — | backlog 6.4 |
| SYS-003 | Combat gaps: Dueling, Two-Weapon Fighting, damage effects, history | open | P2 | M | none | — | backlog 6.3 |
| SYS-004 | Action bar missing Force powers, mines, TSL slots | open | P2 | M | none | — | backlog 6.5 |
| SYS-005 | Hardcoded 1.7 m LOS/listener height at six sites | open | P3 | M | none | — | backlog 6.6; collapse to one accessor first |
| SYS-006 | Assorted small fixes (low-HP animations, selectability, listbox CTD, …) | open | P3 | M | none | — | backlog 6.8 |

## TOOL — build, tooling, diagnostics, process

| ID | Title | St | Pri | Eff | Owner | Blocked by | Provenance |
|---|---|---|---|---|---|---|---|
| TOOL-001 | **`tests.exe` heap corruption on a cold cache** | open | P0 | M | S1 residue | — | DESIGN.md; engine fixed 4/4; committed tree measured 0/4 against a stashed control 2026-08-05, worse than the "1/3" this row used to claim — RECORD.md 1.13; `ISession` churn ruled out |
| TOOL-002 | **GPU timing in `IStatistic`** | open | P1 | M | S0 | — | backlog 2.5; blocks every performance claim in TRC and STR |
| TOOL-003 | Test the editor's Save buttons end to end | open | P0 | S | none | — | backlog 0.2; read-modify-write preserves foreign lines by position |
| TOOL-004 | Rebuild and smoke-test the Debug configuration | open | P0 | S | none | — | backlog 0.4; the checked VMA is the instrument for lifetime bugs |
| TOOL-005 | Exercise the checked-VMA Debug path to completion | open | P1 | S | none | TOOL-004 | DESIGN.md; a Debug capture blocked in `endFrame` |
| TOOL-006 | Suppress wall-clock readouts under `isCaptureRun` | open | P1 | S | none | — | backlog 7.1; the flag exists, nothing at the draw consults it |
| TOOL-007 | Fold the uniform blocks onto the Slang-runtime path | open | P2 | M | S1 | — | DESIGN.md; two schema mechanisms coexist |
| TOOL-008 | Dead shader inventory: 8 of 20 modules have no pipeline consumer | open | P2 | S | S1 | — | DESIGN.md; paid on every cold start |
| TOOL-009 | Isolation fixtures: grass cluster placement fails at close range | open | P2 | S | none | — | backlog 7.7; zero clusters at 12 units |
| TOOL-010 | Runtime resolution change is unfinished | open | P2 | M | none | — | backlog 4.5; cameras never re-apply projection |
| TOOL-011 | Retro publishes fewer render targets than PBR | open | P2 | M | none | — | backlog 4.6 — **but see Closed: the audit says this already landed** |
| TOOL-012 | Stale descriptor sets in the render-target viewer across a resize | unproven | P3 | S | none | TOOL-010 | backlog 4.10; never reproduced |
| TOOL-013 | Delete the vendored FSR 3.1.4 SDK (436 MB) | open | P3 | S | none | — | backlog 4.3 |
| TOOL-014 | Ninja generator evaluation | blocked | P2 | M | none | FSR2 external build | backlog 4.4 |
| TOOL-015 | Measure a clean build before/after the build-speed work | open | P2 | S | none | — | backlog 0.6 |
| TOOL-016 | Whole-frame parity baseline for danm14ab frame 900 is invalid | open | P2 | S | none | — | backlog 7.3; no replacement figure exists |
| TOOL-017 | A settings tool honest about the three runtime tiers | open | P2 | M | none | — | vulkan-remaining §2 |
| TOOL-018 | Move the `grass` gate from area load to draw time | open | P3 | S | none | — | vulkan-remaining §2b |
| TOOL-019 | Debug view modes (lights, bounds, emissives, object type) | open | P3 | M | none | — | RECORD.md |
| TOOL-020 | Rename the registry panel to "Scene viewer" | open | P3 | S | none | — | RECORD.md |
| TOOL-021 | Main screen render is cropped rather than scaled | open | P2 | S | none | — | RECORD.md |
| TOOL-022 | Add a Dxun exterior to the K2 fixture set before volumetrics | open | P3 | S | none | TRC-028 | DESIGN.md |
| TOOL-023 | Retire the three obsolete planning documents into a history folder | open | P3 | S | none | — | this consolidation; see README provenance table |

---

## Closed by the 2026-08-05 staleness audit

Items the documents still listed as pending that the code shows are done. Kept
so the closure is answerable, not deleted.

| Was | Title | Evidence |
|---|---|---|
| backlog 3.3 | Deforming shadow casters | `shadow_megadraw` draws already-skinned merged geometry — `scenepipeline.cpp:411-416`; `e7a4f5b6` |
| backlog 3.6 | Draw-distance culling in shadow passes | The cited symbol was removed in `db668c2f`; the real residue is FID-012 |
| backlog 4.1 | `toolkit.exe` link failure | Linked in two full Release builds 2026-08-05; binary on disk |
| backlog 4.6 | Retro publishes only its final Output | Both raster modes use the same target enumeration — `scenepipeline.cpp:969-985`. **TOOL-011 should be verified and closed.** |
| backlog 4.9 | Compile shaders in the engine with Slang | Landed as S1 — `a6dc6ed1` |
| backlog 9.3 | Bring raster onto the merged buffer | Landed as the G track |
| vulkan-remaining §1 | Render-target viewer empty on Vulkan | `targets()` forwards a populated list — `vulkan.cpp:144-163` |
| vulkan-remaining §2 | Grass gated only at area load | `--grassdensity` is live per frame — `admission.cpp:486-505`; `1b6559aa` |
| vulkan-remaining §5 | Retro is OpenGL-only and forward-rendered | Retro is G-buffer-based on Vulkan — `802ec6c8` |
| vulkan-remaining §6 | Registration rework outstanding | Replaced by direct `GpuScene` collection — `8d37449d` |
| cleanup-plan F-geo | "F2 done, F3 next" | The successors all shipped — `ef6c5850`, `802ec6c8`, `e7a4f5b6` |
| rt-backend §10.2 | Per-mesh BLAS with refits | One scene BLAS since `9b98c37c`; no `refit`/`MODE_UPDATE` in the tree |
| rt-backend §12 | FSR "not started", NRD "not chosen" | Both shipped |
| backlog 8.1–8.6 | Per-mesh loop optimisations | The loop no longer exists — `9b98c37c` |
