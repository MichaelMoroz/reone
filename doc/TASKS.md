# Open tasks

Read [README.md](README.md) for the ID scheme, states and priorities, and
[GLOSSARY.md](GLOSSARY.md) for the vocabulary. Shapes and acceptance criteria are
in [DESIGN.md](DESIGN.md); fidelity evidence is in [FIDELITY.md](FIDELITY.md).

Re-audited against the tree 2026-09-22. **Items the code shows are built were removed,
not marked done** — git history is where they went. The audit was thorough for every ID a
commit since 2026-08-09 mentions and for anything cheaply checkable by grep; rows it could
not settle that way kept their previous state rather than being upgraded on optimism.

## RAS — raster

| ID | Title | St | Pri | Notes |
|---|---|---|---|---|
| RAS-003 | G8 — the blended pass and the three alpha kinds | open | P1 | Draw built; how lit transparency is *lit* is open. See TRC-019 |
| RAS-004 | G8 — the authored `transparencyHint` as the blended sign | open | P2 |  |
| RAS-005 | G8 — per-node alpha fade must actually render | open | P2 | Every door and placeable fade is lost. Fix with RAS-004 |
| RAS-006 | G8 — the transparency sort remap | blocked | P3 | Revives only if D2 reverses |
| RAS-007 | G8 — authored emitter `renderOrder` in the ordering decision | blocked | P3 | Same gate as RAS-006 |
| RAS-008 | G8 — additive without alpha uses `SRC_COLOR/ONE` | open | P2 |  |
| RAS-009 | G8 — `Punch-Through` emitter blend is dropped | open | P2 | Hard-edged cards render soft |
| RAS-010 | G9 — lens flares | open | P2 | AA and bloom are built. Starts at admission, not in a shader. See STR-010 |
| RAS-013 | Bump has never been held to a fixture | open | P2 |  |
| RAS-014 | Metal at LOD 0 filters nothing at distance | open | P2 | A compute resolve has no implicit derivatives, so a footprint-derived LOD is the only answer |
| RAS-015 | Textured roughness/metalness, in one step for both consumers | open | P2 | One-sided adoption breaks the shared-material rule |
| RAS-016 | Sky cubemap keeps 0.73 of the geometry sky's horizontal detail | open | P3 | |
| RAS-017 | Collapse per-body-part registration (30 draws for one droid) | open | P3 |  |
| RAS-018 | Per-pass instance re-copy in the draw walk | open | P3 | |
| RAS-021 | Measure phase F on low-end hardware; per-mesh draws may win | open | P3 | |
| RAS-022 | Far-hills shadow breakdown: cascade seam, undersampled far cascades | open | P2 | |
| RAS-023 | Dantooine's sun fades in over the first 30 frames | open | P2 | |
| RAS-025 | Retro renders emissive textures as plain white | open | P1 | The per-category emission dial stops at PBR and path tracing |
| RAS-027 | Particle spawning is frame-quantised, so density follows frame rate | open | P2 | |
| RAS-029 | Frame-timed animation behaviour that should be game-timed | open | P2 | |
| RAS-031 | Warp and save-load need a renderer reset to render correctly | open | P1 | Module changes leave renderer state behind |
| RAS-032 | Stale GL-era premises in the shader headers | open | P3 | |
| RAS-034 | Grass size ramp fades from a distance the cull no longer reaches | open | P2 | Ramp at 32, cull at 25, so it never completes |
| RAS-037 | SSR as a specular provider inside the channel contract | open | P2 |  |
| RAS-040 | White-furnace check of the material model | open | P2 | `E_spec + diffuse albedo <= 1` per roughness in the testbed |

## STR — scene and structural

| ID | Title | St | Pri | Notes |
|---|---|---|---|---|
| STR-001 | S0 — sub-zone `SceneGraph::update` and `Game::update` | open | P1 | `src/libs/game/` has no zones. No `updateAnimations` exists — the walk is an inline loop |
| STR-002 | S0 — the scaling fixture (the no-creep tripwire) | open | P1 | Spawn N, capture, repeat at 10×N, assert render-side CPU flat in N |
| STR-003 | S0 — update the diagnostics skill's calibration line | open | P2 |  |
| STR-004 | S2 — nodes own GPU slots | open | P1 | Unblocked: G8's vocabulary is quiet |
| STR-005 | S2 — fix `SceneGraph::_nodes` never releasing | open | P1 | Fix as part of STR-004, not around it |
| STR-006 | S3 — skin palettes become event-driven | open | P2 |  |
| STR-007 | S3 — dangly moves into the merge kernel | open | P2 |  |
| STR-008 | S3 — particles simulate on the GPU | open | P2 | |
| STR-009 | S3 — `settleMeshTransform` dies | open | P2 | |
| STR-010 | S3 — flare LOS becomes a GPU visibility query | open | P2 | The CPU LOS walk does not survive both this and RAS-010 |
| STR-011 | S4 — decide what "static" is provable from | open | P2 | The admission proof, not the `staticObject` hint the code distrusts |
| STR-012 | S4 — static/dynamic partition of buffer and material table | open | P2 | danm14ab: 45k of 86k triangles static |
| STR-013 | S4 — remove the per-element binary search in the merge | open | P2 |  |
| STR-019 | S6 — the legacy per-mesh path | open | P3 | Two consumers: the sky bake and the walkmesh debug draw. The bake is staying, so both move onto the merged path or the item closes |
| STR-020 | S6 — delete `Registered*` and the frame-phase surface | blocked | P3 |  |
| STR-021 | S6 — delete the per-draw texture-set path | blocked | P3 |  |
| STR-022 | Room visibility: the VIS graph is gone | open | P2 | Parsed and never read; every room always drawn. Gated on D1 |
| STR-023 | Debug geometry returns inside `GpuScene`, in a non-BLAS region | open | P3 |  |
| STR-024 | `admitted` vs `drawn` has no mechanism under ranges | open | P3 | |
| STR-025 | Walkmesh geometry deletion is a game-model refactor | open | P3 |  |
| STR-026 | Frame floor outside the renderer (the ~2 ms update slot) | open | P3 | |
| STR-027 | In-phase wind: every dangly tree moves identically forever | open | P3 | Seeding from the node id would destroy BLAS sharing — decide which is worth more |

## TRC — path tracing

| ID | Title | St | Pri | Notes |
|---|---|---|---|---|
| TRC-001 | Smoke particles produce non-finite pixels; the denoiser spreads them | open | P0 | |
| TRC-002 | NRD accumulation is nondeterministic | open | P1 | |
| TRC-003 | Run the FSR convergence harness and record the numbers | open | P2 | |
| TRC-004 | Record why growing `InstanceMaterial` caused `VK_ERROR_DEVICE_LOST` | open | P2 | A grown struct changed array stride. Keep GPU-shared arrays power-of-two |
| TRC-005 | Verify specular demodulation on chromatic `Rf0` | open | P2 | |
| TRC-006 | Residual diffuse demodulation leak (corr 0.145, was 0.271) | open | P2 | |
| TRC-007 | Specular `F` vs `Fenv` mismatch | open | P2 | |
| TRC-008 | Diffuse channel outliers (max 25.3 vs p99.99 1.30) | open | P2 | |
| TRC-009 | FSR reactive and transparency/composition masks are null | open | P2 | |
| TRC-010 | Light selection table evaluated twice per path vertex | open | P2 | |
| TRC-011 | Sphere lights sampled over the whole cone, not the visible cap | open | P2 | |
| TRC-012 | Lightmaps treated as albedo where physically wrong | open | P2 | |
| TRC-013 | Feed NRD `IN_DIFF_CONFIDENCE` / `IN_SPEC_CONFIDENCE` | open | P2 | |
| TRC-014 | Feed NRD `IN_DISOCCLUSION_THRESHOLD_MIX` | open | P3 | |
| TRC-015 | Evaluate NRD's SH path + `NRD_SG_ReJitter` | open | P3 | |
| TRC-016 | Channel debug views need their own exposure | open | P3 | |
| TRC-017 | Material PBR-ification: authored name→material map, then heuristic | open | P2 |  |
| TRC-019 | Traced transparency: stochastic commits, flat additive loop, analytic sabers | decided | P2 | Design in [LESSONS.md](LESSONS.md) |
| TRC-021 | A cross-consumer scene validator not dependent on judging an image | open | P1 | **Premise replaced.** The traced G-buffer derives from raster and agrees by construction. A cross-check that cannot fail is not one, and it still produces plausible numbers |
| TRC-022 | The fog grid, the march, and additive as spheres/capsules | open | P2 | |
| TRC-023 | Honour the authored emitter `tinted` flag when baking media | open | P3 |  |
| TRC-024 | Sky: whole-room granularity swallows the props | open | P1 | `001ebo16` is one lump — shell plus three asteroids, a planet and a nebula. The reserved `meshes =` key is ignored |
| TRC-025 | Sky coverage curation | open | P1 | 418 K1 and 290 K2 entries still `# review`. A wrong room name silently suppresses level geometry |
| TRC-026 | ReGIR: ReSTIR reservoirs in a world-space hash grid | open | P2 |  |
| TRC-027 | SHARC radiance cache, then resample into a dense volume | open | P3 | |
| TRC-028 | Volumetrics consuming the grid (prototype transmittance first) | open | P3 | |
| TRC-029 | Next-event estimation over the full light list | open | P2 | At our counts a light BVH answers a question this N does not pose |
| TRC-030 | Emissive geometry into the sampled light set | open | P2 | |
| TRC-032 | Explain 7.2% occupancy on the primary-ray dispatch | open | P2 | |
| TRC-033 | `HitGeometry` and `SurfaceShading` are the remaining live state | open | P3 | |
| TRC-034 | BLAS compaction is not implemented | open | P3 |  |
| TRC-035 | Verify AMD's Vulkan BLAS build path before trusting per-frame rebuild | open | P2 |  |
| TRC-036 | Is 86k triangles typical? Log counts across a warp loop | open | P3 | |
| TRC-037 | Re-trace the current renderer; the old 12 ms predates the merge | open | P2 | Every old traced frame-time figure is invalid for this reason |
| TRC-038 | SER, once a Slang release emits the instruction | blocked | P3 | |
| TRC-039 | Sky leaks into fully enclosed scenes (Taris underground) | open | P1 | |
| TRC-040 | Bounce lighting does not exist for analytic lights | open | P1 | NEE runs at the primary hit only; TRC-026 fixes it |
| TRC-041 | Some objects do not interact with lighting (leaves, hair, doors) | open | P1 | |
| TRC-042 | Calibration: tonemapper, intensities, bounces, sky scale | open | P1 | |
| TRC-043 | Ambient light has no place in path tracing | decided | P2 | |
| TRC-044 | MIS between specular and diffuse lobes | open | P2 | |
| TRC-045 | A distributable denoiser (A-SVGF) if NRD's licence blocks shipping | open | P3 | |
| TRC-046 | Ray-traced shadows / AO / reflections replacing the raster equivalents | open | P3 | |
| TRC-051 | Grass in the TLAS as chunked cluster BLASes | open | P2 | |

## FID — retro fidelity

Titles only; **status, evidence and the proof each needs are in
[FIDELITY.md](FIDELITY.md)**, which is the working document for this area.

| ID | Title |
|---|---|
| FID-001 | Alpha-test reference value (0.5 vs the reference 0.1) |
| FID-002 | The opaque range is untested and reaches it too easily |
| FID-003 | Emitter fidelity set: tint, rotation, size, order, jitter, alignment |
| FID-004 | Light selection policy |
| FID-005 | Dynamic light on lightmapped geometry |
| FID-006 | `SunShadows` / `MoonShadows` parsed and ignored |
| FID-007 | No shadow or soft-shadow option |
| FID-008 | Fog distance metric is radial, references are planar |
| FID-009 | Anisotropic filtering on by default |
| FID-010 | TXI `filter` and `mipmap` flags unparsed |
| FID-011 | BC textures with no shipped mip chain get none |
| FID-012 | Object draw distance is dead code and the slider does nothing |
| FID-013 | Grass distance policy |
| FID-014 | Destroy fade ignored (`bNoFade`, `fDelayUntilFade`) |
| FID-015 | Meshes with a null diffuse texture are dropped |
| FID-017 | Face culling: the tree is uniformly two-sided by decision |
| FID-018 | Shadow-caster scope within a mode: the per-node flag is unread |
| FID-019 | TSL `n_forcezombie` load aborts on a non-NUL-terminated name |
| FID-020 | Build the eleven named fixtures |
| FID-021 | Three content scans before any code change |

## SYS — game systems

The largest block of deferred work in the repository, least connected to the renderer
tracks. All unowned, all P2-P3.

| ID | Title |
|---|---|
| SYS-001 | 60 of 64 effect `.cpp`, 25 header-only effects, 15 of 40 actions are stubs |
| SYS-002 | Serialisation gaps: placeable/door effects and actions, items, `LastDisturbed` |
| SYS-003 | Combat gaps: Dueling, Two-Weapon Fighting, damage effects, history |
| SYS-004 | Action bar missing Force powers, mines, TSL slots |
| SYS-005 | Hardcoded 1.7 m LOS/listener height at six sites |
| SYS-006 | Assorted small fixes (low-HP animations, selectability, listbox CTD) |

## TOOL — build, tooling, diagnostics, process

| ID | Title | St | Pri | Notes |
|---|---|---|---|---|
| TOOL-002 | GPU timing in `IStatistic` | open | P1 | Blocks every performance claim — the graphics zone measures CPU recording time, not GPU |
| TOOL-003 | Test the editor's Save buttons end to end | open | P2 | |
| TOOL-004 | Rebuild and smoke-test the Debug configuration | open | P2 | |
| TOOL-005 | Exercise the checked-VMA Debug path to completion | open | P2 | The standing instrument for the lifetime bug class; last Debug capture blocked in `endFrame` |
| TOOL-006 | Suppress wall-clock readouts under `isCaptureRun` | open | P2 |  |
| TOOL-009 | Isolation fixtures: grass cluster placement fails at close range | open | P2 | |
| TOOL-012 | Stale descriptor sets in the render-target viewer across a resize | unproven | P2 | |
| TOOL-016 | Whole-frame parity baseline for danm14ab frame 900 is invalid | open | P2 |  |
| TOOL-017 | A settings tool honest about the three runtime tiers | open | P2 | The tiers are `OptionApply::Live/Reapply/Restart` in code |
| TOOL-018 | Move the `grass` gate from area load to draw time | open | P3 | Grass is already live; the shape recurs for the next load-gated option |
| TOOL-019 | Debug view modes (lights, bounds, emissives, object type) | open | P3 | |
| TOOL-021 | Main screen render is cropped rather than scaled | open | P2 | Carries the unfinished half of runtime resolution changes |
| TOOL-022 | Add a Dxun exterior to the K2 fixture set before volumetrics | open | P2 |  |
| TOOL-024 | A startup schema mismatch segfaults instead of naming the field | open | P1 | The message is correct; the engine can fault before printing it |
| TOOL-025 | NRD shader blobs can be stale/empty and CMake accepts them | open | P0 |  |
| TOOL-028 | Draw distance has three disagreeing ranges and no consumer | open | P2 | Launcher 32–128, the in-engine dial another, the cull a third |
| TOOL-029 | Retro renders three screens visibly unlike the build the GUI came from | open | P1 | |
| TOOL-030 | Seven capture states outside tolerance at 3440×1440, by 2 to 10 pixels | open | P2 | |
| TOOL-031 | Tracer-side poison visibility | open | P3 | Secondary-hit sampling bypasses `scene_draw`, so a stale id reached only by secondary rays stays dark instead of fullbright. Extend only if field diagnosis needs it |
| TOOL-032 | Geometry-range epochs | open | P3 | Audited, no hazard; revisit only if merged ids start crossing frame boundaries outside the checked upload |
| TOOL-033 | `CMakeLists.txt` still probes for `slangc` | open | P3 | Nothing reads it — one schema mechanism, one misleading build message |

---

# Decisions a person has to settle

Each names both sides. Where the code settles the factual half, that is stated; the
*policy* half is still a choice.

**D1 — room visibility.** The VIS branch was deleted on measurement: removing ~9,000
frustum tests per frame moved frame time by nothing. But every room is now always drawn,
the `.vis` graph is parsed and never read, and **both reference engines apply room
adjacency camera-independently** — the property the deletion gave up rather than adopted,
since retail had adjacency indoors and all-rooms outdoors and we implement only the
second. The performance claim is not in dispute; **the question is whether retro accepts
drawing rooms the original hid.** Owner: none. Blocks STR-022, and interacts with the sky
since every backdrop room but one is world geometry.

**D2 — transparent ordering.** A CPU sort feeding a per-triangle remap was designed
against the finding that kvp-main's replay reorders only true-opaque depth-writing
geometry, so a remap sort produces regressions the original did not have. **The second
won and the draw ships in admission order.** The row survives because the escape hatch
still points at the remap and its two-read hazard is a real trap. Owner: G8. Blocks
RAS-006, RAS-007.

**D4 — static/dynamic BLAS.** One buffer partitioned static and dynamic with two BLAS was
argued at length; what shipped is one BLAS rebuilt every frame, with the split kept as an
escape hatch pending the AMD measurement. Owner: S4. **Settle by doing TRC-035 first.**

**D5 — debug and walkmesh geometry: delete or re-admit.** One side: delete rather than
exclude, because it carries `offMaterial`, the one attribute `MergedVertex` structurally
cannot express. The other, newer and better evidenced: the merged stream already carries a
per-triangle material id, so walkable versus non-walkable is two materials over one mesh
and no vertex format widens. Owner: none. Blocks STR-023, STR-025.
