# Doc staleness report — 2026-08-05

## Summary

Audited the nine requested documents against the `path-tracing` working tree at
`c72f4e75` (retaining pre-existing local changes). 172 atomic verifiable claims
were checked: **21 stale**, **8 pending items already done**, **6
cross-document contradictions**, **126 verified current**, and **17 not
settled without a build/capture or external retail data**. All 70 commit IDs in
the documents resolve; the two remaining hash-looking strings are data hashes,
not commits (`5b6089c88c790882`, `a12866a1ebaac77d`).

Line numbers in older documents have broadly drifted. A line drift alone is not
reported as stale where the cited symbol and claim remain valid.

## Stale claims

| Doc | Location | Claim as written | What the tree actually shows | Evidence (file:line) | Confidence |
|---|---|---|---|---|---|
| backlog.md | 3.3, line 135 | `shadow.slang` is rigid, therefore skinned meshes never cast shadows. | The active shadow pipeline selects `shadow_megadraw`, which draws canonical merged geometry; the merge is already the skinned geometry path. `shadow.slang` remains as a source module but has no pipeline-key consumer. | `src/libs/graphics/vulkan/scenepipeline.cpp:411-416`; `slang/shadow_megadraw.slang:1`; commit `e7a4f5b6` | High |
| backlog.md | 3.6, line 138 | Shadow distance culling reads `visibility.drawDistanceCamera`. | Neither that member nor `drawDistanceCamera` exists. Draw distance itself is currently configured, but the cited shadow-camera mechanism was removed with G1. | `src/apps/engine/optionsparser.cpp:127,239`; `include/reone/graphics/options.h:200`; `git log -S 'visibility.drawDistanceCamera'` → `db668c2f` | High |
| backlog.md | 4.6, line 275 | Retro exposes only final Output, while PBR exposes intermediate G-buffer targets. | Both raster modes use the same non-primary-ray target enumeration: six G-buffer colours, depth, and Output. | `src/libs/graphics/vulkan/scenepipeline.cpp:969-985`; `src/libs/scene/render/pipeline/vulkan.cpp:130-141` | High |
| backlog.md | 4.9, line 278 | Slang is build-time `shaderpack`/SPIR-V work and engine-runtime compilation is pending. | `src/apps/shaderpack` was deleted; the linked Slang compiler hashes all `.slang` inputs, caches modules, and supports a runtime recompilation command. | `CMakeLists.txt:236-277`; `src/libs/graphics/vulkan/shadercompiler.cpp:153-227`; `src/apps/engine/engine.cpp:337-346`; commit `a6dc6ed1` | High |
| phase-f.md | State G1, line 97 | Raster modes “run an empty plan and present a cleared scene.” | That described the intermediate G1 commit, not the current tree. Raster plans now record Shadow, Geometry, Retro/PBR resolve, and Blended. | `src/libs/scene/render/pipeline/vulkan.cpp:130-141`; `src/libs/graphics/vulkan/scenepipeline.cpp:808-832` | High |
| cleanup-plan.md | state table, lines 290-291 | F-geo is “next — F2 done … F3 next.” | The F-geo successors are present: merged G-buffer geometry, retro/PBR resolves, and real-geometry shadows. | `src/libs/graphics/vulkan/scenepipeline.cpp:411-455,462-547,640-686`; commits `ef6c5850`, `802ec6c8`, `e7a4f5b6` | High |
| renderer-registration-plan.md | “What happens today”, lines 65-121 | `SceneGraph::renderScene` fills `RenderRegistry`; registry passes and `drawScene` are the live seam. | `RenderRegistry`, its header, and the cited source files are absent. `SceneGraph::render` resets and fills `GpuScene` directly via `collectInto`. | `src/libs/scene/graph.cpp:492-618,684-713`; commit `8d37449d` | High |
| vulkan-remaining-plan.md | 1, lines 6-43 | `VulkanRenderPipeline::targets()` returns an empty vector and the viewer has no Vulkan implementation. | It forwards a populated list to the executor; the executor makes an ImGui descriptor and renders a preview image. | `src/libs/scene/render/pipeline/vulkan.cpp:144-163`; `src/libs/graphics/vulkan/scenepipeline.cpp:988-1084,1289-1295`; commit `b991a197` | High |
| vulkan-remaining-plan.md | 2, lines 65-85 | Grass is gated only at area-load time and needs a reload. | `--grassdensity` writes `GraphicsOptions::grassDensity`; admission supplies its live value to the merge upload each frame. | `src/apps/engine/optionsparser.cpp:67,186`; `src/libs/scene/render/admission.cpp:486-505`; commit `1b6559aa` | High |
| vulkan-remaining-plan.md | 5, lines 152-188 | Retro is OpenGL-only, forward-rendered, has no Vulkan counterpart, and produces no dump targets. | Vulkan chooses `RetroResolve` when `--pbr 0`; retro is G-buffer-based and its targets are published by the common raster enumeration. OpenGL and `--backend` are gone. | `src/libs/scene/graph.cpp:495-506`; `src/libs/scene/render/pipeline/vulkan.cpp:130-141`; `src/libs/graphics/vulkan/scenepipeline.cpp:640-686,969-985`; commit `df1aa375` | High |
| vulkan-remaining-plan.md | 6 and working notes, lines 194-214 | Renderer registration remains to do; building `engine` repacks `shaderpack.erf`. | Registration was replaced by direct `GpuScene` collection, and runtime Slang compilation replaces shaderpack repacking. | `src/libs/scene/graph.cpp:585-601`; `src/libs/graphics/vulkan/shadercompiler.cpp:119-140,217-250`; commits `8d37449d`, `a6dc6ed1` | High |
| vulkan-opengl-remaining-difference.md | title and lines 10-150 | The document describes a current OpenGL-versus-Vulkan comparison, GLSL counterparts, and an OpenGL shaderpack entry list. | The comparison target and GL sources are gone. Remaining Vulkan-only claims must be restated and independently validated; they are no longer parity facts. | `git show --stat df1aa375`; `CMakeLists.txt:317-382`; `src/libs/graphics/vulkan/shadercompiler.cpp:26-28` | High |
| vulkan-rt-backend.md | lines 19-30, 421-425 | Hybrid primary visibility is the current architecture: raster creates a G-buffer in every mode and the tracer does not trace camera rays. | It remains a decision/plan, not current code. In primary-ray mode `init()` allocates only output and returns; `render()` invokes `callbacks.renderPrimary()` and returns before raster passes. | `src/libs/graphics/vulkan/scenepipeline.cpp:171-187,770-806`; `doc/phase-f.md:1241-1258` | High |
| vulkan-rt-backend.md | status, lines 40-46 | `RenderRegistry` removal, Vulkan containment, and raster consuming `GpuScene` are in flight. | The registry removal and raster `GpuScene` consumers landed. This is an obsolete status snapshot. | `src/libs/scene/graph.cpp:585-601`; `src/libs/graphics/vulkan/scenepipeline.cpp:411-455,462-547`; commits `8d37449d`, `e7a4f5b6` | High |
| vulkan-rt-backend.md | 1/2.1/2.3, lines 58-125 | Fourteen Vulkan units, backend selection, GL services, and two implementations of the renderer seams are live architecture. | The tree is Vulkan-only; legacy backend-selection and GL-service claims do not describe the current executable architecture. | `CMakeLists.txt:317-382`; `src/libs/scene/graph.cpp:495-506`; commit `df1aa375` | High |
| vulkan-rt-backend.md | 10.2, lines 377-383 | BLAS is per mesh with per-node deforming BLAS refits, and TLAS is built from flattened instances. | The settled/current strategy is one scene BLAS rebuilt from merged geometry; no `refit` or `MODE_UPDATE` source occurrence remains. | `doc/backlog.md:133`; `rg 'refit|MODE_UPDATE' src include` (no matches); commit `9b98c37c` | High |
| vulkan-rt-backend.md | 16.3, lines 767-769 | Named builds skip `compile_spirv`; inspect stale `.spv` modules. | `compile_spirv` is absent, and runtime Slang source hashing/cache compilation replaces generated scene `.spv` modules. | `CMakeLists.txt:236-277`; `src/libs/graphics/vulkan/shadercompiler.cpp:153-227`; commit `a6dc6ed1` | High |
| cleanup-plan.md | hazards, lines 1112-1123 | `skin.slang` independently declares scene structs and `backend` defaults to `gl`. | The schema is centralized in `slang/lib/scene_schema.slang`; mode parsing accepts raster/path-tracing, not a backend selector. | `slang/lib/scene_schema.slang:1`; `src/libs/scene/graph.cpp:495-505`; commit `a6dc6ed1` | High |
| renderer-redesign-plan.md | S1 lines 213-224 | The schema duplication and shaderpack entry inventory are future work. | S1 itself above this passage records it as landed; the current compiler reflects the schema at startup and the shaderpack app is deleted. The retained future-tense duplicate text is stale. | `src/libs/graphics/vulkan/shadercompiler.cpp:269-330`; `CMakeLists.txt:236-277`; commit `a6dc6ed1` | High |
| phase-f.md | G3 design text, lines 258-339 | Admission is still forked through `RayQueryPipeline::prepareRaster` and raster/path tracing do not share the scene description. | The named method no longer exists. `GpuSceneAdmission` owns one upload and `VulkanRenderPipeline` feeds the renderer; the remaining ray-query code consumes the admission result. | `src/libs/scene/render/admission.cpp:472-539`; `src/libs/scene/render/pipeline/rayquery.cpp:56-115`; commit `cc9a36ac` | High |
| vulkan-opengl-remaining-difference.md | lines 48-50 and 132-144 | `--slangshaders` and `shaderpack` entry selection are relevant live controls. | Neither flag exists; the runtime compiler has a fixed module list. `retroAABBFragment` may still be source text, but “shaderpack entry list” is obsolete terminology/mechanism. | `src/libs/graphics/vulkan/shadercompiler.cpp:24-28`; commit `8ba1aede` | High |

## Already done but still listed as pending

| Doc | Item | Evidence it landed (commit or file:line) | Confidence |
|---|---|---|---|
| backlog.md | 3.3 Deforming shadow casters | `shadow_megadraw` is the pipeline module selected for both directional and point shadows; its source explicitly contrasts itself with old `shadow.slang`. | `src/libs/graphics/vulkan/scenepipeline.cpp:411-416`; `slang/shadow_megadraw.slang:1`; `e7a4f5b6` | High |
| backlog.md | 4.6 Retro target exposure | Common raster enumeration returns all G-buffer targets and depth, independent of Retro/PBR resolve selection. | `src/libs/graphics/vulkan/scenepipeline.cpp:969-985`; `802ec6c8` | High |
| backlog.md | 4.9 Runtime Slang compiler | In-process `SlangShaderCompiler`, cache, reflection guard, and `recompileshaders` command are present; shaderpack app was removed. | `src/libs/graphics/vulkan/shadercompiler.cpp:119-250,269-330`; `src/apps/engine/engine.cpp:337-346`; `a6dc6ed1` | High |
| cleanup-plan.md | F-geo (listed next) | Merged geometry now drives geometry and shadow rendering, and both deferred resolves are active. | `src/libs/graphics/vulkan/scenepipeline.cpp:411-455,462-547,640-686`; `ef6c5850`, `802ec6c8`, `e7a4f5b6` | High |
| vulkan-remaining-plan.md | 1 Render-target viewer | `targets()` forwards targets, while preview allocates an image, registers its ImGui texture, and records a preview pass. | `src/libs/scene/render/pipeline/vulkan.cpp:144-163`; `src/libs/graphics/vulkan/scenepipeline.cpp:988-1084`; `b991a197` | High |
| vulkan-remaining-plan.md | 2 Live grass setting | The CLI option is live and admission updates the merge input each frame. | `src/apps/engine/optionsparser.cpp:67,186`; `src/libs/scene/render/admission.cpp:486-505`; `1b6559aa` | High |
| vulkan-remaining-plan.md | 5 Vulkan retro pipeline | Raster mode dispatch selects `RenderMode::Retro` and the Vulkan plan selects `RetroResolve`. | `src/libs/scene/graph.cpp:495-506`; `src/libs/scene/render/pipeline/vulkan.cpp:130-141`; `802ec6c8` | High |
| vulkan-remaining-plan.md | 6 Registration rework | Direct scene collection replaced the registry. | `src/libs/scene/graph.cpp:585-601,684-713`; `8d37449d` | High |

## Cross-document contradictions

| Topic | Doc A says | Doc B says | Which the code supports | Evidence |
|---|---|---|---|---|
| Live scene hand-off | `renderer-registration-plan.md:65-121` says `RenderRegistry` is the current hand-off. | `cleanup-plan.md:288` and `backlog.md:565-566` say it was deleted and `GpuScene` took over. | Cleanup/backlog. | `src/libs/scene/graph.cpp:585-601,684-713`; commit `8d37449d` |
| Hybrid primary visibility | `vulkan-rt-backend.md:19-23,421-425` says raster currently owns it in every mode. | `phase-f.md:1241-1258` says V1 remains future work and tracer still traces camera rays. | Phase F. | `src/libs/graphics/vulkan/scenepipeline.cpp:171-187,770-806` |
| Vulkan retro status | `vulkan-remaining-plan.md:152-188` says Retro is OpenGL-only and forward. | `backlog.md:285` says that claim is closed; `phase-f.md:101` says retro shades the G-buffer. | Backlog/Phase F. | `src/libs/scene/render/pipeline/vulkan.cpp:130-141`; `src/libs/graphics/vulkan/scenepipeline.cpp:640-686` |
| Render-target viewer | `vulkan-remaining-plan.md:6-43` says Vulkan targets are empty and lack a viewer. | `backlog.md:276` says this was wrong/fixed. | Backlog. | `src/libs/scene/render/pipeline/vulkan.cpp:144-163`; `src/libs/graphics/vulkan/scenepipeline.cpp:988-1084` |
| Transparent ordering | `phase-f.md:837-924` calls for a sorted transparency remap. | `retro-rendering-differences.md:64` records that G8's sort requirement conflicts with the deliberate submission-order decision. | Current code uses submission order; it does not resolve the policy contradiction. | `src/libs/graphics/vulkan/scenepipeline.cpp:626-631` |
| G1 status | `phase-f.md:97` retains the intermediate empty-plan result as G1 state. | The same document's later G2/G5/G7 sections describe raster passes that necessarily replaced it. | The later G2/G5/G7 state. | `src/libs/scene/render/pipeline/vulkan.cpp:130-141`; `src/libs/graphics/vulkan/scenepipeline.cpp:411-455,462-547` |

## Verified still current

- `phase-f.md`'s V0 premise that runtime sky baking remains live is current:
  `RayQueryPipeline::render` gathers a selected sky room and calls
  `VulkanRayQuery::bakeSkyRoom` (`src/libs/scene/render/pipeline/rayquery.cpp:56-115`).
  Consequently V5's removal acceptance is not yet satisfied: `slang/sky.slang`
  still exists and the runtime bake symbol remains declared in
  `include/reone/graphics/vulkan/rayquery.h:36`.
- Phase F V1 is correctly pending, not silently landed: path-tracing mode
  allocates only an output target and returns before G-buffer allocation and
  raster-pass execution (`src/libs/graphics/vulkan/scenepipeline.cpp:171-187,770-806`).
- The current admission guard-rail is real. `--admissionshadow` is parsed and
  compares incremental against full-scene upload hashes every frame
  (`src/apps/engine/optionsparser.cpp:70,189`; `src/libs/scene/render/admission.cpp:493-529`).
- The current tree has all three render modes, not `RTDebug`:
  `SceneGraph::render` maps only raster/PBR/Retro and path-tracing to the
  pipeline factory (`src/libs/scene/graph.cpp:495-506`; `include/reone/scene/render/pipeline.h:57-62`).
- The recent retro audit's correction of backlog 3.6 and 3.3 is accurate
  (`doc/retro-rendering-differences.md:110-118`); its cited replacement
  mechanisms match the tree above.
- The diagnostic claim that grass density is a GPU/merge input is current;
  admission assigns it before hashing (`src/libs/scene/render/admission.cpp:486-490`).
- The current shared-schema claim in `renderer-redesign-plan.md` S1 is accurate:
  runtime reflection checks field offsets and strides (`src/libs/graphics/vulkan/shadercompiler.cpp:269-330`).
- `toolkit` remains a declared CMake target and now explicitly links
  `imgui::imgui` (`CMakeLists.txt:26,344-345`; `src/apps/toolkit/CMakeLists.txt:75-86`).
  History records the intended link-failure fix as `acc005b0`.

## Could not determine

- **`backlog.md` 4.1 — `toolkit.exe` link failure.** Source and `acc005b0` support
  the document's “done” statement, but a clean default build was deliberately
  not run for this read-only audit. A Release build log is required to establish
  whether a current linker failure still exists.
- **Numerical capture/performance claims** throughout `phase-f.md`, `backlog.md`,
  `cleanup-plan.md`, `vulkan-rt-backend.md`, and
  `vulkan-opengl-remaining-difference.md` (frame differences, milliseconds,
  coverage, and image hashes) cannot be validated from source. The cited
  symbols/flags usually still exist, but confirmation requires the documented
  headless capture or Tracy procedure.
- **Retail/reference-engine assertions** in `phase-f.md` and
  `retro-rendering-differences.md` (xoreos/KotOR.js/KVP semantics) were not
  treated as current-tree facts. The cited external checkout/revision and a
  focused source comparison would settle each.
- **The claimed absence of an offline-sky asset consumer** is strongly suggested
  by the active runtime-bake call, but proving there is no alternate resource
  consumer requires an asset-install/override scan in addition to the source
  scan. The factual implementation result is therefore limited to: runtime
  baking is definitely still the active path.
