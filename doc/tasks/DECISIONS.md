# Open decisions and unowned work

Two things [MASTER.md](MASTER.md) cannot hold: places where two documents say
incompatible things and a person has to choose, and work with no owning plan
step. Both were surfaced by the 2026-08-05 consolidation.

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

Do not treat correction 6 as settled either way. **Owner: none.** A `_lit`
pattern texture on a dark surface separates the two operators immediately.

### D7 — Shadow-caster scope for retro

G7 deliberately made the shadow pass draw all opaque and cutout geometry. The
retired path enrolled only selected creature and placeable meshes. **Whether
retro keeps G7's choice is unowned**, and the authored per-node flag that would
be the filter already exists and is unread (FID-018).

## Resolved by the staleness audit — no decision needed

These looked like contradictions and are simply one side being out of date.

| Topic | Stale side | Current side |
|---|---|---|
| Hybrid primary visibility | `vulkan-rt-backend.md:19` says raster owns it now | `phase-f.md` V1 — it is a decision, not code. Confirmed: PT mode returns before G-buffer allocation |
| Live scene hand-off | `renderer-registration-plan.md:65` says `RenderRegistry` | Deleted in `8d37449d`; `GpuScene` took over |
| Retro on Vulkan | `vulkan-remaining-plan.md:152` says OpenGL-only | Retro is G-buffer-based on Vulkan |
| Render-target viewer | `vulkan-remaining-plan.md:6` says empty | Returns a populated list |
| Particles in the AS | `vulkan-rt-backend.md` §9.1 says composite | Admitted as merged quads, Phase D |
| Dependency status | `vulkan-rt-backend.md` §13 says FSR not started, NRD not chosen | Both shipped |
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

**Path tracing quality, no owner:** TRC-006 through TRC-016 (the denoising and
demodulation set), TRC-039 through TRC-044 (the calibration programme and the
observed lighting defects recorded 2026-07-29).

**Raster, explicitly unowned by name:** RAS-019 SSAO and RAS-020 SSR, both marked
"unowned — decide" in G9's table. The argument for deciding rather than
scheduling: both are screen-space approximations of what path tracing computes
properly, so they are raster-only catch-up.

**Game systems:** all of SYS-001 through SYS-006. By volume the largest block of
deferred work in the repository and the least connected to the renderer tracks.

**Tooling:** TOOL-005 the checked-VMA path, which `renderer-redesign-plan.md`
says outright is "worth finishing separately"; TOOL-017 the settings tool;
TOOL-019 through TOOL-021.
