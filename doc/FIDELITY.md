# Retro fidelity

Where retro still differs from the original, with the evidence and the proof each row
needs. **A source difference is not a regression unless it has an observed failure or
an active plan calls it unfinished.**

Rows key to the items in [TASKS.md](TASKS.md). Status: *defect* (reproduced or
recorded as wrong) · *omission* (field parsed, no consumer) · *reference-open* (the
reference supports the difference, no visual failure shown) · *unproven* (code
differs, effect not shown).

## Reported symptom → check in this order

Candidate linkages by likelihood, **not diagnoses**. Add a symptom here when a report
is traced.

| Symptom | Order | Why |
|---|---|---|
| Force field wrong | RAS-004, FID-002, RAS-005 | Classification decides whether it blends at all; the authored hint is the missing signal; the opaque range is the alpha-plus-selfillum case that bypasses the check |
| Manaan water wrong | RAS-004 | `wateralpha` reaches the material but the water branch is bypassed when `TransparentModel` classifies as Cutout |
| Dantooine interiors too dark | FID-004, FID-005, FID-006 | All three change the same pixels |
| Particles missing or wrong | FID-003 | The emitter omissions as a set. **Start from the census** before assuming which field is at fault |
| Objects pop instead of fading | RAS-005, then FID-014 | Destroy fade is a second consumer blocked behind the same defect |
| Geometry visible that should not be | STR-022, then TRC-024 | Room visibility is gone, so backdrop rooms are drawn; the sky overlaps for backdrop geometry |
| Too many things cast shadows | FID-018, then FID-006 | The per-node flag that would filter within a mode is unread; the area-level enable is ignored |

## Coverage and transparency

| Item | St | Evidence | Proof |
|---|---|---|---|
| **FID-001** alpha-test reference value | reference-open | We test at **0.5** in the gated G-buffer draw and both shadow passes, and the tracer mirrors the constant — load-bearing now that it reads its primary from this G-buffer. xoreos sets `GL_GREATER, 0.1f` globally: **five times the reference**, and the coverage removed is non-linear in the alpha distribution, so **measure, do not reason** | A foliage silhouette at 0.1 and 0.5. **Both constants must move together.** A preservation question, not a taste one |
| **FID-002** the opaque range is untested | unproven | The opaque draw runs **no alpha test at all**, where retail alpha-tested everything. Worse, `isTransparent` returns false for any node with an **envmap or bumpmap**, and for self-illum luma >= 0.99, both **before** the alpha check — such meshes render transparent texels solid | Scan K1/K2 for alpha plus envmap/bumpmap, then capture one. **Mechanically certain; unproven only for lack of a capture** |
| **RAS-004** `transparencyHint` unread | omission | Parsed into `TriangleMesh::transparency`, no consumer anywhere. xoreos prefers the authored hint and falls back to "has an alpha channel" only when absent; we classify purely from texture alpha and TXI, which is why `TransparentModel` goes to Cutout — and moving all of it to blended breaks foliage | **This is the narrower blended signal, already in the file format.** Hint-first with the alpha test as fallback, then re-run the foliage/pane/water fixture |
| **RAS-005** per-node alpha fade never renders | omission | `_alpha` reaches `diffuseColor.a`, but only the **blended** pass reads it; the G-buffer fragment and coverage gate read `input.color.a`, hard-coded to `1.0` for all MDL geometry. A fading mesh classifies Cutout, so it draws at full opacity **and** is discarded by the blended pass — it never fades, it vanishes at zero. **Every door, placeable and appear/disappear fade is lost** | Fix with RAS-004; a placeable driven 1 to 0, captured at three intermediate values |
| **RAS-008** additive without alpha | reference-open | xoreos uses `SRC_COLOR, ONE`; our alpha-zero output yields `ONE, ONE`. Narrower than "all additive is wrong" | Identify the affected cases before choosing a material value or exceptional path |
| **FID-017** face culling | unproven | Mesh data asks for back-face culling; the tree never culls, in all three passes. **Now a decision, not an omission** — a single-sided Odyssey wall must occlude from whichever side the light is on, and writing both faces supplies the depth bias the shadow pass relies on | A closed shell, a back-facing card, a decal. **Assign an owner only if retail's culling changes visible pixels** |

## Emitters and particles (FID-003)

All omissions: parsed, reaching nothing. Counts from fresh scans — K1 read 2,847 models /
8,870 emitters, TSL 3,001. **Use the census to find a real authored example before
building any fixture**, and do not infer behaviour from standalone bit 2.

| Field | Missing | Reference |
|---|---|---|
| `tinted` | Never changes particle colour. 714 K1 / 592 K2 set it | KotOR.js applies an ambient-derived tint |
| `particleRot`, `blurLength` | Neither is read; motion blur uses a fixed `0.25 × 16` stretch | KotOR.js applies `particleRot × age` and the authored length |
| `sizeStartY/MidY/EndY` | No consumer, so every lifetime-sized particle stays square | KotOR.js carries a separate Y scale |
| `renderOrder` | Never affects admission or the draw. 1,904 K1 / 1,014 K2 non-zero | KotOR.js assigns it to the emitter mesh |
| P2P Bézier, wind, bounce, inherited motion | Bit 2 is `P2P_SEL`, meaningful only with P2P (16 K1 / 17 K2 set both); gravity is implemented only when P2P is set *without* it, so the selected branch has no Bézier path. Wind (154/226) and bounce (453/463) have no consumer | — |
| `uvJitter`, `uvJitterSpeed` | Read and discarded; retained UV state carries only scroll direction | KVP preserves both; KotOR.js feeds `UVJitter` |
| `aligned_to_world_z` | The instance switch has no case and falls back to camera-facing. 20 per game | KotOR.js has a distinct shader mode |

**RAS-009** — the emitter blend mode is parsed but only `Lighten` is tested, so
Punch-Through emitters carry no blending and classify `LitBlended`: continuous alpha
blending, no alpha test, no depth write, no G-buffer or shadow presence, **visible as
hard-edged sprite cards rendering soft.** KotOR.js maps the same case to normal blending
with `alphaTest = 0.5`; map it to the cutout path.

## Lighting, shadows, sampling, distance

| Item | St | Evidence | Proof |
|---|---|---|---|
| **FID-004** light selection policy | omission + unproven | Ceiling 64, budget a live dial at 48. Selection is a **single global set by camera distance**, so every surface sees the same lights. KotOR.js models 8 global / 3 per model from `NumDynamicLights` — **but neither KVP nor xoreos corroborates the per-model cap, and a settings table is not proof of the renderer's internal limit, so treat 8/3 as one reimplementation's model.** Three further gaps: no per-model assignment; no light frustum culling; MDL light `priority` parsed and read by nothing | A room with >8 authored lights swept across `--maxlights`, and one model inside three overlapping volumes. **Since the budget became a dial this is a capture away, not a code change** |
| **FID-005** dynamic light on lightmapped geometry | reference-open | We apply every `dynamicType == 1` light to lightmapped static geometry and modulate by albedo; KotOR.js drops `directDiffuse` **entirely** there and adds only the animated-light array, additively, unmodulated, at half strength | **It annotates its own `* 0.5` as a hardwired shadow intensity, so it is doing double duty and is not a clean oracle.** Capture a lightmapped interior with a moving light first |
| **FID-006** `SunShadows`/`MoonShadows` ignored | omission | Parsed, stored, pushed to the graph, which reads only `opacity`. Enablement comes solely from the MDL light's flag, so an area authored `SunShadows=0` still casts | Honour them, or record ignoring them as deliberate the way `ShadowOpacity` was |
| **FID-007** no shadow / soft-shadow option | unproven | Retail exposed both, per the ini KVP reproduces — **which establishes the options existed and were user-facing, not what either switched between.** Our four-cascade CSM is unconditional | The missing *options* are separable, cheap and evidenced; the technique question needs a source describing retail's implementation, which none of the three has |
| **FID-018** caster scope within a mode | omission | The set is per mode and settled: retro holds `Creature` and `Equipment` only, the other modes every opaque surface. **What is left is narrower:** the authored per-node MDL caster flag has no renderer reader, so a creature mesh authored not to cast still casts | KotOR.js gates on exactly that flag, and records that the original derived creature shadows from the **skeleton**, not the render mesh |
| **FID-009** anisotropy on by default | unproven | Applied to **every** texture loaded, defaulting to 4x; xoreos sets none anywhere. **That is one reimplementation's choice and does not establish what retail did.** What is certain is we turn it on and nothing records that as a decision | Check the retail ini and KVP's captured sampler state **first**; if retail set none, a preservation default of 1x follows. Either way a grazing-angle floor at 1x and 4x |
| **FID-010** TXI `filter`/`mipmap` unparsed | omission | The reader parses eight other fields but not these. xoreos honours `filter` — false gives nearest with no mips — and forces it off for cursors. **We cannot express an authored unfiltered texture** | Scan the TXI set to size how many set them |
| **FID-011** BC textures get no mip chain | unproven | `generateMips` excludes compressed formats, so a BC texture shipping one level never minifies; xoreos generates a chain. **A second upload path computes the same flag without the `!compressed` term, so the two disagree** | Settle which path is live; count the exceptions first, since most TPCs ship chains |
| **FID-012** draw distance is dead code | omission | Plumbed from CLI, editor, registry and launcher into `setDrawDistance`; the getter has **no caller anywhere**, so the slider does nothing, and **the three ranges disagree** — 32-128, 1-1000, 1-100000 (TOOL-028) | Restore culling or remove the option and its sliders. **Do not leave a control that silently does nothing** |
| **FID-013** grass distance policy | unproven | The cull is a live option at **25**, from the **camera**; the size ramp still hardcodes **32**, so at the default the ramp is inert (RAS-034). KotOR.js fades **25 to 100** from the **player** and disables the fade during dialogue. **The visible delta is the last one:** a dialogue camera away from the player strips grass around the player | A dialogue camera on a grassy exterior. The camera-versus-player term is the cheap half and is independently testable |
| **FID-015** null-diffuse meshes dropped | unproven | `shouldRender` requires a non-empty `diffuseMap`; the reader blanks the name when texture1 is `"null"`. **Neither reference makes texture presence a visibility condition**, so a lightmap-only mesh would be drawn by both and is not drawn here | Scan for `render == 1 && texture1 == "null"`. **Do not call this a player-visible regression until that scan finds an instance** |
| **FID-008** fog distance metric | unproven | Retro's fog is per-pixel **radial**; `GL_LINEAR` and KotOR.js both use view-space depth, i.e. **planar**, per vertex, and radial fog is thicker toward the screen edges. **The rest of the chain matches** | One-line change; a wide-FOV corridor with fog tightened. `MoonFog*` is unused here *and* in KotOR.js — a shared unowned field, not a deviation |
| **FID-014** destroy fade ignored | omission | `DestroyObject` reads `bNoFade` and `fDelayUntilFade` and discards both; `SetIsDestroyable` throws. KotOR.js fades to zero over 10 s, so **corpses and destroyed placeables pop** | Blocked behind RAS-005 — partial alpha does not currently render |
| **FID-019** TSL `n_forcezombie` load | unproven | The loader aborts on a node name without a NUL terminator in its 32-byte window; KVP reads the table as variable-length C strings, and one TSL model is skipped | **Establish whether it is reachable in game** before treating a tolerant read as a render fix, and do not weaken other malformed-file checks |

## Settled — do not re-open

- **Odyssey light falloff and range cull.** Retro's cull compares a linear distance
  against `radius²` and its falloff is `radius²/(radius+d)²`. **Both are dimensionally
  wrong and both are deliberate** faithful ports; retro keeps them, PBR retires them.
  **Reject bug reports against the retro light loop that argue from dimensional
  analysis.**
- **Self-illumination operator.** Retro adds rather than multiplies, with a measurement
  behind it (Dantooine far ridge, 46k pixels, luma 18 → 67). **The references are not
  unanimous** — `GL_MODULATE` and KotOR.js's 0.25-floor multiply argue the other way — so
  that disagreement is recorded rather than resolved, and **if the additive operator is
  ever seen to be wrong, this is where the counter-argument is.**
- **MDL vertex colours.** Ruled out: `MergedVertex.color` is a literal white constant and
  the loader never reads `offMdxVertexColors`, so `diffuse *= input.color` is a guaranteed
  no-op.

## Confirmed matches — do not re-open either

Negative results carry weight: each closes a plausible line of enquiry. **Lightmap
composition** matches xoreos's multiply pass · **ambient policy** is suppressed on
lightmapped and static geometry, with DynAmbient on dynamic objects only · **fog exists
and is wired**, only the metric differs · **UV scroll, bump-frame cycling and self-illum
animation** all reach the merged material record, with the one caveat that bump cycling
applies only to grayscale bump maps, since a non-grayscale one routes to the NormalMap
slot · **TXI `clamp`** is unparsed here and parsed-then-ignored by xoreos, wrap being
`REPEAT` in both · **no model LOD anywhere**, since Odyssey MDL carries no such field and
reone's and xoreos's independent header reads agree · **no camera-proximity object
fading** in retail, reone, or any reference. Two Unity reimplementations supply **format
corroboration only** — they confirm the emitter fields are real Odyssey data, but neither
implements Odyssey render state.

## Work order

**Transparency first, and it is larger than it looks:** start from **RAS-004**, the
authored hint already in the format, and fix **RAS-005** in the same change since it is
the same defect from the other side; then the coverage rule (**FID-001**, **FID-002**),
then RAS-008, RAS-009 and emitter order. **Give the particle-simulation move an acceptance
set** — FID-003 and RAS-009 must survive it, starting with tint, rotation/size and
alignment. **Treat FID-004, FID-005, FID-008, FID-009, FID-011, FID-013, FID-015 and
FID-017 as evidence-gathering, not fixes**, of which FID-008 and FID-009 are one-line
changes once the capture exists. And **FID-006, FID-010, FID-012 and FID-014 are cheap
omissions with no owner, worth one sweep** — each currently presents a control or authored
field that silently does nothing.

## Fixtures (FID-020) and content scans (FID-021)

Eleven fixtures, by area. **Transparency:** a triple-alpha set (punchcard, blended pane,
additive glow) plus a depth-interleaved transparent pair; one foliage card at alpha-test
0.1 and 0.5; one mesh carrying both a live alpha channel and an envmap or bumpmap; a
placeable driven 1 to 0, captured at three intermediate values and reused for destroy
fade. **Emitters:** tinted and rotating sprites, non-square lifetime sizing, two blur
lengths, two render orders, world-Z alignment, one Punch-Through beside one Lighten, and
census-selected P2P, bounce and wind examples. **Lighting and sampling:** a room with
more than eight authored lights swept across `--maxlights`; one model inside three
overlapping volumes; a lightmapped interior crossed by a moving light; a grazing-angle
floor at 1x and 4x anisotropy; a wide-FOV fogged corridor. **Scene:** an interior doorway
and a Korriban-style backdrop room in third person and free camera; a closed shell, a
back-facing card and a decal; an opaque prop, lens flare, bloom source and edge-heavy
geometry in all three modes and each AA method.
**Three want a content scan rather than a capture** before any code changes: how many
K1/K2 TXIs set `filter`/`mipmap`; how many meshes carry alpha plus an envmap or bumpmap;
and how many carry `render == 1` with a null diffuse texture.

Use deterministic raster captures and compare localized regions — see
[CONVENTIONS.md](CONVENTIONS.md).
