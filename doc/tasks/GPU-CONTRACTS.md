# GPU contract hardening

Plan drafted after the pr/gui-scale merge regressions, whose fixes are folded
into the merge itself (`10ea2df64`). They were one defect family: a
shared or cached GPU object whose contract existed only as a coincidence of
call order. The merge changed no Vulkan file; it changed *sequencing*, and the
contracts fell over. This plan makes those contracts explicit and
machine-checked, and makes the next violation loud instead of a device reset.

Phases are ordered by value over cost and are independently landable. Each has
its own verification and its own commit(s).

---

## Phase 1 - one resource epoch, checked at every cache boundary

**Now:** `IRenderer::resourceGeneration()` exists and `GpuSceneAdmission`
compares it once per prepare, dropping its caches on change (the merge `10ea2df64`).
That protects the one cache the bug was caught in. Any other holder of a
backend texture id is still unguarded.

**Change:**
1. Audit every container that stores a bindless id or descriptor-table index
   past a frame boundary. Seed list: `GpuScene` material tables
   (`include/reone/graphics/rendering/gpuscene.h`), ray-query material
   references (`rayquery.h`), scene-side `GpuScene` caches
   (`include/reone/scene/gpuscene.h`), and anything reachable from
   `patchMaterialUv` / `patchBumpMapFrame`-style incremental writes.
2. Each such container records the epoch it was lowered under and compares
   once per prepare - the admission pattern, promoted to a rule. Mismatch
   drops/relowers the container wholesale.
3. Document the rule in `doc/tasks/CONVENTIONS.md`: *a raw backend id may not
   cross a frame boundary without a recorded epoch beside it.*

**CPU budget:** the check is O(1) per container per frame; relower fires only
on invalidation events (module reload, `invalidateResources`). No per-object
per-frame work - consistent with the standing no-CPU-creep constraint.

**Verify:** the two-warp chargen repro at 1024/1920/3440 with
`--vkvalidation 1` (zero VUIDs); full test suite; one capture-matrix run.
Add a unit test that bumps the generation between two prepares and asserts
relowering happened (admission already testable this way).

## Phase 2 - poison the unbound descriptor slots

**Now:** a stale id samples an unbound slot: device lost, no identity, no
image. The fatal id (`mainTex id=110`) was only ever seen via temporary
instrumentation.

**Change:** in `VulkanDescriptors` (`initBindless` region, capacity at
`descriptors.cpp:160`), fill every slot of the bindless table that is not
backed by a live texture with a 4x4 solid-magenta poison texture at each table
rebuild. In dev builds (`--dev 1` or a debug define), log when lowering writes
an id at or beyond the live count.

**Effect:** the next lifetime bug renders as magenta in a capture - the
harness completes, the artifact is diagnosable from the PNG, and the existing
render-verification workflow catches it. Converts the class from
device-reset to visible-artifact.

**Verify:** temporarily disable the epoch relowering in a scratch worktree, run the
repro, confirm magenta-not-crash, restore. Suite + repro green.

## Phase 3 - reflection-driven pipeline layout validation

**Now:** the merge `10ea2df64` sizes the cached pipeline push-constant range from the
largest C++ block and asserts callers fit - but a *shader* growing its block
still only surfaces as a VUID at draw time. `ShaderReflection.pushConstantSize`
(`rhi/computepipeline.h:57`) already carries the truth.

**Change:** at pipeline build in `pipelinecache.cpp`, compare the reflected
push-constant size (and descriptor binding count while there) against the
layout; on mismatch fail with the shader's name. Slang is compiled at runtime,
so this catches drift at startup of the first frame that uses the shader,
named, instead of three warps into chargen.

**Open decision (below):** fatal always, or fatal in dev and log-once in
release.

**Verify:** deliberately grow one shader's block in a scratch tree and confirm
the failure names it; suite + repro green.

## Phase 4 - explicit per-recording frame arena

**Now:** the merge `10ea2df64` gives each recorded scene its own descriptor set from a
fence-recycled pool - the right mechanism, local to descriptors. "One per
frame" elsewhere (uniform ranges, future per-frame state) is still implicit.

**Change:** promote the pattern to the interface: a frame-arena object owned
by the renderer, passed to each recorded scene's render, from which per-frame
descriptor sets and uniform ranges are *allocated*. A second recording in the
same frame allocates rather than overwrites, by construction. Migrate the
descriptor pool from the merge `10ea2df64` into it; migrate uniform ring usage where the
same overwrite shape exists.

This is the largest and least urgent phase - the known instance is already
fixed. Land last, or defer until the next per-frame resource is added.

**Verify:** chargen repro (multiple graphs per frame is exactly its shape),
suite, capture matrix, and a frame-time check against the current baseline -
the arena must not add per-frame allocation cost in the steady state.

## Phase 5 - validation-clean gate in the harness

**Now:** the entire merge diagnosis rested on pre-merge emitting exactly zero
VUIDs. That baseline is an asset and currently defended by nobody.

**Change:** add a `-ValidationGate` step to `scripts/capture-game-proof.ps1`
(or a sibling script): run the short chargen repro with `--vkvalidation 1` at
1024x768 and 3440x1440, fail on any `VUID-` line, print the offenders. ~30s.
Wire it ahead of the capture matrix so a dirty build fails fast, before
40 minutes of captures.

**Verify:** runs clean on current HEAD; seeded with a known-bad build
(pre-fix merge commit) it must fail and name the VUIDs.

---

## Decision points (resolve before the affected phase)

1. **Phase 3 severity in release builds:** fatal at pipeline build, or
   log-once-and-continue? Recommendation: fatal in dev, log-once in release -
   a shipped build should degrade, a dev build should stop.
2. **Phase 2 poison in release builds:** keep the poison texture in release
   (costs one tiny texture, saves a black-screen support case) or dev-only?
   Recommendation: keep in release; log only in dev.
3. **Phase 1 scope:** epoch-stamp only texture ids, or also mesh/vertex-range
   ids in the merged-geometry tables? The sprite-tail work shows geometry
   ranges have the same shifting-namespace shape. Recommendation: textures
   first, geometry ranges as a follow-up audit item.

## Outcome (all phases landed)

Decisions were resolved as recommended: phase 3 is fatal in dev and log-once
in release; phase 2's poison ships in release with dev-only logging; phase 1
covered texture ids, with geometry ranges audited and cleared below.

- **Phase 5** - `98382df67`. Gate script + default-on harness wiring. Proven
  both directions: clean on a good build, failed against a build with the
  epoch fix reverted - which failed on exit code alone, because a stale id
  within table capacity resets the device without tripping any VUID. That is
  why the gate checks both signals.
- **Phase 1** - `1428df374`. The audit found the scene-side classification
  and material interner (including the shadow scene and the patch paths) is
  the only persistent consumer of bindless ids, and the existing admission
  epoch check covers it; everything downstream is frame-owned or rebuilt from
  the checked upload. No new stamps - the rule and a generation-bump
  regression test are the deliverable. No concrete stale-geometry hazard.
- **Phase 2** - `352321c76`. Type-correct 4x4 magenta poisons, monotonic
  high-water bound on the fill (never device capacity). First attempt failed
  its own blind visual review - lit magenta albedo reads as plausible dark
  purple - so the poison carries an alpha sentinel that scene_draw publishes
  as fullbright magenta. Evidence: epoch check disabled, the old device-loss
  repro exits 0 with 216,802 exact #FF00FF pixels; protected control has 0.
- **Phase 3** - `05b335816`. Cached graphics/compute layouts validated
  against reflected push bytes and descriptor capacities; reflection-built
  layouts are correct by construction; tracing's C++ push range checked.
  Proven by growing a shader block: creation-time failure naming the shader
  and both sizes ("postprocess... reflects 32 bytes, layout declares 20").
- **Phase 4** - `b45538010`. Audit across descriptor sets, uniform slices,
  command buffers, tracing scratch, image views, staging and pools found
  ZERO rewrite-in-place instances - the merged descriptor-set fix was the last. The invariant
  and inventory are codified in CONVENTIONS.md; the arena-interface refactor
  is correctly not built.

Follow-ups this work surfaced, deliberately not done:

1. Tracer-side poison visibility: secondary-hit sampling in
   `slang/tracing/material.slang` bypasses scene_draw, so a stale id reached
   only by secondary rays stays safely-bound dark instead of fullbright.
   Extend the marker compare there if field diagnosis ever needs it.
2. Geometry-range epochs: audited, no concrete hazard today; revisit only if
   merged-geometry ids ever start crossing frame boundaries outside the
   admission-checked upload.

## Execution notes

- Each phase is a separate codex dispatch with its own brief, capped at
  45 min, verified by the repro + suite before the next phase starts. Phases
  1-3 are codex-sized; phase 4 needs a design pass first.
- Brief the *intent and the contract*, not the implementation - the phase
  sections above are close to brief-ready.
- Engine runs for verification are required for phases 1, 2, 4, 5
  (headless, command-file driven, as established).
