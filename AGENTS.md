# AGENTS.md — working in reone

reone is a KotOR/TSL engine reimplementation. C++ engine under `src/` and
`include/reone/`, Slang shaders under `slang/`, Vulkan-only renderer (OpenGL
was deleted in `df1aa375`; do not add GL paths, comparisons, or `--backend`
flags). All renderer/RT work happens on the `path-tracing` branch.

Read before proposing work, in this order:

- `doc/phase-f.md` — the feature track (raster rebuild G-steps, runtime
  R-steps, sky V-steps, the path-tracing substage). Current state table at the
  top.
- `doc/renderer-redesign-plan.md` — the structural track (S-steps: scene
  inversion, Slang runtime, residency, RHI) and the two invariants all
  renderer work is judged by.
- `doc/backlog.md` — everything outstanding, prioritized, including the
  postmortems. If your idea appears there struck through, it was tried;
  read why it died before re-proposing it.
- `.claude/skills/reone-diagnostics/SKILL.md` — the full measurement harness
  and its trap list. The short version is below, but the skill is the
  authority.

## Design direction

**The mode contract.** There are three render modes and one rule: **Retro is
preservation, PBR and path tracing are improvement.** Retro reproduces what
the original game did — a deviation there is a bug, not a taste question, and
the reference engines settle disputes — read-only checkouts of:

- **xoreos** (authority on GL semantics) — https://github.com/xoreos/xoreos
- **KotOR.js** (gameplay policy) — https://github.com/KobaltBlu/KotOR.js
- **KVP**, the KOTOR Vulkan Pipeline (the retail draw stream, and the only
  prior art for PBR over these assets) — https://gitlab.com/nineteenss/kvp

Checkout location varies per machine; the findings survey and where-each-wins
rules live in `doc/phase-f.md`'s reference section. PBR/PT are held to looking right, not to matching 2003.
Never trade these against each other inside one mode.

**One scene description.** Raster and the tracer consume the same admission,
the same merged geometry, the same material table. The invariant is checkable
and checked: the `GpuSceneUpload` hash must be equal across all three modes at
the same camera and frame. Renderer-side artifacts (the transparency sort
remap, BLAS ranges) are derived data and must stay *out* of the shared
description so the hash equality survives.

**The structural invariants** (from the redesign plan; apply them to any new
code, not just S-steps):

1. Render-side CPU is O(changes), never O(objects) or O(passes). A new render
   feature adds a GPU pass, a pipeline, a shader — never a per-frame
   per-object CPU loop. That hook pattern is how every existing creep entry
   got in, and the structural track exists to delete the hook.
2. The CPU/GPU seam is a data contract. Scene state is schema (GPU-shared
   structs), not call graphs. A new consumer of scene data should need zero
   scene-side C++ changes.

**Settled decisions — do not re-litigate without new measurements:** one BLAS
for the whole scene, full rebuild per frame (per-mesh structures and refit
schedules were deleted; the AMD caveat is backlog 8.15); the sky is a curated
offline asset, never a runtime classification (the classifier was built,
swept over 117 modules, and lost 46 skies — backlog 1.14); hybrid primary
visibility is decided but **not landed** — raster will own the G-buffer in
every mode (phase-F V1), but today the tracer still traces camera rays; grass
generates on the GPU from integer hashes; culling buys nothing here
(~9,000 frustum tests/frame measured at zero frame-time change).

## Coding style

- Match the surrounding file. Prevailing conventions: `PascalCase` types,
  `camelCase` functions, `_camelCase` members, `kCamelCase` constants,
  interfaces prefixed `I`. Headers in `include/reone/<lib>/`, implementations
  in `src/libs/<lib>/`.
- **GPU-shared structs are contracts, with one declaration each.** The Slang
  side lives in `slang/lib/scene_schema.slang`; the C++ mirror lives in
  `include/reone/graphics/gpuscene.h` with `alignas` and `static_assert`ed
  offsets. Both move in the same commit — the renderer reflects the schema at
  startup and aborts naming the field if they disagree. Beware storage-buffer
  array stride when adding fields (backlog 0.3: a grown struct caused
  `VK_ERROR_DEVICE_LOST`).
- Comments state constraints and reasoning the code can't show — this repo's
  comments are load-bearing (they record why, measurements, rejected
  alternatives). Never narrate what the next line does, and never write
  comments addressed to a reviewer about the change itself.
- Vulkan objects owning VMA allocations must be released in an explicit
  `deinit()`, never left to destructors — a member destructor running after
  the allocator is destroyed is the standing teardown-crash class. Silent
  startup crashes around Vulkan libraries are lifetime/symbol bugs until
  proven otherwise (volk's `vk*` data symbols collide silently).
- Docs: decisions are recorded in `doc/*.md` with rationale and the numbers
  that forced them. Steps are sized to one agent context, briefable in a
  page, prove their own work positively, and are committable alone.
- Commits: `[area] sentence` style — `[graphics] …`, `[scene] …`, `[game] …`,
  `[doc] …`. One commit per decision, not one per exchange. **No co-author or
  generated-with trailers.** Surface decisions to the developer driving the
  session *before* acting on them, not as a footnote after.

## Testing methodology

**Build correctly before believing anything:**

- `cmake --build build --config Release` → `build/bin`. The final `toolkit.exe`
  link failure is known noise; `engine.exe` links before it.
- Named-target builds (`--target engine`) **skip the test suite**. Build the
  default target, then `--target tests` explicitly and run
  `build/bin/tests.exe` (~350 tests, under a second). A binary that links is
  not a suite that passes.
- Check `build/bin/engine.exe`'s timestamp against your edit before trusting
  any run. Five consecutive runs once "proved" a fix using a stale binary.
- **Shaders are not built.** Slang is linked into the engine and compiles
  `slang/` at startup, cached on disk under a hash of every source file, so a
  shader edit needs no rebuild — restart, or run the `recompileshaders`
  console command. A source error logs and keeps the last good module rather
  than taking the frame down. This retires the old `compile_spirv` stale-module
  trap that older docs still warn about; if a shader edit still seems
  ineffective, check the log for a compile error before suspecting descriptors.
- Debug (`--config Debug` → `build/debug/bin`) links the checked VMA — use it
  for teardown crashes; it names leaked allocations.

**The capture harness** (full detail in the diagnostics skill):

```
build/bin/engine.exe --game <GAME_DIR> --dev 0 --mode <mode> --pbr <0|1> \
    --grassdensity 1 --headless 1 --commands-file <abs-path>\warp.txt \
    --capture out.tga --captureframe 310
```

- Pass `--dev 0`, `--mode`, `--pbr` **explicitly every time** — untracked
  `build/bin/reone.cfg` wins any flag you omit, and `dev=1` draws a live FPS
  readout into the captured image, forging regressions.
- Frames below ~300 are the splash screen and prove nothing. Iterate at 310;
  frame 900 only for legacy baselines. Emitters fill slowly — capture late for
  particles.
- Capture runs are deterministic per binary (fixed 1/60 step, input dropped,
  seeded RNG) but **not across binaries** — load-timing shifts animation
  phase, so cross-commit dumps must mask animated actors or expect drift.
- Keep `build/bin` clean: commands files, captures, logs, and analysis output
  go to a scratch directory, never the working tree.

**The bar, per mode:**

- **Raster is byte-exact.** Two captures differing by one pixel (outside a
  known HUD box) is a real bug, not noise. Compare hashes, not tolerances.
- **Traced output is nondeterministic** (AS build order): compare
  distributions, N runs a side, never a single pair against a stored
  baseline. `--ptdenoise 0` makes traced *energy* stable to six decimals; NRD
  is the entire source of run-to-run variance.
- **Cross-mode geometry comparisons need `--taajitter 0` on both sides**, or
  alpha-edge sampling noise dominates (measured: 2.94% apparent vs 0.67%
  real).
- Localize before interpreting: `--dumptargets <dir>` writes every pipeline
  target as `.npy`; compare RGB not RGBA; diff per-region, not whole-frame.

**The verification instruments — use them, keep them alive:**

- Upload-hash equality across modes (log line), and the `admissionShadow`
  oracle: a full rebuild compared against the incremental scene every frame.
  Any registration/admission refactor runs acceptance with the oracle armed,
  including a module transition.
- The traced-vs-raster G-buffer agreement numbers (phase-F G2: depth error
  ~0.0128% of pixels) — the geometry-correctness instrument until V1c, and
  the planned `RTDebug` mode after (backlog 7.9).
- Isolation fixtures beat game modules: `warp testbed [grass|smoke|none]`,
  `scene empty`, `spawn`, and the `campos`/`camlook`/`camstatus` scripted
  camera. Build a scene where the answer is unmistakable instead of arguing
  about a screenshot of Dantooine.

**Measuring cost:**

- CPU: Tracy is built in (headless: run engine, `tracy-capture.exe -s 10`,
  `tracy-csvexport.exe`). Zone totals are **inclusive** — never sum parents
  with children. Attach at steady state, never during load. Absolute timings
  are hardware-specific — calibrate on the machine you measure on, and
  sanity-check the *shape* instead: the `update` slot dominates real CPU in
  both modes, most of `graphics` is present-block (GPU wait, not CPU work),
  and `collectInto`/admission-prepare are small post-R1/R3 zones — if either
  reads several tenths of a millisecond, the run hit the loading window or a
  stale binary.
- GPU time does **not** exist in Tracy here — until backlog 2.5 lands it
  comes from in-engine readouts or Nsight.
- Never time with `--vkvalidation 1` (3× cost) or extra `--logch` channels
  (the identity-stability check measures itself). PT trace-stats stay off
  (their atomics once cost 95% of the frame).
- Wall-clock: `(t(900 frames) − t(300)) / 600`, discard a warm-up run, two
  samples after; differences under ~10% on one module are unproven.
- Attribute cost to something you measured. A ratio of call counts is not
  evidence; time the thing before optimizing it.

**Judging renders:** do not certify a visual result from reasoning alone —
render it and look, and for anything visible in play get a human's
confirmation before committing. Five G7 shadow corrections came from play
after the acceptance set passed; a term can pass byte-comparison while
looking wrong from any camera the captures never stood at.

**Validation and logs:** `--vkvalidation` is off by default and VUIDs go to
stderr, not `engine.log` — `... > val.txt 2>&1` and grep that. A zero count is
only evidence if a known-bad build produces a non-zero one. `--logch 9`
surfaces graphics warnings (missing textures announce themselves there and
nowhere else).
