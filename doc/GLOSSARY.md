# Glossary

Vocabulary used in [TASKS.md](TASKS.md) and [DESIGN.md](DESIGN.md). Where a term is
dead, that is said outright — several appear in commit messages and mean nothing now.

## Step names

**G1-G9** the raster track (rebuild raster on `GpuScene`): delete the old path, one draw
and one G-buffer, one scene description, kill the GL legacy, retro shading, PBR shading,
shadows from real geometry, the blended pass, the shared output stage · **R1-R3**
runtime-cost work cutting across G, of which R2 is now STR-004 · **S0-S6** the structural
track: instrument, Slang in the engine, nodes own GPU slots, per-frame CPU work becomes
events or GPU, residency, the RHI, deletions · **V0-V5** visibility and sky, **largely
overtaken** since V1 and V2 landed and V0/V4 were superseded when the offline baker was
deleted · **PT substage** everything traced · **F0-F10, F-geo, F-vis, Phases A-E** **dead
numbering** from three retired restructures, used by commits before 2026-08-02, whose
cross-references will mis-resolve.

## The scene pipeline

| Term | Meaning |
|---|---|
| **`GpuScene`** | The one path by which scene geometry becomes GPU data: admission, one compute dispatch, merged world-space geometry, plus stable primitive identity, residency and lifetime. **Must not require ray tracing.** |
| **admission** | The shared, renderer-neutral decision about which objects are merged and how they are classified, so neither renderer owns it. |
| **merged geometry** | One world-space vertex and index stream for the whole scene with a per-triangle material id, consumed by both the BLAS build and the raster draws. |
| **merge / skin dispatch** | The single compute dispatch (`scene_resolve.slang`) that writes the merged buffer, applying skinning, dangly, saber displacement and grass expansion. |
| **megadraw** | One indexed draw over the merged buffer pulling vertices by `SV_VertexID`. **The module of that name is gone** — the G-buffer, blended and both shadow passes are entry points in `scene_draw.slang`, though `MegaDrawPushConstants` and the `kMega*` constants survive. |
| **procedural quad** | The one device-side record kind that grass, particles and billboards all lower into. |
| **residency class / Region** | The contract-level declaration that a range of merged geometry is static or dynamic. **Present but unused** — S4 is what consumes it. |
| **change-log** | S2's sync mechanism: mutations append `(slot, range, payload)`; each frame replays what its in-flight copy has not seen. |
| **`Registered*` / `ObjectRecord`** | The per-frame intermediates S2 and S6 delete. Their raw pointers into node-owned vectors are the boundary's standing lifetime hazard. |
| **upload hash** | A hash of the whole `GpuSceneUpload`, required to be **equal across all three modes** at the same camera and frame — the direct test that scene policy has not forked. |
| **shadow oracle** | The full per-frame rebuild kept behind a flag and hashed against the incremental path every frame, to catch stale-invalidation bugs. |

## Rendering

| Term | Meaning |
|---|---|
| **retro / PBR / path tracing** | The three `RenderMode` values. **Retro preserves the original look, so a deviation is a bug**; the other two are held to looking right, not to matching 2003. |
| **caster set** | Which categories a mode's shadow map draws, as a bitmask on the shadow push constants rather than a separate submission. Retro's is creatures and equipment — all the original drew; the other modes' is everything opaque. |
| **light ceiling vs light budget** | Two numbers that used to be one: `kMaxLights = 64` sizes the uniform block, `maxLights = 48` is how many slots a frame fills. **A claim about "the light limit" has to say which.** |
| **thin** | A material bit meaning "a surface with no interior" — a leaf, a banner — lit from both sides and transmitting to its far side. It follows the cutout classification rather than the TXI, which is what finally gave it any surfaces at all. |
| **BLAS / TLAS** | One BLAS over the whole merged scene (two geometries, opaque and non-opaque) and one TLAS instance. |
| **punch-through / cutout / lit-blended / additive-emissive** | The one classification vocabulary; everything downstream derives from it mechanically. |
| **coverage-as-transmission** | Shade a blended surface, weight by alpha, continue with `1−α`. **Historical twice over:** the hybrid removed the camera ray it applied to, and shadow-ray visibility is now binary. |
| **`noiseFree`** | The tracer output holding what must bypass the denoiser. **Anything routed here must be computed deterministically.** |
| **demodulation** | Dividing albedo out of diffuse, and the Fresnel term out of specular, before NRD, so the denoiser sees transport rather than texture. |
| **NRD** | NVIDIA Real-Time Denoisers — GAPI-free, returning dispatches we record. Proprietary, so off by default. **Two instances:** diffuse+specular, and the primary-vertex direct channel alone. |
| **FSR** | FidelityFX Super Resolution at **NativeAA** — no upscaling, so purely temporal anti-aliasing. **It is the only thing that resolves jitter, and that is the whole rule:** `computeJitter` returns zero unless FSR occupies the AA slot, so `--antialiasing off` is how you turn jitter off. |
| **`ptShadowFilter`** | What settles the traced primary-vertex direct channel: `Off`, `Penumbra` (the engine's own blur, sized from the penumbra the geometry implies) or `Denoiser`. **Three architectures rather than a strength dial.** |
| **cascades / CSM** | Four-cascade shadow maps with a rotated 16-tap Poisson kernel at a fixed world-space width. **A modern reconstruction with no retail reference.** |
| **curated materials / `TraceClass`** | Per-model and per-node authored trace classification plus albedo, roughness, metallic and emission overrides, keyed by interned model and node name. |
| **sky bake** | Rendering a sky shell into a cubemap by casting rays from inside it. **One exists: the runtime `Sky::bakeSkyRoom`** — the offline `skybake` app and its output were deleted, since it never gained an engine consumer and its survey's real product was the curated list. |
| **curated sky list** | `override/<game>/modules.ini`, keyed by room model name, loaded by `scene::SkyRooms`. Replaced a runtime guess that read "room without a walkmesh" as sky, which classified no TSL sky at all. **A module with no entry has no sky, which is an answer rather than a gap.** |
| **backdrop cutout** | A shading class, not a bake: background scenery drawn as an alpha cutout takes the sky's unlit surface model and intensity, but stays admitted, rasterized and traced. |
| **`RTDebug`** | A **planned** fourth mode holding the ray-traced G-buffer as a validation instrument. **Never built and will not be** — TRC-021 is what remains of the need. |
| **hashed alpha** | A stochastic alpha test, deleted because it could not match the tracer's deterministic one. |

## Measurement and process

| Term | Meaning |
|---|---|
| **`cmake --preset ninja`** | The configure. Ninja Multi-Config, one build tree, Release and Debug from one configure. `ENABLE_FSR` on (MSVC), `ENABLE_NRD` off (licence), `ENABLE_TRACY` off. |
| **update slot / graphics slot** | The two top-level Tracy zones. **The graphics slot measures CPU time *recording* the frame, not GPU execution** — which is why TOOL-002 blocks every performance claim. Both exist only under `-DENABLE_TRACY=ON`. |
| **testbed** | A synthetic module (`warp testbed`, with `grass` and `smoke` variants) used to isolate one geometry class at a time. |
| **scaling fixture** | S0's no-creep tripwire: spawn N of one blueprint, capture, repeat at 10×N, assert render-side CPU zones flat in N. |
| **`--dumptargets` / `--capture` / `--commands-frame`** | The headless harness: dump every exposed target as `.npy`, capture a frame by index, run a command list at a chosen frame. |
| **scene capture** | The `scenecapture` command, writing the frame, every pipeline target, one image per debug channel, and the object list with names and classifications. **It exists so a pixel can be resolved to a triangle, an object id and a *name*** — guessing from a screenshot had already aimed three changes at the wrong geometry. |
| **`--dev 0`** | Hides the frame-time readout that otherwise writes text into the captured image and forges a per-run difference. **Mandatory for any capture.** |
| **`isCaptureRun`** | The predicate that makes a capture deterministic. Exists, but nothing at the draw consults it yet — TOOL-006. |
| **xoreos / KotOR.js / kvp-main** | The three reference engines, with divided authority: xoreos on fixed-function GL semantics, KotOR.js on gameplay policy and MDL controllers, kvp-main on anything observed from the retail draw stream. |

## Dead terms

Encountered in older commit messages; they denote nothing now. **`RenderRegistry`** —
read every "registry" in older text as `GpuScene`, which inherited the behaviour ·
**`compile_spirv` / `shaderpack`** — Slang compiles in-process behind a cache and
`recompileshaders` rebuilds live, **and the compiler prefers the source tree over the copy
beside the executable** · **`--backend` / `--slangshaders`** — died with OpenGL ·
**`ShadowCaster` / shadow proxies** — shadows draw real merged geometry ·
**`VisibilityPolicy` / `drawDistanceCamera`** — deleted with frustum and draw-distance
culling · **`IUniforms` / `glToVulkanClip`** — matrices are born in Vulkan clip space ·
**`--taajitter` / `--pttaablend`** — jitter is implied by FSR and nothing else ·
**`--pbr`** — a value of `--mode`, and passing it as a flag is a hard parse error, so an
old command line exits before rendering · **`megadraw_geometry`, `shadow_scene_draw`,
`skin`, `rayquery`, `nrd_composite`, `megadraw`** — read as `scene_draw`,
`scene_resolve`, `path_trace` and `nrd_resolve`.
