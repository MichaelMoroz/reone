---
name: reone-diagnostics
description: Measure and compare reone empirically without fooling yourself. Covers the deterministic screenshot harness for A/B, the synthetic testbed and free-camera commands for isolating one class of geometry, numeric render-target dumps for localising a difference to a pass, the frozen-scene sequence capture for scoring temporal filters, scripted RenderDoc capture, frame-time measurement, and the build and tooling traps that silently invalidate all of the above. Use when a shader renders wrongly, when a renderer disagrees with another, when a denoiser or TAA is not converging, when something got slower, when you need a scene simple enough to judge by eye, or when you need bound buffers and uniform contents rather than a guess. Triggers on: shader renders wrong, geometry missing, grass, particles, smoke, emitter, testbed, isolation fixture, synthetic scene, free camera, camstatus, spawn, reproducible viewpoint, compare renderers, A/B, frame capture, RenderDoc, uniform buffer contents, G-buffer, frame time, regression, slower, benchmark, validation layers, stale build, path tracing, denoiser, NRD, TAA, ghosting, shimmer, noise, does not converge, flicker, temporal.
---

# Measuring a reone frame

Guessing at shader faults from the rendered image is slow and gets it wrong.
Five things make it empirical: an unattended screenshot harness for A/B
comparison, a synthetic testbed and a scriptable camera so the image is simple
enough to judge at all, numeric target dumps for localising a difference to a
pass, a frozen-scene sequence capture for anything temporal, and a scripted
RenderDoc capture for seeing what the GPU actually received.

**Most of the time lost here has gone to measurements that were quietly
invalid** - a stale binary, a splash-screen frame, two different renderers
compared as if they were two backends, validation left on. The traps at the end
are not trivia; read them before trusting a number.

## Screenshot A/B, unattended

The engine can warp somewhere, render, screenshot and exit with no human present:

```
cd build/bin
echo warp danm14ab > warp.txt
engine.exe --game "<GAME_DIR>" \
    --commands-file warp.txt --capture out.tga --captureframe 3
```

- `--commands-file` runs console commands at startup; `warp <module>` is the useful one.
  It runs during init, so the module is loaded before the first frame.
- `--capture <path>` writes a TGA on frame `--captureframe`, then exits.
- **Frames below ~300 are the splash screen.** The logo plays before the module
  is presented, so `--captureframe 3` writes a perfectly valid, perfectly
  deterministic TGA of the splash - identical on both backends, identical
  before and after any renderer change, and therefore evidence of nothing. Two
  captures matching at a low frame number is the expected result whether the
  change is correct or catastrophic. **Use `--captureframe 900`**, as the
  `--dumptargets` examples below do, and look at the image before trusting a
  comparison built on it.
### Capture runs are deterministic, and that is load-bearing

### Always pass `--dev 0`, or the FPS counter forges a regression

**Raster captures are byte-identical — but only with the debug UI off.** With
developer mode on, the editor draws a live frame-time readout into the top-right
of the frame (`editor.cpp:1583-1588`), and it lands *inside* the captured TGA:

```
run 1:  234.4 FPS  4.27 ms
run 2:  241.3 FPS  4.14 ms
```

That is ~500 pixels in one 153x13 box, with channel deltas up to 219 — which
reads exactly like a renderer regression. Mask that box, or pass `--dev 0`, and
three runs each of `danm14ab`, `ebo_m12aa` and `danm13`, in **both** PBR and
retro, hash the same. Six groups, no exceptions:

```
engine.exe --game ... --dev 0 --mode raster --pbr 0 \
           --commands-file warp.txt --capture out.tga --captureframe 310
```

`dev` defaults to whatever `build/bin/reone.cfg` says, and that file has
`dev=1`, so **the trap fires by default**. It cost this project four false
regression reports across the OpenGL removal, and an elaborate statistical
comparison method built to tolerate noise that was never there. No code change
is needed — it is a flag.

So the bar for any raster change is **hash equality**, not a tolerance. If two
raster captures differ by a single pixel outside that HUD box, something is
genuinely nondeterministic and that is the bug.

**Path tracing varies per pixel, and that part is genuine.** Measured over three
runs per module with the HUD excluded, `danm14ab` differs by 8.4-13.9% of
pixels, `danm13` by 12.2-13.5%, `ebo_m12aa` by **53-64%**. Driver
acceleration-structure builds are not run-reproducible, so ray-query candidate
*arrival order* varies, and the bounce loop's transparency accumulation and
early-exit are order-dependent by design (the additive-hit cap was too until it
became nearest-8-by-distance).

### But the traced *energy* is deterministic — add `--ptdenoise 0`

Mean luminance over repeated runs of `danm13`, frame 310:

| | sd | range |
|---|---:|---:|
| denoiser and FSR both off | **0.00063** | 0.00176 |
| FSR alone on | 0.00051 | 0.00154 |
| **NRD alone on** | **0.14043** | 0.41438 |
| both on | 0.01283 | 0.03936, and bistable |

**NRD is the whole of it.** FSR contributes nothing and partly *masks* NRD's
excursions, which is why both-on looks tamer than NRD-on. With `--ptdenoise 0`
the traced mean is stable to six decimals, so a traced change can be judged from
a handful of runs instead of sixteen a side.

That is also a real bug and not only a harness nuisance — REBLUR returning a
different result from identical input is temporal instability. It is backlog
7.5; measuring around it is the workaround, not the fix.

So: **compare traced changes with the denoiser off.** Reach for the
distribution machinery below only when the denoised image is itself what is
under test.

### Comparing a traced change: distributions on both sides

Two mistakes here have produced every false failure in this project:

1. **A single run-pair is not a noise floor.** It varies 1.6x on raster and
   8.4→16.3% on tracing. A floor estimated from one pair condemns changes that
   altered nothing.
2. **A stored baseline image is one sample**, not ground truth. Comparing fresh
   runs against it is bounded by wherever that single draw landed. In one case
   every post-change run fell on the same side of the stored image — a 1.6%
   coincidence that looked like a systematic regression and was not.

So capture N times *before* the change and N times *after*, and ask whether the
cross-boundary spread exceeds the within-group spread. One measured example: an
apparent 0.009 luminance shift sat inside the 0.017 scatter of the unmodified
build measured against itself. Stored baselines remain a smoke test for gross
breakage and are not evidence at this precision.

Two path-tracing capture notes: the engine-log trace rates (shadow rays,
lights past cutoff, secondary misses) print only when the **Trace stats**
checkbox in the Path tracing window is on - the GPU counters behind them are
off by default because their atomics once cost 95% of the traced frame (36 ms
of a 38 ms frame; see the plan's light-hierarchy postmortem). Leave them off
for any timing measurement.

Four things buy that, and all four key off the same predicate
(`Engine::isCaptureRun`, true when `--capture` or `--dumptargets` is given):

- **Fixed 1/60 timestep.** Wall-clock timing lands the same frame number on
  different animation state every run.
- **Input is dropped, not dispatched.** One mouse move over the window turns the
  camera and every later frame differs. This really happens: a measurement
  during this work was silently contaminated by someone moving the camera.
- **Focus is ignored.** The loop normally idles when the window is in the
  background; a capture is being measured, not watched.
- **The shared generator is seeded to 0.**

A consequence worth knowing before it looks like a bug: **a capture run does not
play at real speed.** The simulation advances a sixtieth of a second per frame
however long the frame actually took, so it looks fast on a light scene and slow
on a heavy one. That is exactly what makes frame N the same simulated moment
every time, and it does not affect what is captured.

Run it twice with whatever you are comparing - two commits, two settings - then
diff. **There is no `--backend` any more**: OpenGL was deleted in `df1aa375`,
Vulkan is the only backend, and any example below that still passes `--backend`
predates that and will fail to parse. Cross-backend comparison is history; what
remains is comparing a change against the commit before it. TGA here is BGR and
bottom-up:

```python
from PIL import Image, ImageChops
import struct
def load(p):
    d = open(p,'rb').read(); idlen = d[0]
    w,h = struct.unpack_from('<HH', d, 12); bpp = d[16]; desc = d[17]
    img = Image.frombytes('RGB', (w,h), bytes(d[18+idlen:18+idlen+w*h*(bpp//8)]))
    b,g,r = img.split(); img = Image.merge('RGB', (r,g,b))
    if not (desc & 0x20):
        img = img.transpose(Image.FLIP_TOP_BOTTOM)
    return img
```

Amplify the difference (`v*10`) before viewing it, then read the PNG directly -
the difference image localises the fault far better than the two frames do.

## Isolation fixtures: stop judging a shader against a game module

**A game frame is the wrong place to check whether one class of geometry
renders.** `danm14ab` carries a sky, lightmaps, a few hundred objects and no
ground truth, so "does grass cast a shadow" becomes an argument about a
screenshot. It was reported absent twice when the shadows were present and
merely sub-pixel, and neither report could be settled by looking harder. Build a
scene where the answer is unmistakable instead.

### `warp testbed [grass|smoke|smoke-control|none]`

No sky, a flat white plane, one object at the origin, the camera looking at it,
one directional light. It is code-built in `Game::loadTestbed` rather than a
synthetic IFO/ARE/GIT, so it needs no assets and cannot inherit a room, a party,
scripts or the previous graph. A shadow either is in that picture or is not.

Note the command word: it is **`warp testbed`**, not `scene testbed`.
`scene` takes only `empty`.

The `smoke` variant emits `fx_smoke01` — deliberately the same texture the main
menu and the Dantooine vents use, so the fixture exercises the asset actually
under investigation. **An emitter resolves its texture by name through the
registry** (`node/emitter.cpp:255`), so a runtime-generated texture returns null
and the emitter registers *nothing at all* — which reads as "the tracer cannot
see particles" rather than as a missing asset. Do not point a fixture emitter at
a texture you built in memory.

### `scene empty`, and the other scene commands

| command | what it does |
|---|---|
| `scene empty` | tears down module, party and graph; leaves an empty scene and a free camera |
| `spawn <resref> x y z` | UTC or UTP blueprint if one exists, otherwise a bare MDL — the fallback is what reaches renderer-only classes (dangly, saber, emitters, billboards) |
| `grass <surface_model> <grass_texture> x y z` | turns a whole supplied surface into grass at density 64 |
| `emit` | detonates the emitters in the last spawned model |
| `ignite` | plays powerup on the last spawned model — the saber path |

**`scene empty` really is empty, and the sky is not a counter-example.** The sky
is baked at startup and is not an object in the graph, so seeing one does not
mean the command failed.

**Emitters take time to fill and this invalidates early captures.** At frame 310
the Dantooine vents hold 55 particles; at frame 1200 they hold 232. A particle
change measured at 310 is measured against a quarter of the geometry, and the
overlap-dependent behaviour — transmission, self-shadowing, denoiser guides — is
exactly what the missing three quarters would have exercised. Capture late.

### The camera, which is what makes any of it reproducible

| command | what it does |
|---|---|
| `camera free` | switch to the free camera |
| `campos x y z` | put it at a point |
| `camlook x y z` | aim it at a point |
| `camstatus` | **print the `campos`/`camlook` pair for the current viewpoint** |

`camstatus` is the one that matters. Fly to the thing you are investigating by
hand, run it, and paste the two lines it prints into a commands file — the
viewpoint is now reproducible across builds, across commits and inside an
unattended capture. Framing a defect by editing coordinates blind does not work;
this took several rounds of a subject drifting off-screen before it existed.

```
warp danm14ab
camera free
campos 320.481415 106.536171 8.482998
camlook 326.0 118.0 13.0
```

### `--commands-frame N`, because `warp` finishes after the file does

A commands file runs during init by default, and `warp` completes its module
load *after* that — so `camera free` and `campos` in the same file are applied
to a scene that is then replaced. Run the file on a later frame instead:

```
engine.exe --game "<GAME_DIR>" --dev 0 --mode path-tracing --ptdenoise 0 \
    --commands-file "<ABSOLUTE PATH>\cam.txt" --commands-frame 120 \
    --capture out.tga --captureframe 1200
```

The path must be absolute; a relative one resolves against the working
directory, not the file. This is also the missing piece for the per-class
isolation fixtures in `test/fixtures/render-isolation/`, which render nothing
today for exactly this reason (backlog 7.7).

## Scene state capture, for naming the object somebody is pointing at

"The skyscrapers in the background", "that cardboard", "the grass walkmesh" are
not questions a renderer can be asked. Guessing from a screenshot is how three
separate changes in one session got aimed at the wrong geometry: first at every
`backgroundGeometry` mesh, which on Taris is lamps; then at `AdditiveEmissive`,
which is lightsaber blades; then at the sky bake, which those meshes are not in.
All three were confident and all three were wrong.

**Graphics → Advanced → "Capture scene state"**, or the console command
`scenecapture`. Writes `<exe>/capture/state_NNNN/`, numbered, never overwritten.

```
frame.tga            the frame as shown
debug_00..19.tga     the frame rendered with each debug channel
*.npy                every pipeline target, values as stored
objects.tsv          id, kind, model/node, material, classification
records.tsv          object_id and the triangle range it owns
materials.tsv        surface type, feature mask, category, texture ids
scene.txt            module, mode, camera, counts, the live dials
README.txt           the procedure below, written into the folder
```

**Naming a pixel:**

```python
import csv, numpy as np
tri = np.load("g_buffer_triangle_id.npy")[:, :, 0]
recs = list(csv.DictReader(open("records.tsv"), delimiter="\t"))
objs = {r["id"]: r for r in csv.DictReader(open("objects.tsv"), delimiter="\t")}
t = int(tri[y, x])                       # 4294967295 means sky - nothing drawn
for r in recs:
    f, c = int(r["first_triangle"]), int(r["triangle_count"])
    if f <= t < f + c:
        o = objs[r["object_id"]]
        print(o["model"], o["node"], o["classification"], "mat", r["material"])
```

Ask the user for the pixel coordinate. Any image viewer reports it, and the
capture is full resolution, so their number indexes the arrays directly.

**The feature mask is in `g_buffer_lightmap` alpha**, as `value * 255`:
`1` envmap, `2` shadows, `4` fog, `8` lightmap, `16` static, `32` thin. Thin is
set for alpha cutouts, so **it doubles as "this is a cutout"** - which is what
separated three painted skyline cards from 132 modelled buildings that were
otherwise classified identically.

**`g_buffer_self_illum` at 255 means the surface is graded by the emissive
dial.** Read `emissive_intensity` out of `scene.txt` and check the arithmetic
before blaming the shading: on Taris the white skyline was
`albedo^albedoGamma × selfIllum × emissiveIntensity` = `0.235^1.16 × 1 × 3.59`
= 0.68, against 0.67 measured in `traced_noise_free`. That closes a question
that three rounds of reasoning had not.

**Three things about this tool that were wrong on the first attempt**, all of
which looked like success:

- The debug channels are **one per frame**. `renderFrame` refuses to nest, so
  asking for all twenty inside one frame wrote twenty-one identical copies of
  the same picture. The capture is a state machine across frames.
- A button press and a console command both land **outside** the render frame,
  where the renderer has nothing to read back. The capture is deferred to the
  point the engine takes its own `--capture` screenshot.
- Triangle ranges come from `GpuScene::View::primitiveIds`, **not** from the
  object records: `dstTriangleBase` is only filled during the device merge, and
  the ranges already have the opaque/non-opaque offset applied. Dumping the
  records instead produced a table of zeros that resolved nothing.

## Render target dumps, for localising a difference to a pass

A screenshot is the end of a long chain, so when two backends disagree it says
nothing about where. `--dumptargets <dir>` writes every target the scene
pipeline exposes as a `.npy`, on the same frame as the screenshot:

```
engine.exe --dev 0 --pbr 1 --dumptargets out_a --captureframe 900 ...
# rebuild the other commit, then:
engine.exe --dev 0 --pbr 1 --dumptargets out_b --captureframe 900 ...
```

```python
import numpy as np
for n in ["g_buffer_diffuse", "g_buffer_eye_normal", "g_buffer_lightmap",
          "g_buffer_self_illum", "g_buffer_depth", "output"]:
    a = np.load(f"out_vk/{n}.npy").astype(np.float64)
    b = np.load(f"out_gl/{n}.npy").astype(np.float64)
    c = min(a.shape[2], b.shape[2], 3)       # RGB only - see below
    d = np.abs(a[..., :c] - b[..., :c])
    print(f"{n:22s} meanabs={d.mean():8.4f} max={d.max():8.4f}")
```

**Compare RGB, not RGBA.** Alpha in `output` is 255 on both backends, so averaging
it in divides the error by exactly four thirds - every figure quoted during this
work was 25% under until that was noticed. It is consistent, so trends still
held, but the absolute number was wrong. `min(..., 3)` also keeps the
GL-RGB8-vs-Vulkan-RGBA8 mismatch on the normal buffer from mattering.

Values arrive exactly as stored - depth as 32-bit float, motion as float, no
rounding into bytes - because the point is to find small differences.

This is what settled where the OpenGL/Vulkan gap actually was: motion
bit-identical, depth and lightmap effectively so, diffuse and normals within a
couple of levels of 255, and `output` differing by 17%. The geometry pass was right and
the whole discrepancy was in the resolve. Reason about a screenshot only after
the dumps say which pass to look at.

`--dumptargets` works with or without `--capture`: on Vulkan it flushes the
current frame before reading targets back. Only the OpenGL **PBR** pipeline
exposes targets; the retro pipeline exposes none and dumps nothing.

### The path tracer dumps its whole split, not just the image

In `--mode path-tracing` the dump carries every channel behind the assembled
frame: `traced_noise_free`, `traced_albedo`, `traced_normal_roughness`,
`traced_view_z`, `traced_motion`, `traced_diffuse`, `traced_specular`, and
NRD's `denoised_diffuse` / `denoised_specular`, alongside `traced_output`.

That distinction is what separates *the tracer is noisy* from *the denoiser is
not clearing it*, and neither is visible in the final image. It is also the only
way to check a claim about one channel. "Noise-free" contained raw path-tracer
noise for as long as it existed - the bounce loop routed additive-surface
emission there on a comment asserting it was deterministic along the view ray,
when a scattered ray finding a blade plane is a sampling outcome that changes
every frame. The channel bypasses the denoiser by definition, so that noise
reached the screen unfiltered and no amount of NRD tuning could touch it.

### Path tracing makes the harness slow, and frame 900 is usually not needed

Every frame of a capture run renders at full cost, so `--captureframe 900` in
`--mode path-tracing` traces nine hundred frames to keep one. At 32 samples per
pixel that is well over a minute per capture, and iterating on a shader at that
rate is miserable.

**Use the lowest frame past the splash screen for iteration** - around 310.
Frames below ~300 are the logo and are evidence of nothing, but 310 is a settled
scene, deterministic like any other, and roughly three times cheaper than 900.

Frame 900 is only required when comparing against the existing baselines, which
were captured there. Keep it for cross-commit checks; do not pay for it while
tuning a sample count.

## Temporal filters: freeze the world and watch the residual decay

A denoiser or a TAA cannot be judged from one frame. The measurement that works
is to stop the simulation, restart the temporal history, and capture a run of
frames: with nothing in the world moving, whatever still changes between
consecutive frames is exactly the residual the filters have not removed.

```
engine.exe --game "<GAME_DIR>" --dev 0 --pbr 1 --mode path-tracing \
    --headless 1 --commands-file warp.txt \
    --capture <SCRATCH>\seq\f.tga --captureframe 350 --captureframes 51 \
    --freezeframe 350 --pttaablend 0.9
```

- `--freezeframe N` holds the simulation from frame N (`frameTime` becomes 0) and
  restarts every temporal history once. Rendering is untouched: the jitter
  sequence, the tracer's frame index, NRD's accumulation and the TAA history all
  keep advancing over a scene that no longer moves.
- `--captureframes K` writes K consecutive frames as `f_0350.tga`, `f_0351.tga`…
  A count of 1 keeps the path exactly as given, so old baselines still match.
- `--pttaablend` and `--ptdenoise` set the two dials from the command line.

**The pass criterion is geometric decay to a floor, not convergence to zero.**
A blend-factor filter is an exponential moving average: it settles at a small
non-zero residual and stays there, because the jitter cycles and the tracer
reseeds every frame. Scoring it against zero marks a working filter as broken.
Restarting the history at the freeze frame is what makes the *approach*
measurable, and the decay from cold to settled is the evidence:

| | cold step | settled | decay |
|---|---|---|---|
| `--pttaablend 0.9` | 5.57 | 0.88 | 6.3x - accumulating |
| `--pttaablend 0` | 5.34 | 3.14 | 1.7x - flat, no history at all |

Without the restart the sequence is already settled by the first captured frame
and reads as flat in both cases, which says nothing.

**Mask to edges.** A whole-frame mean is dominated by large flat areas that
converge immediately. Take the spatial gradient of the last frame, threshold at
the 97th percentile, and report that subset separately - it ran 7.4 against a
frame mean of 0.95 here, and foliage alone was 3.4x the frame mean.

**Then look at an amplified diff of the last two frames** (`v*20`), max-pooled
rather than box-downscaled so a one-pixel edge survives the resize. The numbers
say how much is left; only the image says *where*, and the answer was entirely
alpha-cutout foliage, character silhouettes and the saber blade, over an almost
black floor. `scripts/analyze.py <dir>` (per-step residual, edge-masked,
verdict), `scripts/channels.py <dirA> <dirB>` (per-channel determinism) and
`scripts/diffimg.py <dir> <out.png> <gain>` (amplified diff, max-pooled) beside
this file do all three.

### Determinism is the sharper test, and jitter has to be off for it

With the scene frozen **and `--taajitter 0`**, every visibility-ray output is a
pure function of the camera and must come back bit-identical between frames:

```
traced_noise_free        0.00000   0.000%  identical
traced_albedo            0.00000   0.000%  identical
traced_normal_roughness  0.00000   0.000%  identical
traced_motion            0.00000   0.000%  identical
traced_diffuse           0.07709  60.845%  VARIES    <- sampled, correct
```

Anything deterministic that *varies* is a bug, located to one channel, with no
image interpretation involved. Leave the jitter on and this test is worthless:
the primary ray lands on a different sub-pixel every frame, so `traced_albedo`
differed on 57% of pixels for entirely legitimate reasons and the real signal
was invisible.

## CPU attribution: Tracy, headless

Since `1c703dde` the engine carries Tracy (v0.13.1, `ENABLE_TRACY`, on-demand
mode — zero cost until a capture attaches, macros compile out when the option
is off). The whole loop is scriptable, no GUI needed:

```
# terminal 1 (or Start-Process): a long-lived engine
engine.exe --game ... --dev 0 --mode raster --pbr 0 --grassdensity 1 \
    --headless 1 --commands-file warp.txt --captureframe 3000 --capture out.tga
# terminal 2, once it is past loading:
tracy-capture.exe -o run.tracy -s 5      # both tools live in build/bin
tracy-csvexport.exe run.tracy > zones.csv
```

`zones.csv` has `name,total_ns,counts,mean_ns,...` per zone; totals are
**inclusive**, so do not sum parents with their children. The four top-level
zones carry the frame-slot names (`input`/`update`/`graphics`/`audio`) so the
log line and Tracy agree. Calibration point, danm14ab retro steady state,
280 fps: `graphics` ≈ 1.9–2.3 ms with `collectInto` ≈ 0.73 and
`SceneAdmission::prepare` ≈ 0.60 inside it.

Two traps this section exists to prevent: the frame-slot log line measured
over a loading-adjacent window reads several times higher than steady state —
a 4.8/5.9 ms reading taken that way was chased as a regression that did not
exist; and there are no GPU zones (skipped deliberately), so GPU time still
comes from the in-engine readouts.

## Frame time, and how to compare two commits

A capture run renders as fast as it can with a fixed 1/60 simulation step, so
**wall-clock time for a fixed frame count is a direct measure of render cost**.
That makes the harness a usable stopwatch without any instrumentation.

Time two runs at different frame counts and difference them, so startup and
module load cancel:

```
per-frame = (t(900 frames) - t(300 frames)) / 600
```

**Discard a warm-up run first.** The first run after a build pays a one-time
shader, pipeline and texture cache cost. Land that inside the 300-frame
baseline and the difference is deflated - this produced a 1.755 ms/frame
reading for a build that actually cost 4.5 ms, which read as a *speedup* from
the commit under test. Take two samples after the warm-up; spread is around 2%.

Numbers from `danm14ab`, OpenGL, `--pbr 1`, for calibration:

| | ms/frame |
|---|---|
| before the registry refactor | 4.51 |
| after it | 5.62 |
| after material flattening recovered part of it | 5.01 |

### Things that make a timing comparison meaningless

- **Validation layers cost 3x.** 5.37 ms becomes 16.88 ms with `--vkvalidation 1`.
  Vulkan and OpenGL are otherwise within noise of each other - 5.37 against
  5.38 - so a "Vulkan is four times slower" result is almost always this.
  Compare with validation off on both sides.
- **Window focus.** Outside a capture run the loop idles when the window is in
  the background, so a live FPS readout depends on focus. Capture runs ignore
  focus deliberately; live and captured numbers are not comparable.
- **The in-engine profiler is already bracketed**, which beats guessing at which
  half moved: `engine.cpp` measures Input, Update, Graphics render and Audio
  render separately, and `Editor::frameTimes` plots them. Log the per-slot means
  and compare those first - it halves the search space before any hypothesis.
- **`checkIdentityStability` fires whenever the Graphics channel is on** and
  only exists after `296a0474`. It sorts ~1500 ids per frame. Instrumentation
  that logs through Graphics and compares across that commit measures itself.

### Attribute cost to something you measured

The failure worth naming: culling moved from once-per-model-per-frame to
once-per-entry-per-pass, roughly 9000 frustum tests where there had been 200.
Caching it removed the calls and changed frame time by **nothing**, because an
AABB-frustum test is tens of nanoseconds. A ratio of call counts is not
evidence; an absolute cost is. Time the thing before optimising it.

## RenderDoc, scripted

Vulkan captures are labelled: each pass is its own region (shadows, opaque
geometry, deferred resolve, transparent geometry, post-processing, 2D), and
images and pipelines are named, so a draw reads
`pbr_model:skinnedVertex/opaqueFragment` rather than a handle.

`renderdoccmd capture` has no option to capture a chosen frame, and triggering by
keypress does not suit an unattended run. The engine therefore calls RenderDoc's
in-application API itself: `--renderdoc 1` triggers a capture on the frame before
the screenshot. `extern/renderdoc_app.h` is vendored from the installation.

```
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture --wait-for-exit \
    --working-dir "<BIN>" --capture-file "<BIN>\name" \
    "<BIN>\engine.exe" --game "<GAME_DIR>" \
    --commands-file warp.txt --capture rdc.tga --captureframe 3 --renderdoc 1
```

Produces `name_frameNNN.rdc`.

## Inspecting a capture without the GUI

`qrenderdoc --python script.py` runs a script in the embedded interpreter. There
is no standalone `renderdoc` Python module in the installation, so this is the
only scripted route.

**End every script with `os._exit(0)`.** Otherwise the main UI opens after the
script and blocks the terminal until someone closes it.

```python
import renderdoc as rd, os, struct
cap = rd.OpenCaptureFile()
cap.OpenFile(r"...\name_frame840.rdc", "rdc", None)
_, ctl = cap.OpenCapture(rd.ReplayOptions(), None)

# find a draw - instanced grass is 256 instances of 6 indices
hit = [None]
def walk(actions):
    for a in actions:
        if hit[0] is None and a.numInstances == 256 and a.numIndices == 6:
            hit[0] = a
        walk(a.children)
walk(ctl.GetRootActions())

ctl.SetFrameEvent(hit[0].eventId, True)
st = ctl.GetPipelineState()
refl = st.GetShaderReflection(rd.ShaderStage.Vertex)
for i, blk in enumerate(refl.constantBlocks):
    cb = st.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
    d = cb.descriptor
    data = ctl.GetBufferData(d.resource, d.byteOffset, 64)
    print(blk.name, blk.fixedBindNumber,
          struct.unpack_from("<16f", bytes(data)))

ctl.Shutdown(); cap.Shutdown()
os._exit(0)
```

API names vary by RenderDoc version and the errors are unhelpful. In 1.x as
installed here: `GetConstantBlock` (not `GetConstantBuffer`/`GetConstantBuffers`),
`ctl.GetGLPipelineState()` (not on `PipeState`), and `GLState` has no
`uniformBuffers` - it uses a descriptor store. When a name fails, dump
`[x for x in dir(obj) if not x.startswith('_')]` and look.

## Validating one pass: measure what it does to its own frame

Comparing a backend's final image against the other backend cannot tell you
whether a single pass is correct, because the pass inherits whatever difference
came before it. Measure the pass against **its own** input instead, on both
backends, and compare the two magnitudes:

```
for each backend: render with the pass off and on, diff those two
```

This is what caught a broken FXAA port. Cross-backend, FXAA "on" differed by
3.27 against 0.91 with it off, which is ambiguous - a high-pass filter
amplifying an existing difference looks the same. Within each backend the
answer was immediate:

```
             changed its own frame by    pixels touched
  opengl     0.5140                      9.29%
  vulkan     2.6313                      10.79%
```

The same *share* of pixels touched, so edge detection agreed; five times the
magnitude, so the blend distance was wrong. That localised it to the span
length in a few minutes, where the cross-backend number had been argued about
for an hour. Two ratios worth computing separately: how many pixels a pass
touches, and how far it moves them.

## Traps that cost real time here

- **Check the feature is switched on before debugging why it does not work.**
  `ptTaaBlend` defaults to **0**, which disables the composite's TAA entirely -
  `historyValid` goes false in the shader and the output is the raw jittered
  frame. It was graded to zero deliberately, years of commits ago, while mip-0
  aliasing made history clamping useless, and nothing since put it back. A
  session went into reading the reprojection maths for a filter that was never
  running. The tell was in the numbers before it was in the code: the
  frame-to-frame residual repeated with **period 8**, exactly `kJitterPhases`,
  which means the output was a pure function of the jitter phase and no history
  was being mixed in at all. A periodic residual is not noise - it is a filter
  that is not accumulating. Read the default in `GraphicsOptions` and log the
  effective value before forming any hypothesis about the shader.
- **Vulkan readback before submission returns the previous frame.** The target
  images still contain frame N-1 while frame N is only recorded, so a dump can
  look correct wherever the scene is static while every moving thing is one
  frame out. Flush the frame before starting a separate readback command buffer.
- **Validation is off unless you ask for it, and it does not go to the log.**
  `--vkvalidation` defaults to false, so grepping `engine.log` for VUIDs
  without it always returns zero - which reads exactly like "no errors" and was
  reported as such several times during this work. The messages also go to the
  debug messenger on stderr, not into `engine.log`, so redirecting stdout to
  `/dev/null` hides them even when the layers are on. Run
  `--vkvalidation 1 ... > val.txt 2>&1` and grep that. Sanity-check the
  mechanism once by confirming a known-bad build does print something; a count
  of zero is only evidence if a non-zero count was reachable.
- **Channel order is not uniform across targets.** The Vulkan `output` image
  carries the swapchain format, `B8G8R8A8_UNORM`, while every G-buffer target
  is RGBA. `--dumptargets` now swizzles the output to RGBA on the way out so
  every `.npy` is one order, but if a new target is added in a BGRA format,
  add it to `isBGRA` in `dumpTargets` too. Comparing BGR against RGB once
  turned a real 0.91 difference into an apparent 11.67 and produced a
  confident report of a colour cast that did not exist - blue ground where
  OpenGL had brown was entirely the analysis, not the renderer. If a diff
  suggests a *hue* shift rather than a brightness one, test
  `np.abs(a[..., ::-1] - b)` before believing it.
- **ImGui persists panel layout, so a changed default does nothing.** Table
  column widths, window sizes and dock positions are saved to
  `build/bin/imgui.ini` and win over the values in `TableSetupColumn` and
  `SetNextWindowSize`, which apply only the first time a widget is seen. Edit a
  width, rebuild, and the panel looks exactly as before - which reads as the
  build not having taken. Delete `imgui.ini` to see what a first run actually
  shows. Related: the same panel is worth capturing at two dock widths, because
  a stretch column that looks fine at 800px can collapse to nothing at 480 while
  every fixed column keeps its size.
- **Two build trees, and the one you want is not the default.** `cmake --build
  build --config Release` writes `build/bin`; `--config Debug` writes
  `build/debug/bin`. Every capture harness path in this file assumes
  `build/bin`, so building Debug and then running `build/bin/engine.exe` runs
  whatever was there before - silently, with a plausible-looking result. Five
  consecutive runs during this work "proved" a crash had been fixed and that an
  entire code path never executed; all five were a binary from before the patch
  was applied. **Check `ls -la build/bin/engine.exe` against the clock** before
  believing any run that contradicts what you expected.
- **A segfault with no validation errors is usually teardown, not rendering.**
  Look at whether the screenshot was written first: if it was, the frame is
  fine and the fault is on the way out. Bisect it by logging between the steps
  of `VulkanRenderer::deinit` - but note the log is buffered, so the last line
  printed is a hint, not the answer.
  **Then run the Debug build, which links a checked VMA** and asserts
  "Some allocations were not freed before destruction of this memory block!"
  That names the bug class immediately. The cause here was a `unique_ptr<VulkanImage>`
  member added to `init()` but not released in `deinit()`: the member destructor
  then ran after `VulkanDevice::deinit` had already destroyed the allocator.
  Any object owning a VMA allocation and outliving the device must be reset in
  an explicit `deinit`, never left to its destructor.
- **Stale shader modules.** Building *any* named target - `--target engine`,
  `--target vulkanprobe` - skips the SPIR-V transpile. Separately, the
  transpile rule used to depend only on the top-level `.slang` file, so editing
  anything under `slang/lib/` left every `.spv` stale while the build reported
  success. That is fixed - the rule now globs all of `slang/` - but the failure
  mode is worth knowing, because it is silent and the measurements that follow
  look real: a hash fix was measured as making parity *worse* when in fact only
  the OpenGL half of it had been compiled. If a change should affect both
  backends, confirm both actually moved before interpreting the direction.
  This has now cost three separate investigations: three debugging probes against a module older than
  the edit, and later a texture that sampled as flat white because the sample
  was not in the compiled module at all. Build the default target, or the
  `compile_spirv` target explicitly.
  **When a shader edit seems not to take effect, disassemble the module first**
  (`spirv-dis x.spv | grep Decorate`) and confirm the thing you just wrote is
  actually in there. It is a five-second check that beats an hour of suspecting
  descriptors.
- **A broken `toolkit` trains you into the habit that hides everything else.**
  The default `cmake --build build --config Release` fails at the end on
  `toolkit.exe`, on pre-existing unresolved `ImGui_ImplVulkan_AddTexture` /
  `RemoveTexture` symbols that nobody is fixing. `engine.exe` has already
  linked by then, so the run is usable and the failure reads as known noise.
  The natural response is `--target engine` - and that skips the SPIR-V
  transpile above **and** the `tests` target.

  That is how `tests` stayed broken across four commits without anyone
  noticing: a full build failing looked exactly like the toolkit failure
  everyone had learned to ignore, and no named-target build ever compiled the
  suite. **Build `--target tests` explicitly and run `build/bin/tests.exe`
  before believing a change is clean** - 350 tests take under a second, and a
  binary that links is not the same claim as a suite that passes.

  The signature when it happens: `error C2259: cannot instantiate abstract
  class` on a `NiceMock<...>`, followed by a cascade of unrelated-looking
  `ReturnRef` / `make_shared` / `WillByDefault` errors that name the wrong
  function entirely. It means an interface gained a pure virtual and the mock
  in `test/fixtures/` did not. **Read the C2259 notes** - MSVC lists each
  unimplemented member and the header line it came from. Grepping the interface
  for `= 0` and diffing against `MOCK_METHOD` misses multi-line declarations and
  sends you after the wrong symbol.
- **Removed OpenGL Slang path.** OpenGL once ran Slang SPIR-V modules, but its
  missing Vulkan draw-parameter builtins silently dropped instanced geometry.
  Measurements made through that path are invalid.
- **Probing the G-buffer.** Writing a marker colour to `SV_Target0` in a
  deferred pass does not put that colour on screen - it goes through lighting.
  Write to the self-illumination target instead, which is added directly.
- **Detecting the marker.** Test hue (`r > g*1.3 and b > g*1.3`), not absolute
  brightness; lighting scales the value down.
- **Confirm the flag works.** A comparison flag that silently stopped being
  applied made two builds look identical for the wrong reason. Log the active
  state at startup and check it in the capture log.
- **Check the screenshot contains what you think.** `glReadPixels` samples
  whatever is bound as `GL_READ_FRAMEBUFFER`. Until this was fixed, every
  capture read an offscreen scene target: the 3D scene appeared but the entire
  2D layer - HUD, minimap, cursor, main menu - was missing, and the frames still
  looked plausible enough to reason about. Look at the image and confirm the
  parts you care about are in it before diffing.
- **Match the settings, not just the build.** The engine reads `reone.cfg` from
  its working directory. A second build tree without one silently runs a
  different resolution *and* a different pipeline (`pbr=0` vs the default), so
  92% of the frame differs for reasons that have nothing to do with the change.
  Copy the cfg into the reference bin.
- **`reone.cfg` wins every flag you do not pass, and anyone can have edited it.**
  It is untracked, it lives in `build/bin`, `git stash` and `git checkout` do not
  touch it, and both the launcher and anyone testing a mode will write to it.
  A capture that omits `--mode` is not "the default mode", it is whatever that
  file last said.

  This has now cost time twice in one session, in both directions. A leftover
  `mode=path-tracing` made **OpenGL refuse to start** - exit 3, no window, no
  message that survived the buffer. Later the same key made five consecutive
  Vulkan "raster" captures come back **two million pixels** different from their
  baseline, because they were path-traced frames compared against a rasterised
  one. Both times the symptom looked exactly like a code regression, and the
  second time an agent's change was blamed for it before the config was checked.

  **Pass the flags you are comparing on, explicitly, every time** - `--mode`,
  `--pbr`, and `--dev 0` - rather than trusting any of them to default. That cuts
  both ways: `reone.cfg` supplies `mode=path-tracing`, `pbr=1` *and* `dev=1`, so
  omitting `--dev 0` silently puts a live FPS counter in every captured image.
  And when a frame differs enormously for no reason the diff can explain, read
  `build/bin/reone.cfg` and check its modification time *before* bisecting
  anything.
- **The mouse cursor is in the capture.** It is drawn at whatever position the
  game holds. Input is dropped during a capture, so it no longer wanders
  mid-run, but it is still in the image and still worth ruling out before
  investigating a handful of sharp pixels.
- **Diff by region, not by whole-frame percentage.** A single number over the
  whole frame hides everything. Comparing regions separately is what exposed a
  real minimap regression while the rest of the frame moved for unrelated
  reasons. This mattered more when runs were noisy; it still matters, because a
  large uniform difference in the sky will drown a small wrong one on a
  character.
- **Compare the same renderer.** `--pbr` selects between two genuinely
  different renderers - PBR deferred and retro - and they are 77% of pixels and
  26 levels of mean luminance apart on the same module. An entire session's
  comparisons were once made across that boundary before a zero-target dump gave
  it away. Pass `--pbr` explicitly on both sides. (Retro *does* work on Vulkan,
  whatever the older notes say: `VulkanRenderPipeline` branches on
  `options.pbr` internally.)
- **Graphics warnings are off by default.** `--logch 9` enables the Graphics
  channel alongside Global. Missing textures, unsupported formats and
  unimplemented render-pass stubs all announce themselves there and nowhere
  else; without it a backend silently substitutes a blank texture and the frame
  merely looks wrong.
- **Nondeterminism, if it returns.** It was an `unordered_set` keyed on a node
  pointer: pointer values differ per process, so iteration order did, which
  reordered emitter updates against the one shared random generator and
  reordered transparent compositing. The tell was *bimodal* difference - two
  clusters rather than a continuum - and the experiment that found it was
  capturing the **main menu**, which was also affected despite having no module,
  no AI and no scripts. If frames stop matching, look for order that depends on
  an address before looking at anything else.
- **Keep renderer randomness out of the shared stream.** SSAO kernels and noise
  textures draw from `renderRandomFloat`, not `randomFloat`, because the OpenGL
  pipeline builds an SSAO kernel and the Vulkan one does not. When they shared a
  generator the two backends began every comparison at different points in the
  sequence, and particles and grass then differed for reasons unrelated to
  rendering - about a third of the measured gap.
