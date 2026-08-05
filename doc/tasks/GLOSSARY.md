# Glossary

What the vocabulary in [MASTER.md](MASTER.md) and the planning documents means.
Definitions are drawn from how the documents actually use each term, with the
place it is best explained. Where a term is dead, that is said outright — several
appear in commit messages and older docs and mean nothing now.

## Step names

| Term | Meaning | Best explained at |
|---|---|---|
| **G1–G9** | The raster track: rebuild raster on `GpuScene`. G1 delete the old path, G2 one draw and one G-buffer, G3 one scene description, G4 kill the GL legacy, G5 retro shading, G6 PBR shading (G6b material id, G6c the traced shading model), G7 shadows from real geometry, G8 the blended pass, G9 the shared output stage. | `DESIGN.md` |
| **R1–R3** | Runtime-cost work cutting across G: R1 persistent registration, R2 delete the translation layer, R3 grass on the GPU. R2 is now STR-004. | `DESIGN.md` |
| **S0–S6** | The structural track: S0 instrument, S1 Slang in the engine and one schema, S2 nodes own GPU slots, S3 per-frame CPU work becomes events or GPU, S4 residency, S5 the RHI, S6 deletions. | `DESIGN.md` |
| **V0–V5** | Visibility and sky, inside the PT substage: V0 fix the baker, V1 hybridise, V2 composite the sky once, V3 the tracer keeps only transport, V4 suppress exactly the shell, V5 delete the runtime bake. | `DESIGN.md` |
| **PT substage** | Everything traced, sequenced after G8 and G9. | `DESIGN.md` |
| **F0–F10** | **Dead numbering.** Outlived three restructures; commits before 2026-08-02 refer to F0/F1/F2, which are done. `cleanup-plan.md` still uses F-geo/F-vis and its cross-references will mis-resolve. | `DESIGN.md` |
| **Phases A–E** | `cleanup-plan.md`'s lettering: A remove OpenGL, B0 one source buffer, B extract `GpuScene`, C residency in the contract, D admit everything, E Vulkan containment. All done. | `RECORD.md` |

## The scene pipeline

| Term | Meaning | Best explained at |
|---|---|---|
| **`GpuScene`** | The one path by which scene geometry becomes GPU data: admission, then one compute dispatch, then merged world-space geometry, plus stable primitive-to-object identity, residency and lifetime. Must not require ray tracing. | `RECORD.md` |
| **admission** | The shared, renderer-neutral decision about which objects are merged and how they are classified. Extracted in G3 so neither renderer owns it. | `DESIGN.md` |
| **merged geometry** | One world-space vertex and index stream for the whole scene with a per-triangle material id, consumed by both the BLAS build and the raster draws. | `RECORD.md` |
| **merge / skin dispatch** | The single compute dispatch (`slang/skin.slang`) that writes the merged buffer, applying skinning, dangly, saber displacement and grass expansion. | `RECORD.md` |
| **megadraw** | One indexed draw over the merged buffer pulling vertices by `SV_VertexID` — the raster counterpart of the single BLAS. `shadow_megadraw` is the shadow pass form. | `DESIGN.md` |
| **procedural quad** | The one device-side record kind that grass, particles and billboards all lower into. | `DESIGN.md` |
| **residency class / Region** | The contract-level declaration that a range of merged geometry is static or dynamic. Present since Phase C, still unused — S4 is what consumes it. | `DESIGN.md` |
| **change-log** | S2's sync mechanism: mutations append `(slot, range, payload)`; each frame replays what its in-flight copy has not seen. | `DESIGN.md` |
| **`Registered*` / `ObjectRecord`** | The per-frame intermediate records S2 and S6 delete. Their raw pointers into node-owned vectors are the boundary's standing lifetime hazard. | `DESIGN.md` |
| **upload hash** | A hash of the whole `GpuSceneUpload` required to be **equal across all three modes** at the same camera and frame — the direct test that scene policy has not forked. | `DESIGN.md` |
| **shadow oracle** | The full per-frame rebuild kept behind `--admissionshadow`, hashed against the incremental path every frame to catch stale-invalidation bugs. | `DESIGN.md` |

## Rendering

| Term | Meaning | Best explained at |
|---|---|---|
| **retro / PBR / path tracing** | The three `RenderMode` values. Retro preserves the original look, so a deviation is a bug. PBR and path tracing are held to looking right, not to matching 2003. | `DESIGN.md` |
| **`RTDebug`** | A **planned** fourth mode holding the ray-traced G-buffer as a permanent cross-renderer validation instrument. Does not exist yet. | `RECORD.md` |
| **BLAS / TLAS** | Bottom- and top-level acceleration structures. Since `9b98c37c`: one BLAS over the whole merged scene (two geometries, opaque and non-opaque) and one TLAS instance. | `RECORD.md` |
| **punch-through / cutout / lit-blended / additive-emissive** | G3's one classification vocabulary; everything downstream derives from it mechanically. | `DESIGN.md` |
| **coverage-as-transmission** | The tracer's transparency model — shade a blended surface, weight by alpha, continue with `1−α`. A primary-ray model, undermined by the hybrid decision. | `RECORD.md` |
| **`noiseFree`** | The tracer output holding what must bypass the denoiser. Anything routed here must be computed deterministically. | `CONVENTIONS.md` |
| **demodulation** | Dividing albedo out of diffuse (and the Fresnel term out of specular) before NRD, so the denoiser sees transport rather than texture. | `CONVENTIONS.md` |
| **NRD** | NVIDIA Real-Time Denoisers. GAPI-free: it returns dispatches we record. Proprietary, so an optional CMake component, off by default. | `CONVENTIONS.md` |
| **FSR** | AMD FidelityFX Super Resolution 2.2.1 run at **NativeAA** — no upscaling, so it functions purely as temporal anti-aliasing. Requires jitter on. | `CONVENTIONS.md` |
| **hashed alpha** | A stochastic alpha test, removed from the gated raster draw because a stochastic test cannot match the tracer's deterministic one. | `DESIGN.md` |
| **cascades / CSM** | Four-cascade shadow maps with a rotated 16-tap Poisson kernel held to a fixed world-space width. A modern reconstruction with no retail reference. | `DESIGN.md` |
| **curated materials / `TraceClass`** | Per-model and per-node authored trace classification plus albedo, roughness, metallic and emission overrides, keyed by interned model and node name. | `RECORD.md` |
| **sky bake** | Rendering a sky shell into a cubemap by casting rays from inside it. **Two exist**: the offline `src/apps/skybake` (the intended survivor, currently with no consumer) and `VulkanRayQuery::bakeSkyRoom` (the runtime one, which V5 deletes). | `DESIGN.md` |

## Measurement and process

| Term | Meaning | Best explained at |
|---|---|---|
| **update slot / graphics slot** | The two top-level Tracy zones. The graphics slot measures CPU time *recording* the frame, not GPU execution — which is why TOOL-002 blocks every performance claim. | `DESIGN.md` |
| **testbed** | A synthetic module (`warp testbed`, with `grass` and `smoke` variants) used to isolate one geometry class at a time. | `RECORD.md` |
| **scaling fixture** | S0's no-creep tripwire: spawn N of one blueprint, capture Tracy, repeat at 10×N, assert render-side CPU zones are flat in N. | `DESIGN.md` |
| **`--dumptargets` / `--capture` / `--commands-frame`** | The headless harness: dump every exposed target as `.npy`, capture a frame by index, run a command list at a chosen frame. | `.claude/skills/reone-diagnostics/SKILL.md` |
| **`--dev 0`** | Hides the frame-time readout that otherwise writes text into the captured image and forges a per-run difference. Mandatory for any capture. | `RECORD.md` |
| **`isCaptureRun`** | The predicate that makes a capture deterministic (fixed timestep, input dropped, seeded RNG). Exists, but nothing at the draw consults it yet — TOOL-006. | `RECORD.md` |
| **xoreos / KotOR.js / kvp-main** | The three reference engines, with divided authority: xoreos on fixed-function GL semantics, KotOR.js on gameplay policy and MDL controllers, kvp-main on anything observed from the retail draw stream. Checkout location varies per machine. | `DESIGN.md` |

## Dead terms

Encountered in older documents and commit messages; they no longer denote
anything in the tree.

| Term | What happened |
|---|---|
| **`RenderRegistry`** | Deleted in `8d37449d`. Read every "registry" in the older documents as `GpuScene`, which inherited the behaviour rather than replacing it. |
| **`compile_spirv` / `shaderpack`** | The build-time SPIR-V step and its application, deleted with S1. Shaders compile in-process at startup. |
| **`--backend` / `--slangshaders`** | Backend selection died with OpenGL in `df1aa375`. There is one backend. |
| **`ShadowCaster` / shadow proxies** | Deleted in G7; shadows draw real merged geometry. |
| **`VisibilityPolicy` / `drawDistanceCamera`** | Deleted in `db668c2f` along with frustum and draw-distance culling. |
| **`IUniforms` / `glToVulkanClip`** | Deleted in G4; matrices are born in Vulkan clip space. |
| **F-geo / F-vis** | `cleanup-plan.md`'s split, superseded by the G and V tracks. |
