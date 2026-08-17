# Open decisions and unowned work

Two things [MASTER.md](MASTER.md) cannot hold: places where two documents say
incompatible things and a person has to choose, and work with no owning plan
step. Both were surfaced by the 2026-08-05 consolidation.

**Re-checked 2026-08-09** against `8611ae52b`. Three of the seven contradictions
have since been answered by the code — D3 in part, D6 and D7 outright — and two
unowned blocks have owners or implementations. They are marked resolved in place
with the commit that did it, not deleted: a question that was worth asking is
worth being able to find again, and the reasoning on both sides is what makes
the answer checkable.

## The standing rule for settling them

**Anything that brings the picture closer to the original game goes into every
render mode, unless it obviously regresses the advanced lighting or material
features of PBR and path tracing.** Fidelity work is not retro's alone: retro is
where a difference from the original is *measured*, not where the correction
belongs. Where a fix is a correction to how authored data is read — a cutout
threshold the format supplies, an animated texture the TXI declares, a
classification the material asks for — it is a correction in all three modes,
because all three read the same data and none of them benefits from reading it
wrongly.

The exception is narrow and has to be argued, not assumed: a change regresses
the other modes when it would undo something they do *better*, not merely
differently. Retro's non-inverse-square falloff and its radius-squared cull are
the model of a difference that stays retro-only, because the corrected model is
the point of the other two.

This decides the default for every row below and every RAS/TRC item: propose it
for all three modes, and say explicitly which mode it is confined to when it is
confined at all.

*Recorded 2026-08-16, from the fixture and force-field work: the TPC header
cutout threshold and the ARE grass threshold went into the raster gate and both
tracer gates together, and RAS-030's animated-grid indexing is the same kind of
fix.*

## Contradictions a person must settle

Ordered by how much downstream work they block. Each names both sides; where the
code settles the factual half, that is stated, but the *policy* half is still a
choice.

Citations below like `phase-f.md:277` name a **retired** document — they record
where each side of the argument was made, not somewhere you can go and read it.
The nine source documents were deleted on 2026-08-05; their surviving content is
in DESIGN, RECORD, CONVENTIONS and FIDELITY, and git history has the rest.

### D1 — Room visibility: performance argument versus preservation argument

**`phase-f.md:277`** justifies deleting the VIS branch on measurement: removing
about 9,000 frustum tests per frame moved frame time by nothing, so the branch
was deleted outright rather than given a camera-appropriate key.

**`FIDELITY.md` #17** records the consequence: every room is
now always drawn, the `.vis` graph is parsed and never read, and both reference
engines apply room adjacency **camera-independently** — which is the property the
deletion gave up rather than adopted. Retail had two policies, adjacency indoors
and all-rooms outdoors; we implement only the second.

These are different axes and neither document acknowledges the other. The
performance claim is not in dispute. The question is whether retro accepts
drawing rooms the original hid. **Owner: none.** Blocks STR-022 and interacts
with the sky chain, since every backdrop room but one is currently world
geometry.

### D2 — Transparent ordering: sort or preserve submission order

**`phase-f.md:894`** (G8) calls for a CPU sort of the blended set feeding a
per-triangle remap buffer.

**`phase-f.md:1408`** (its own reference correction 8) says keep submission order
for non-opaque draws, because kvp's replay of the retail game reorders only
true-opaque depth-writing geometry — and warns that a remap sort will produce
regressions the original did not have.

The same document argues both sides, and commit `eed85ac5` calls the current lack
of sorting deliberate. **Owner: G8.** Blocks RAS-006 and RAS-007.

### D3 — Material derivation order: authored map first, or heuristic first

**`vulkan-rt-backend.md` §8.2** lists the heuristic as steps 1–2 and the authored
override table as step 3.

**`backlog.md` 3.2** and **`renderer-registration-plan.md:1231`** both insist the
authored map must precede the heuristic, because shipping the guess first makes
every later authored entry a correction rather than a decision.

The rt-backend ordering appears to be the older text. **Owner: TRC-017.**

**Partly answered by the code, 2026-08-09.** `735fccd69` gave PBR and the tracer
one material chain in `slang/lib/material_ops.slang`, and `resolveMaterial`
(`:76-110`) states its ordering as load-bearing rather than incidental: the
heuristic derives a value, curation runs against the albedo it derived, the
category override then *replaces* that albedo, and the roughness scale and floor
close over whatever survived. So the rt-backend ordering is what shipped — but
the objection that motivated the other side does not survive it, because an
authored entry now replaces rather than corrects. What remains for TRC-017 is
the authored name→material map itself, which does not exist yet; the evaluation
order it would slot into is settled and single-sourced.

### D4 — Static/dynamic BLAS: one structure or two

**`backlog.md` 8.9's design prose** argues at length for one persistent
world-space buffer partitioned static and dynamic, with two BLAS over it.

**What shipped, and `renderer-redesign-plan.md` decision 5**, is one BLAS fully
rebuilt every frame, with the split kept only as an escape hatch pending the AMD
measurement (TRC-035).

The prose is retained as a design record but reads as a live proposal. **Owner:
S4.** Settle by doing TRC-035 first.

### D5 — Debug and walkmesh geometry: delete or re-admit

**`cleanup-plan.md:648`** argues walkmesh and trigger geometry should be deleted
rather than excluded, because it carries `offMaterial`, "the one attribute
`MergedVertex` structurally cannot express".

**`backlog.md` 7.8** reverses this: the merged stream already carries a
per-triangle material id, so walkable versus non-walkable is two materials over
one mesh and no vertex format widens. Debug geometry comes back *inside*
`GpuScene`, in a region the BLAS never asks for.

7.8 is the newer and better-evidenced argument. **Owner: none.** Blocks STR-023
and STR-025.

### D6 — Self-illumination: additive or multiplicative

**`phase-f.md` reference correction 6** says vanilla adds emission on top of the
texture, citing kvp.

**`FIDELITY.md` #35** shows the references disagree: KotOR.js
multiplies with a 0.25 floor, fixed-function `GL_MODULATE` matches what reone
already does, and kvp's line sits inside its own legacy-to-PBR conversion with
compensating heuristics on the next lines.

~~Do not treat correction 6 as settled either way. **Owner: none.**~~

**Resolved 2026-08-09: additive shipped.** `eeb22ffe8` moved retro's lightmapped
branch to what it reports all three references agreeing on — the lightmap
multiplies the diffuse texel at full strength, never shadow-attenuated, never
clamped against the dynamic sum, **with self-illum added after the product** —
and `1df5164ce` collapsed the remaining branches into it. The expression is
`shadowFactor * (baked + ambient + max(0, direct)) * diffuseSample.rgb +
selfIllumSample.rgb` (`slang/retro_resolve.slang:227-229`), with a measurement
behind it: the Dantooine far ridge, 46k pixels, luma 18 → 67.

Note what is and is not settled. Correction 6's *conclusion* is now the code;
its *evidence* is still the weakest of the three, and FIDELITY #35 keeps the
record that `GL_MODULATE` and KotOR.js both argue the other way. If the additive
operator is ever seen to be wrong, that row is where the counter-argument
already is. The `_lit` pattern texture on a dark surface is now a regression
fixture, not a discriminator.

### D7 — Shadow-caster scope for retro

G7 deliberately made the shadow pass draw all opaque and cutout geometry. The
retired path enrolled only selected creature and placeable meshes. ~~**Whether
retro keeps G7's choice is unowned.**~~

**Resolved 2026-08-09 by `4dcbce2e9`: the caster set is per mode.** Retro's holds
`Creature` and `Equipment` and nothing else — "that is all the original ever
drew: a stencil volume per creature, no shadow from architecture or terrain at
all" — while PBR and the tracer keep every opaque surface, where terrain
shadowing terrain "is geometry rather than error"
(`src/libs/scene/render/pipeline/renderpipeline.cpp:141-157`). It is enforced as
a category bitmask in the shadow fragment (`slang/scene_draw.slang:154-170`)
rather than as a separate submission, so the caster range stays contiguous and
the draw count does not depend on how many creatures are in the room. The commit
also reports what it bought: the mottling on distant hills was terrain shadowing
itself across cascade texels, and a terrain that never casts cannot.

FID-018 survives this as a narrower item. The authored per-node flag
(`mdlmdxreader.cpp:237,477` → `modelnode.h:80`) is still unread, so nothing can
refine the caster set *within* a mode — a creature mesh authored not to cast
still casts in retro. That is a refinement, no longer the decision.

## Resolved by the staleness audit — no decision needed

These looked like contradictions and are simply one side being out of date.

| Topic | Stale side | Current side |
|---|---|---|
| Hybrid primary visibility | ~~`vulkan-rt-backend.md:19` says raster owns it now~~ — it was right, just early | **Now code**, `4980518cc` 2026-08-06. `slang/tracing/primary.slang` reconstructs the primary surface from the G-buffer's triangle id and `slang/path_trace.slang:201` calls `ptReadPrimary` instead of casting a camera ray, so raster owns primary visibility in every mode. Consequence for anyone reaching for it as evidence: a traced-versus-raster G-buffer comparison is now vacuous — the two agree by construction, which `scene_draw.slang:85-90` makes an explicit invariant |
| Live scene hand-off | `renderer-registration-plan.md:65` says `RenderRegistry` | Deleted in `8d37449d`; `GpuScene` took over |
| Retro on Vulkan | `vulkan-remaining-plan.md:152` says OpenGL-only | Retro is G-buffer-based on Vulkan |
| Render-target viewer | `vulkan-remaining-plan.md:6` says empty | Returns a populated list |
| Particles in the AS | `vulkan-rt-backend.md` §9.1 says composite | Admitted as merged quads, Phase D |
| Dependency status | `vulkan-rt-backend.md` §13 says FSR not started, NRD not chosen | Both shipped. Since `490df9461`: `ENABLE_FSR` defaults on under MSVC, `ENABLE_NRD` and `ENABLE_TRACY` default off, so a default build has neither NRD nor Tracy zones in it |
| Backend selection | `vulkan-rt-backend.md` §2.1 describes `--backend` | Deleted with OpenGL |
| Grass cluster pool | cleanup and registration say 2048 | 4096 start, 32768 cap |
| Toolkit preview | `cleanup-plan.md:386` says still open | Fixed; a camera bug, not rendering |

## Unowned work

Items with no plan step. Most are small; the point of listing them is that "no
owner" is how the previous nine documents accumulated.

**Fidelity, no owner at all:** FID-004 light selection, FID-005 dynamic light on
lightmapped geometry, FID-006 area shadow flags, FID-007 shadow options, FID-008
fog metric, FID-009 anisotropy, FID-010 TXI flags, FID-011 BC mips, FID-012 dead
draw-distance slider, FID-014 destroy fade, FID-015 null-diffuse meshes, FID-016
self-illum operator, FID-017 face culling, FID-019 the TSL loader case.

Two of those changed under the list on 2026-08-09 and want re-reading before
anyone picks them up. **FID-016** is settled in code — retro adds self-illum
(D6 above), so what is left is a regression check, not a choice. **FID-017**
lost its premise: the shadow pass stopped culling in `4dcbce2e9`, so the tree is
uniformly two-sided by decision and the internal inconsistency FIDELITY #5
described is gone. **FID-004** is cheaper than it was: the light count is a live
dial since `58c55eaab` (`--maxlights`, default 48 against a 64 ceiling), so
sweeping it is a capture rather than a code change.

**Path tracing quality, no owner:** TRC-006 through TRC-016 (the denoising and
demodulation set), TRC-039 through TRC-044 (the calibration programme and the
observed lighting defects recorded 2026-07-29).

**Raster, explicitly unowned by name:** ~~RAS-019 SSAO and RAS-020 SSR, both
marked "unowned — decide" in G9's table.~~ **Both were built rather than
decided**, `fedcb7445` 2026-08-06: PBR resolves in a compute dispatch, so
ambient occlusion computes inside the resolve from depth and normals and
replaces the `ao` term that was hard-coded to 1, and reflections are a second
dispatch reading the lit image the resolve wrote — a kernel reading the image it
writes would be reading whichever neighbouring workgroups happened to have run.
Reflections substitute for the authored cube at the same `1 - alpha` weight
rather than adding to it. Both are on by default (`options.h:615-616`), and with
both off the frame is the frame from before. The argument for deciding rather
than scheduling — that both are screen-space approximations of what path tracing
computes properly — was never answered; it was overtaken. Note MASTER's rows
still describe the old state (`pbr_ssao.slang` "exists unwired"; that file is
gone) and want updating.

**Game systems:** all of SYS-001 through SYS-006. By volume the largest block of
deferred work in the repository and the least connected to the renderer tracks.

**Tooling:** TOOL-005 the checked-VMA path, which `renderer-redesign-plan.md`
says outright is "worth finishing separately"; TOOL-017 the settings tool;
TOOL-019 through TOOL-021.
