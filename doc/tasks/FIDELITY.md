# Retro rendering fidelity audit

Re-audited 2026-08-05 against the active Vulkan raster code, the retired OpenGL
tree at `df1aa375^`, and the reference engines. This is deliberately stricter
than the first inventory: a source difference is **not** called a regression
unless it has an observed current failure or an active plan identifies it as
unfinished.

Rows 1–16 are that audit. **Rows 17–37 were added the same day** by a second
sweep over areas the first did not reach: lighting and shadow policy, texture
sampling, material animation, and what gets drawn at all. That sweep also
produced the corrections and the confirmed-match list below — several of its
most useful results are negative, and the matches are recorded so they are not
re-opened.

The S1 Slang-in-engine toolchain change is outside this list: its before/after
raster capture was byte-identical.

## Reported symptom → rows to check first

Entry point for someone holding a bug report rather than a row number. These are
**candidate** linkages ordered by likelihood, not diagnoses: confirm against the
row's own evidence before acting, and add a symptom here when a report is traced
to a row.

| Reported symptom | Check in this order | Why these |
|---|---|---|
| Force field renders wrong | #2, #21, #20, #13 | Continuous-transparency classification decides whether it blends at all; the authored `transparencyHint` is the signal that classification is missing; the opaque range is untested and a force field is exactly the alpha-plus-selfillum case that bypasses the transparency check; animated-UV jitter drives its shimmer. |
| Manaan water renders wrong | #2, #21 | `wateralpha` reaches the material but the forward water branch is bypassed when `TransparentModel` classifies as Cutout, so water is the direct diagnostic for the row 2 defect; #21 is the fix path. |
| Dantooine interiors too dark | #16, then #23, #24, #25 | Broad shadow-caster scope can darken a lightmapped receiver through dynamic occlusion; after that, light selection, dynamic light on lightmapped geometry, and ignored area shadow flags all change the same pixels. |
| Particles missing or wrong | #8–#14, #22 | The emitter field omissions as a set. Start with the census in #12 to find an authored example of the specific behaviour before assuming which field is at fault. |
| Too many things cast shadows | #16, then #25 | #16 is the caster scope itself and names the authored per-node flag that would filter it; #25 is the area-level enable that is parsed and ignored. |
| Objects pop instead of fading | #18, then #33 | Per-node alpha never renders; destroy fade is a second consumer blocked behind the same defect. |
| Geometry visible that should not be | #17, then #1 | Room visibility is gone entirely, so backdrop rooms and rooms behind doorways are drawn; the sky chain in #1 overlaps for backdrop geometry specifically. |

## Status meanings

| Status | Meaning |
|---|---|
| **Confirmed current defect** | Reproduced or explicitly recorded as wrong in the current tree. |
| **Confirmed unfinished design** | Current code and the active plan disagree on a required design decision. |
| **Confirmed source omission** | The asset field is parsed, but no current scene/shader consumer exists for it. |
| **Planned gap** | Missing by an intentional, active plan decision; do not file it again as an unowned regression. |
| **Reference-informed open** | The reference supports the difference, but no current visual failure or final design decision has been demonstrated. |
| **Source divergence, unproven** | Code differs, but its visible effect has not been shown. |
| **Source match, visually unverified** | Relevant shader/dataflow code matches, but no controlled capture has verified the whole effect. |
| **Intentional / accepted** | A documented correction or chosen trade-off. |
| **Withdrawn / non-fidelity** | The earlier claim was unfalsifiable, obsolete, or outside player rendering. |
| **Ruled out** | The reviewed implementation does not support carrying this as an active difference. |

**Compound statuses.** A row may carry two findings whose evidence genuinely
differs in standing, written `A plus B` with each clause naming the part of the
result it covers. Both clauses must be terms from the table above — a compound
is a pairing of defined statuses, never a new one, and it must not be used to
avoid choosing. Row 23 is the only row that currently qualifies; row 16 states a
single status and puts its open sub-question in the action column instead.

## Per-case audit and plan linkage

| # | Area | Re-audited result | Status | Current plan owner | Required proof / next action |
|---:|---|---|---|---|---|
| 1 | Sky | Raster currently suppresses a selected sky room and writes black where no material ID exists. That is real, but Phase F explicitly says raster is black until the offline sky asset chain lands. | **Planned gap** | [Phase F](phase-f.md) — sky after G9; [backlog](backlog.md) 1.14 | Do not restore GL sky shells. Implement the curated offline sky chain, then use the six-colour orientation fixture required by 1.14. |
| 2 | Continuous transparency classification | `TransparentModel` currently goes to **Cutout** because its old predicate frequently only meant “the texture has an alpha channel”; current code and `e81fe2d8` document that moving all of it to blended breaks foliage. That also bypasses the forward water branch, making Manaan-style `wateralpha` material failures a direct diagnostic. Only procedural particles currently reach LitBlended. | **Confirmed current defect** | [Phase F](phase-f.md) **G8** | Define the narrower blended signal; capture a pane/smoke/foliage/water fixture in Retro and PBR. The pass exists, but this classification is incomplete. |
| 3 | Transparent ordering | The current merged draw is in submission order. Phase F G8 requires a raster-only sorted remap; `eed85ac5` instead calls the lack of sorting deliberate, based on kvp-main replay. These are contradictory plan/code statements, not evidence that a particular retail ordering is already wrong. | **Confirmed unfinished design** | [Phase F](phase-f.md) **G8** | Resolve the contradiction in G8, then prove the selected rule with the deliberately interleaved transparent-pair fixture. |
| 4 | Additive textures without alpha | xoreos uses `SRC_COLOR, ONE` for the no-alpha additive case, while the current alpha-zero output yields `ONE, ONE`. Phase F’s reference survey retains this as an open correction. It is narrower than “all additive is wrong.” | **Reference-informed open** | [Phase F](phase-f.md) G8 reference findings; no dedicated substage | Identify affected texture/material cases and compare a localized additive fixture before choosing a material value or exceptional path. |
| 5 | Face culling | Mesh data asks for back-face culling, while current G-buffer and blended pipeline keys use `FaceCullMode::None` (`scenepipeline.cpp:516`, `:599`). Narrower than first written: the **shadow pass does cull**, front-face for the opaque range and none for the gated range (`scenepipeline.cpp:451-455`). So the tree is internally inconsistent about culling rather than uniformly two-sided, and a shadow can be cast by a face the G-buffer also draws. No capture shows a visible regression, and merged/decal geometry may need two-sided handling. | **Source divergence, unproven** | No named owner | Add a closed-shell, back-facing card, and decal fixture. Compare shadow and G-buffer coverage of the same shell, since they currently disagree. Assign an owner only if culling changes the expected visible pixels. |
| 6 | Lens flares | Flares are collected as billboards, but `LensFlare` is excluded by the admission-visible category mask, so they do not reach the current raster output. | **Planned gap** | [Phase F](phase-f.md) **G9** | G9 owns unfiltering flares and their output-stage ordering. Verify with an opaque-prop/flare fixture once that stage exists. |
| 7 | Bloom, FXAA/sharpen, and final output order | These legacy effects are absent from the current raster tail. G9 explicitly boxes them into one shared output stage and specifies the capture and cost acceptance tests. | **Planned gap** | [Phase F](phase-f.md) **G9** | Execute G9’s edge-heavy all-mode fixture; include bloom and flares, not only AA. |
| 8 | Emitter tint | The `tinted` emitter flag is parsed but never changes raster particle colour. KotOR.js applies an ambient-derived tint; fresh scans find 714 K1 and 592 K2 tinted emitters. | **Confirmed source omission** | [Phase F](phase-f.md) fog/march census; raster owner missing | Preserve tint in the particle record. Its PT bake owner exists, but Retro/PBR need an explicit owner and a tinted-emitter fixture. |
| 9 | Particle rotation and authored motion-blur length | `particleRot` and `blurLength` controller IDs are known, but `EmitterSceneNode` reads neither. Motion blur instead uses fixed `0.25 × 16` stretch. KotOR.js applies `particleRot × age` and the authored blur length. | **Confirmed source omission** | [renderer redesign plan](renderer-redesign-plan.md) S3 particle simulation; Phase F retro quad contract | Add rotation and stretch fields to the procedural-particle record, then capture a rotating sprite and two authored blur lengths. |
| 10 | Non-square particle lifetime sizing | Reone reads only scalar `sizeStart/Mid/End`; `sizeStartY/MidY/EndY` have no consumer, so every lifetime-sized particle remains square. KotOR.js carries a separate Y scale through its emitter shader. | **Confirmed source omission** | [renderer redesign plan](renderer-redesign-plan.md) S3 particle simulation | Add independent X/Y lifecycle size and use a deliberately non-square emitter fixture. |
| 11 | Emitter render order | `renderOrder` is parsed but never affects admission or the draw. Fresh scans find 1,904 K1 and 1,014 K2 non-zero orders; KotOR.js assigns it to the emitter mesh, and KVP preserves the retail non-opaque draw stream rather than applying a camera-distance sort. | **Confirmed source omission** | [Phase F](phase-f.md) **G8** ordering decision | Include authored `renderOrder` in the G8 ordering decision and test two overlapping emitters with distinct orders. |
| 12 | P2P Bézier, wind, bounce, and inherited motion | Bit 2 is `P2P_SEL`, meaningful only with P2P: 16 K1 and 17 K2 emitters set both. Reone implements gravity only when P2P is set without that modifier, so the selected branch has no Bézier path. Authored wind (154 K1 / 226 K2), bounce (453 / 463), and inherited-motion flags also have no current consumer. | **Confirmed source omission** | [renderer redesign plan](renderer-redesign-plan.md) **S3**; Phase F emitter census | Use an isolated P2P-SEL fixture plus wind and bounce examples; do not infer behavior from standalone bit 2. |
| 13 | Animated-UV jitter | Reone reads `uvJitter` and `uvJitterSpeed` but discards both; its retained UV state carries only scroll direction. KVP preserves both fields and KotOR.js feeds them to `UVJitter`, so the omission is in the current reader-to-material path, not merely an unused file field. | **Confirmed source omission** | [Phase F](phase-f.md) material/merge validation; no dedicated owner | Carry jitter amplitude and rate through the material record and add a scrolling-versus-jittered texture fixture before matching the exact waveform. |
| 14 | `aligned_to_world_z` particle orientation | The parser recognises this render mode, but the procedural-instance switch has no case for it and falls back to camera-facing `Normal`. Fresh scans find 20 affected emitters in each game; KotOR.js has a distinct shader mode. | **Confirmed source omission** | [renderer redesign plan](renderer-redesign-plan.md) S3 particle simulation | Add its world-axis basis to the procedural particle record and validate it with an oblique camera fixture. |
| 15 | TSL `n_forcezombie` model load | The TSL scan reaches `n_forcezombie` but Reone aborts on a node-name string without a NUL terminator in its 32-byte inspection window. KVP reads the same name table as variable-length C strings. The result is one currently skipped TSL model, but in-game use and visible impact remain unverified — the loader divergence itself is confirmed in code. | **Source divergence, unproven** | No named owner | Reproduce its in-game use, capture its absence, then make bounded name-table reading tolerant without weakening other malformed-file checks. |
| 16 | Shadow-caster scope | The merged shadow pass draws every opaque/cutout triangle. The retired OpenGL path enrolled only selected creature and placeable meshes as `ShadowCaster`; it did not put room, door, equipment, projectile, or arbitrary scenery meshes into that pass. This explains reports of scenery casting where the old renderer did not, and can darken a lightmapped receiver through dynamic occlusion. **Addendum:** the per-node MDL caster flag that would be the filter already exists and is already parsed — `TriangleMesh::shadow` (`mdlmdxreader.cpp:237,477` → `modelnode.h:80`) has no renderer reader, its only writers being the synthetic testbed. KotOR.js gates casting on exactly that flag (`OdysseyModel3D.ts:1429`) and records that the original derived creature shadows from the **skeleton**, not the render mesh (`:1231-1232`). The divergence from the retired path is confirmed and G7 chose it deliberately; what is not settled is whether Retro keeps that choice, and no plan currently owns that question. | **Intentional / accepted** (the G7 decision); the Retro contract is the open part | [Phase F](phase-f.md) G7 merged-shadow decision | Capture a Dantooine lightmap receiver with and without an intervening scenery mesh, then decide whether Retro needs the old caster filter or a separately authored scope. If it does, the authored flag is the filter — no new data is needed. |
| 17 | Room visibility: the VIS graph is gone | `Area::updateRoomVisibility` is now an unconditional "every room visible" loop (`area.cpp:1201-1216`); `88d3efae` deleted the VIS branch, and `db668c2f` had already deleted frustum culling with `VisibilityPolicy`. The `.vis` resource is still read and symmetrised into `Area::_visibility` (`area.cpp:561-576`) and **never read back**. KVP's Ghidra notes on the retail binary give the original policy: `enableVisibilityGraph` defaults to 1, `CollectActiveRooms` walks a per-room adjacency list, and a separate outdoor flag selects all-rooms — so retail had *two* policies and reone implements only the exterior one. xoreos (`kotorbase/area.cpp:708-750`) and KotOR.js (`ModuleArea.ts:804-861`) both apply adjacency, and both do it **camera-independently**, which is the property `88d3efae` gave up rather than adopted. Observed consequence already on record in that commit: every walkmesh-less backdrop room except the single largest is now permanently drawn, which is why free-camera Korriban showed sky. | **Confirmed current defect** | No named owner; interacts with row 1's sky chain | Restore a camera-independent adjacency filter, or record all-rooms as a deliberate choice. Capture an interior doorway and a Korriban-style backdrop room in third person and free camera. Note `Room::setVisible` also re-forces tenant object visibility every frame (`room.cpp:40-57`), which feeds `Area::isObjectSeen` — perception scope moved with the render scope. |
| 18 | Per-node alpha fade never renders | The alpha controller runs and `_alpha` reaches `InstanceMaterial.diffuseColor.a` (`mesh.cpp:309` → `admission.cpp:206-207`), but the only shader that reads it is the **blended** pass (`megadraw.slang:213`). The G-buffer fragment and the coverage gate read `input.color.a` instead, which `skin.slang:301` hard-codes to `1.0` for all MDL geometry. A fading mesh classifies `TransparentModel` → Cutout → punch-through, so it is drawn at full opacity in the gated G-buffer draw *and* explicitly discarded by the blended pass (`megadraw.slang:195`). The object therefore never fades; it disappears when `_alpha` reaches 0 (`mesh.cpp:206`). Every door, placeable, and appear/disappear fade is lost. | **Confirmed source omission** | [Phase F](phase-f.md) **G8** classification; shares the row 2 defect | Fix with row 2: a fading mesh must reach the blended pass. Fixture: a placeable driven through an alpha controller from 1 to 0, captured at three intermediate values. |
| 19 | Alpha-test reference value | Reone tests at **0.5** (`lib/megadraw_geometry.slang:11`, applied `:57`), in the gated G-buffer draw and in **both shadow passes** (`shadow_megadraw.slang:43`, `:79`). xoreos sets `glAlphaFunc(GL_GREATER, 0.1f)` once globally (`graphics/graphics.cpp:440-441`) and only ever disables it around the env-map pass. **`phase-f.md:1338` misquotes this as `GL_GREATER, 0`** — the authority is 0.1, so both the "conflict is a wording slip" reasoning and the defence of 0.5 at `phase-f.md:847-852` rest on a value the reference does not use. **The threshold is five times higher than the reference value**; how much coverage that removes is non-linear in the texture's alpha distribution and is not 5× anything, so it must be measured rather than reasoned about. It applies on every foliage card, fence, and grille, and the same threshold thins their shadows. | **Reference-informed open** | [Phase F](phase-f.md) **G8** coverage rule | The quoted reference value is corrected in `phase-f.md` as of 2026-08-05, in both the confirmed-correct list and the G8 passage that argued from it. Next: capture a foliage silhouette at 0.1 and 0.5 side by side and count the coverage difference. This is a preservation question, not a taste one. |
| 20 | The opaque range is untested, and reaches it too easily | `megadraw_geometry.slang:44` returns true unconditionally for the ungated range, so the opaque draw performs no alpha test at all, where retail alpha-tested everything. Worse, `MeshSceneNode::isTransparent` returns false for any node carrying an **envmap or a bumpmap** (`mesh.cpp:228-230`) and for self-illum luma ≥ 0.99 (`:231-233`), both evaluated *before* the alpha-channel check at `:234`. Such meshes render their transparent texels solid. The blended pass separately kills at `alpha <= 0.0` (`megadraw.slang:246`) where retail killed at 0.1. | **Source divergence, unproven** | [Phase F](phase-f.md) **G8** coverage rule | Scan K1/K2 for meshes with a live alpha channel plus an envmap or bumpmap to size the population, then capture one. Mechanically certain from the code; unproven only in that no capture exists yet. |
| 21 | `transparencyHint` is parsed and unread | The MDL authored transparency hint is read into `TriangleMesh::transparency` (`mdlmdxreader.cpp:475`, `modelnode.h:75`) and has no consumer anywhere in `src/libs/scene` or `src/libs/graphics`. xoreos prefers the authored hint and falls back to "the texture has an alpha channel" only when it is absent (`aurora/modelnode.cpp:500-506`). Reone classifies purely from texture alpha and TXI (`mesh.cpp:212-235`). | **Confirmed source omission** | [Phase F](phase-f.md) **G8** classification | **This is the narrower blended signal row 2 asks for, and it already exists in the file format.** Try hint-first classification with the alpha-channel test as fallback, and re-run row 2's foliage/pane/water fixture. |
| 22 | `Punch-Through` emitter blend is dropped | `mdlmdxreader.cpp:608` parses the emitter blend mode, but `emitter.cpp:325-326` tests **only** for `Lighten`, so Punch-Through emitters carry no `Material::blending` and `admission.cpp:321-323` classifies them `LitBlended`. They get continuous alpha blending with no alpha test, no depth write, and no G-buffer or shadow presence. KotOR.js maps the same case to NormalBlending with `alphaTest = 0.5` (`OdysseyEmitter3D.ts:441-445`); its `Lighten` case is the one reone already matches. Visible as hard-edged sprite cards rendering soft. | **Confirmed source omission** | [renderer redesign plan](renderer-redesign-plan.md) S3 particle simulation; [Phase F](phase-f.md) G8 | Map Punch-Through to the cutout path. Fixture: one Punch-Through and one Lighten emitter side by side. |
| 23 | Light selection policy | Reone keeps **32** lights (`types.h:38`) in a single global set chosen by camera distance (`graph.cpp:267`, `866-902`) and uploads them verbatim (`graph.cpp:542-551`); retro's resolve iterates all of them (`retro_resolve.slang:109`). **KotOR.js models an 8 global / 3 per model budget** (`LightManager.ts:24-25`, sourced there to `NumDynamicLights` in `videoquality.2da`; per-model cap enforced by `canShowLight` at `:721-744`). Retail behaviour is **not** established by that alone — neither KVP nor xoreos corroborates the per-model cap, and `videoquality.2da` is a settings table rather than a proof of the renderer's internal limit. Treat 8/3 as one reimplementation's model until KVP's disassembly or a controlled capture supports it. Three further gaps: there is **no per-model light assignment at all**, so every surface sees the same set; there is **no light frustum culling** (KotOR.js requires `isOnScreen`, reone tests only radius + a 64-unit slop, `graph.cpp:70`); and the MDL light **`priority`** is parsed (`mdlmdxreader.cpp:519`, `modelnode.h:111`) and used only to sort *sounds* (`graph.cpp:379`), where KotOR.js sorts lights by animated, radius, priority, then distance (`LightManager.ts:704-718`). | **Confirmed source omission** (priority) **plus source divergence, unproven** (budget, per-model cap, frustum) | No named owner | Decide whether Retro adopts the 8/3 budget as part of the look. Fixture: a room with more than eight authored lights, and one model inside three overlapping light volumes. |
| 24 | Dynamic light on lightmapped geometry | Reone applies every `dynamicType == 1` light to lightmapped static geometry, modulated by the albedo (`retro_resolve.slang:112`). KotOR.js's lightmap branch drops `directDiffuse` **entirely** on lightmapped surfaces and adds only the animated-light array, additively, unmodulated by albedo and at half strength (`ShaderOdysseyModel.ts:359-365`). | **Reference-informed open** | No named owner | KotOR.js annotates its own `* 0.5` as a hardwired shadow intensity, so it is doing double duty and is not a clean oracle. Capture a lightmapped interior with a moving dynamic light before adopting either rule. |
| 25 | `SunShadows` / `MoonShadows` parsed and ignored | Both are parsed (`are.cpp:356,379`), stored into `ShadowProperties` (`shadowproperties.h:26-27`) and pushed to the graph (`area.cpp:378`), which reads only `opacity` (`graph.cpp:562`). Shadow enablement comes solely from the MDL light's own flag (`graph.cpp:305`), so an area authored `SunShadows=0` still casts. Sibling of the `ShadowOpacity` case, which `2f00b5f5` settled deliberately. | **Confirmed source omission** | No named owner | Honour the area flags, or record ignoring them as deliberate the way `ShadowOpacity` was. Needs an area authored with sun shadows off. |
| 26 | No shadow or soft-shadow option | `GraphicsOptions` exposes only `shadowOpacity` and `shadowResolution`; the launcher offers resolution alone (`frame.cpp:188-198`). **Retail exposed both a `Shadows` and a `Soft Shadows` setting** — the retail ini KVP reproduces carries them (`swkotor.ini.example:93-94`), which establishes that the options existed and were user-facing. It does **not** establish what either one switched between. Separately, retro uses four-cascade CSM with a rotated 16-tap Poisson kernel (`lib/shadow.slang`) unconditionally; Phase F already records that as a modern reconstruction with no reference to check against, so "the original did not use this technique" is a reasonable inference from the era and from the skeleton-shadow note in row 16, not something these sources prove. | **Source divergence, unproven** | [Phase F](phase-f.md) G7 follow-on | The missing *options* are separable, cheap, and evidenced; decide whether Retro exposes them. The technique question needs a source that describes retail's shadow implementation, which none of the three currently does. |
| 27 | Fog distance metric | Retro's fog is per-pixel **radial**, `length(worldPos - cameraPosition)` (`retro_resolve.slang:148-153`). Fixed-function `GL_LINEAR` fog and KotOR.js's three.js fog chunks both use view-space depth, i.e. **planar**, and KotOR.js computes it per vertex. Radial fog is thicker toward the screen edges at a given depth. The rest of the chain matches: retro does implement linear fog, and the authored `SunFog*` parameters reach it through `Area::loadFog` and the per-mesh MDL fog flag. | **Source divergence, unproven** | No named owner | One-line change; capture a wide-FOV corridor with fog near/far tightened so edge thickening is visible. `MoonFog*` is parsed and unused in reone *and* in KotOR.js, so record it as a shared unowned field rather than a deviation. |
| 28 | Anisotropic filtering on by default | `TextureProvider` applies `anisotropy = 2^anisotropicFiltering` to **every** texture it loads (`textures.cpp:106-107`), and `GraphicsOptions::anisotropicFiltering` defaults to 2, i.e. **4×** (`options.h:199`). xoreos sets no anisotropy anywhere in `src/graphics/`. That is one reimplementation's choice and does not by itself establish what the retail renderer did — no retail source has been cited either way, and the retail ini has not been checked for an anisotropy or texture-quality entry. What is certain is that reone turns it on by default and nothing in the audit trail records that as a decision. | **Source divergence, unproven** | No named owner | Check the retail ini and KVP's captured sampler state for an anisotropy term first. If retail set none, a preservation default of 1× follows; either way capture a grazing-angle floor at 1× and 4×. |
| 29 | TXI sampling flags unparsed | `txireader.cpp:55-98` parses blending, wateralpha, cube, envmap, bumpmap, font, procedure and decal fields, but **not `filter` or `mipmap`**. xoreos honours `filter` — true gives linear/trilinear, false gives `GL_NEAREST` with no mips (`aurora/texture.cpp:216-226`) — and forces it off for cursors. Reone cannot express an authored unfiltered texture. | **Confirmed source omission** | No named owner | Parse both flags and route them into `Texture::Properties`. Scan the K1/K2 TXI set first to size how many authored textures set them. |
| 30 | Block-compressed textures with no shipped mip chain get none | `resources.cpp:339-341` sets `generateMips = authoredMips == 0 && !compressed && fullMipCount > 1`, so a BC texture shipping a single level keeps `mipCount = 1` and never minifies. xoreos generates a chain in that case (`aurora/texture.cpp:228-242`). Shows as minification aliasing. | **Source divergence, unproven** | No named owner | Bounded — most TPCs ship chains. Count the exceptions before deciding whether to add a compressed-mip generation path. |
| 31 | Object draw distance is dead code | `drawDistance` is plumbed from the CLI, the editor slider and the launcher (range 32–128) into `ModelSceneNode::setDrawDistance` at three call sites, and `_drawDistance` has **no reader anywhere** — orphaned by `db668c2f`. The user-facing slider therefore does nothing. Backlog **3.6 is stale** for the same reason: the `drawDistanceCamera` symbol it cites no longer exists. | **Confirmed source omission** | No named owner | Either restore distance culling or remove the option and its two sliders. Do not leave a control that silently does nothing. |
| 32 | Grass distance policy | Reone culls grass faces beyond a fixed `kMaxClusterDistance = 32` measured from the **camera** (`gpuscene.cpp:704-736`), with a 4-unit size ramp in the merge kernel (`skin.slang:187`). KotOR.js fades grass by alpha from **25 to 100** units, measured from the **player**, and disables the fade entirely during dialogue (`ShaderGrass.ts:29-31,164`; `ModuleRoom.ts:195-197,444-445`). Three deltas; the visible one is the third — a dialogue or animated camera placed away from the player strips grass around the player. | **Source divergence, unproven** | [Phase F](phase-f.md) R3 grass owner | Capture a dialogue camera on a grassy exterior. The camera-versus-player term is the cheap half and is independently testable. |
| 33 | Destroy fade ignored | `DestroyObject` reads `bNoFade` and `fDelayUntilFade` and then discards both, destroying immediately (`script/routine/impl/main.cpp:2434-2447`); `SetIsDestroyable` throws `RoutineNotImplementedException`. KotOR.js fades a destroyed object's opacity to zero over 10 s after the authored delay (`ModuleObject.ts:516-528, 3691-3725`). Corpses and destroyed placeables pop instead of fading. Depends on row 18: even if honoured, partial alpha does not currently render. | **Confirmed source omission** | No named owner; blocked behind row 18 | Fix row 18 first, then honour the two script parameters. Fixture: kill a creature and capture three frames across the fade. |
| 34 | Meshes with a null diffuse texture are dropped | `shouldRender` requires a non-empty `diffuseMap` (`mesh.cpp:209`) and `collectInto` additionally requires the texture to have resolved (`:263-271`); the reader blanks the name when texture1 is `"null"` (`mdlmdxreader.cpp:461-463`). Neither reference makes texture presence a visibility condition — xoreos gates on `_render` and geometry counts only (`model_kotor.cpp:842-846`), KotOR.js on `flagRender` only. So a `render=1` mesh with no diffuse — lightmap-only or untextured surfaces — **would be rendered by both reviewed reimplementations and is not rendered here**. Whether retail drew it, and whether any such mesh is reachable in a shipped module, are both open. | **Source divergence, unproven** | No named owner | Scan K1/K2 MDLs for `render == 1 && texture1 == "null"` to size the population. Do not describe this as a player-visible regression until that scan finds an instance and a capture shows it missing. |
| 35 | Self-illumination operator | Retro multiplies: self-illum is summed with ambient and direct, clamped, then multiplied by the diffuse texture (`retro_resolve.slang:134-138`). Phase F's reference correction #6 says vanilla *adds* emission on top of the texture, citing KVP (`MaterialSystem.cpp:196`). But the references disagree — KotOR.js multiplies with a 0.25 floor (`ShaderOdysseyModel.ts:383`), and fixed-function `GL_MODULATE` computes `emission + ambient + Σdiffuse`, clamps, then multiplies the texture, which is exactly reone's expression. KVP's line sits inside its own legacy→PBR conversion and needs compensating albedo heuristics on the next lines. | **Reference-informed open** | [Phase F](phase-f.md) reference correction #6 | Do not treat correction #6 as settled either way. A `_lit`/`lsi_` pattern texture on a dark surface separates the two operators immediately. |
| 36 | Odyssey light falloff and range cull | Retro's range cull compares a linear distance against `radius²` (`retro_resolve.slang:116`) and its falloff is `radius²/(radius+d)²` rather than inverse-square. Both are dimensionally wrong and both are **deliberate**: backlog 1.9 records them as faithful ports of Odyssey behaviour, and `085c5893` corrected them in the tracer only. Decided 2026-08-05: **Retro keeps them; PBR does not.** `pbr_resolve.slang:83-85` and `:235` still carry the Odyssey versions with a comment saying they are kept as the GL shader had them. | **Intentional / accepted** for Retro; **confirmed unfinished design** for PBR | [Phase F](phase-f.md) **G6c** | Do not "fix" the retro light loop — a dimensional-analysis bug report there is expected and not actionable. G6c retires the Odyssey cull and falloff from PBR only, and any shared-lighting refactor must keep two functions rather than converging them. |
| 37 | MDL vertex colours | Ruled out as a difference. `MergedVertex.color` is a literal white constant for all MDL geometry (`skin.slang:301`), `SceneObject` carries no colour attribute offset, and the loader declares `offMdxVertexColors` (`mdlmdxreader.cpp:225`) and never reads it. Neither reference reads an MDX colour array either: xoreos's KotOR loader has none, and KotOR.js uses vertex colours only for walkmesh and path debug geometry. | **Ruled out** | — | No action. Note only that `diffuse *= input.color` (`megadraw.slang:138`) is a guaranteed no-op for mesh geometry whose name implies a source that cannot exist. |

## Corrections to sources this file cites

Found while adding rows 17–37. Each is a claim elsewhere in the docs that the
current code or the reference engines do not support.

- **`phase-f.md:1338` misquotes the retail alpha test** as
  `glAlphaFunc(GL_GREATER, 0)`. xoreos uses **0.1**
  (`graphics/graphics.cpp:440`). The G8 passage at `phase-f.md:847-852` that
  dismisses the threshold conflict as a wording slip is therefore reasoning
  from a value the authority does not use. See row 19.
- **Backlog 3.6 is stale.** It cites `gpuscene.cpp:66-71` and
  `visibility.drawDistanceCamera`; both were removed in `db668c2f`. The real
  residue is row 31 — draw distance is not merely mis-scoped in shadow passes,
  it is not implemented anywhere.
- **Backlog 3.3 is stale.** "`shadow.slang` has only a rigid vertex stage; skinned
  meshes never cast shadows" predates G7. The shadow pass is now
  `shadow_megadraw` over merged geometry, which is already skinned by the merge
  compute, so skinned meshes do cast. The `shadow` module itself has no
  pipeline consumer left.
- **Row 5 was broader than the code.** The shadow pass does cull; only the
  G-buffer and blended passes do not.

## Confirmed matches — do not re-open these

Negative results carry weight here, because each one closes a plausible line of
enquiry. Verified against the references during this pass:

- **Lightmap composition.** `lightmap × diffuse` matches xoreos's
  `addPass(TEXTURE_LIGHTMAP, BLEND_MULTIPLY)` (`model_kotor.cpp:670-673`).
- **Ambient policy.** Ambient is suppressed on lightmapped and static geometry,
  and DynAmbientColor applies to dynamic objects only — consistent with the
  reference model.
- **Fog exists and is wired.** Retro implements `GL_LINEAR` fog and the authored
  per-area `SunFog*` parameters reach it through `Area::loadFog` and the MDL
  per-mesh fog flag. Only the distance metric differs (row 27).
- **UV scroll, bump-frame cycling, and self-illum animation** all reach the
  merged material record and the retro resolve. One caveat on bump cycling: it
  only applies when the bump map is grayscale, since a non-grayscale one is
  routed to the NormalMap slot and never cycles.
- **MDL vertex colours** — row 37, ruled out.
- **TXI `clamp`** is not parsed by reone and is parsed-then-ignored by xoreos;
  wrap is `REPEAT` in both. Not a difference.
- **No model LOD anywhere.** The premise that Odyssey MDL carries LOD or
  detail-level data is not supported: reone's and xoreos's independent header
  reads agree and neither has such a field, and xoreos implements LOD only for
  Dragon Age, NWN2 and The Witcher. The retail "Grass" ini entry is a boolean,
  which reone already has.
- **No camera-proximity object fading** in retail, reone, or any reference.
  MDL alpha controllers do run (row 18 is about rendering the result, not
  computing it).

## Additional reference-engine coverage

The original review already used the three sources with complementary
authority: **xoreos** for fixed-function GL semantics, **KotOR.js** for
Odyssey-side policy and controllers, and **KVP** for the retail draw stream.
This extension also inspected two additional Unity reimplementations:

| Project | What the audit found | Weight in this inventory |
|---|---|---|
| [NorthernLights](https://github.com/lachjames/NorthernLights) | Parses Odyssey mesh, emitter, flare, blend, lightmap, self-illum, and render-order fields, but its source explicitly leaves Punch-Through and Lighten unsupported and delegates most rendering to stock Unity materials. | Format corroboration only; not used to assert retail rendering behavior. |
| [KotOR-Unity](https://github.com/rwc4301/KotOR-Unity) | Loads meshes into Unity’s stock lightmapped vertex-lit material and parses light/flare and texture metadata, without an Odyssey render-state implementation. | Format corroboration only; not used as a fidelity oracle. |

Neither Unity project supplies a further independent visual authority. They do,
however, confirm that the emitter/mesh fields behind #8–#14 are real Odyssey
data rather than reone-only inventions. Exact behavior for those rows rests on
the actively implemented KotOR.js path and must still be capture-verified.

The K1 emitter scan completed with 2,847 models / 8,870 emitters. The TSL scan
read 3,001 emitters and skipped only `n_forcezombie`, the separate loader case
in #15; its counts are therefore conservative by one model.

## Current work order

1. **G8 first, and it is now larger than it was:** settle the classification and
   ordering contract (#2–#3), starting from **#21** — the authored
   `transparencyHint` is the narrower blended signal #2 has been looking for and
   it is already parsed. **#18** (alpha fade never renders) is the same defect
   seen from the other side and should be fixed in the same change. Then the
   coverage rule itself: **#19** the reference threshold, **#20** the untested
   opaque range. Then the narrower no-alpha additive case (#4), authored emitter
   order (#11) and Punch-Through emitters (#22).
2. **G9 and then sky:** restore the shared output stage (#6–#7), followed by
   the curated sky chain (#1), in the order Phase F already prescribes. **#17**
   overlaps the sky chain: every backdrop room but one is currently drawn as
   world geometry, so decide room visibility before judging sky output.
3. **Give S3 an emitter-fidelity acceptance set:** #8–#14 and #22 must survive
   the particle-simulation move. Start with tint, rotation/size, and alignment;
   use the census before expanding P2P/wind/bounce behavior.
4. **#36 is decided, not open.** Retro keeps Odyssey's light falloff and range
   cull; G6c removes them from PBR only. Reject bug reports against the retro
   light loop that argue from dimensional analysis.
5. **Treat #5, #23, #27, #28, #30, #32 and #34 as evidence-gathering, not
   fixes.** Each needs a targeted fixture or a content scan before anything
   changes. #28 (anisotropy) and #27 (fog metric) are one-line changes once the
   capture exists; #23 (light budget) is a look decision, not a bug.
6. **Cheap omissions with no owner, worth a single sweep:** #25 area shadow
   flags, #29 TXI sampling flags, #31 the dead draw-distance slider, #33 destroy
   fade (blocked behind #18). None is urgent; all are small and each currently
   presents a control or an authored field that silently does nothing.
7. **Triage #15 separately:** establish whether `n_forcezombie` is reachable
   in TSL before treating its tolerant string read as a visible-render fix.
8. **Validate #16 on the reported scene:** do not retain or revert its broad
   caster policy on principle; use the Dantooine comparison to set the Retro
   contract. If a filter is wanted, the authored per-node flag already exists.

## Fixture set

- G8 triple-alpha fixture: punchcard, alpha-blended pane, additive glow;
  plus the intentionally depth-interleaved transparent pair.
- G9 fixture: opaque prop, lens flare, bloom source, and edge-heavy geometry;
  all three modes and each AA method.
- Material fixture: a no-alpha additive texture.
- Culling fixture: closed shell, single back-facing card, and decal — captured
  in both the G-buffer and the shadow map, which currently disagree (#5).
- Emitter fixture suite: tinted and rotating sprites, non-square lifetime
  sizing, two blur lengths, two render orders, world-Z alignment, one
  Punch-Through beside one Lighten (#22), and real P2P-Bézier/bounce/wind
  examples selected by the census.
- Coverage fixture: one foliage card captured at alpha-test 0.1 and 0.5 (#19),
  and one mesh carrying both a live alpha channel and an envmap or bumpmap
  (#20).
- Fade fixture: a placeable driven through an alpha controller from 1 to 0,
  captured at three intermediate values (#18), reused for destroy fade (#33).
- Room-visibility fixture: an interior doorway and a Korriban-style backdrop
  room, each in third person and free camera (#17).
- Lighting fixture: a room with more than eight authored lights, and one model
  inside three overlapping light volumes (#23); a lightmapped interior crossed
  by a moving dynamic light (#24); a `_lit`/`lsi_` pattern texture on a dark
  surface (#35).
- Sampling fixture: a grazing-angle floor at 1× and 4× anisotropy (#28), and a
  wide-FOV fogged corridor for the radial-versus-planar metric (#27).

Three of these want a **content scan rather than a capture** before any code
changes: how many K1/K2 TXIs set `filter`/`mipmap` (#29), how many meshes carry
alpha plus an envmap or bumpmap (#20), and how many carry `render == 1` with a
null diffuse texture (#34).

Use deterministic raster captures and compare localized regions, not stale
whole-frame OpenGL baselines (`doc/backlog.md` 7.1 and 7.3).
