# Ray culling: keeping grass out of the light path

**Status: implemented and measured on danm14ab; not committed.**

Grass dominates frame cost when it is on — Dantooine builds ~507,000 cards. Most
of what those cards cost the tracer buys nothing: a bounce ray crossing them
gathers indirect light the ground underneath already dominates, and the second
and later next-event draws at one shading vertex re-trace the same cards for a
shadow the first draw already resolved.

What is wanted:

- Scatter (non-NEE) rays do not see grass at all.
- Only the first N next-event draws per shading vertex see it. Default 1.
- Both exposed as settings, not constants.

Primary visibility is rasterized in every mode, so none of this decides whether
grass is drawn — only whether it is in the light path.

## The mechanism: TLAS instance masks

`TraceRayInline`'s third argument is an inclusion mask, ANDed with each TLAS
instance's own 8-bit mask. A zero result skips that instance **during
traversal**, in hardware, before any candidate is reported.

Grass already rides its own TLAS instances: `SceneTracingGeometry` lays the
buffer out as one merged record followed by four fixed card regions, so
`instanceIndex != 0` is exactly "this is a grass card". `tracing_instances.slang`
already writes a mask byte per instance — it writes `0xff` for everything, which
is why the inclusion mask has never selected anything.

The change is one bit:

| instance | mask |
|---|---|
| merged scene geometry | `0x01` |
| grass card | `0x02` (a culled card keeps `0`, as now) |

- `ptTraceNearest` passes `0x01`, or `0x03` when scatter rays are allowed grass.
- `ptShadowRay` takes the inclusion mask and, in one traversal, keeps the
  grass cards' attenuation separate from everything else's.
- `ptDirectLight` caches that grass factor per light across its `neeSamples`
  draws: a draw that picks a light already traced skips the grass instances
  and multiplies the factor back in. Grass shadowing costs one traversal
  through the cards per distinct light per vertex, and every draw stays
  identically distributed.

**Why the draws must stay identically distributed.** The first version let
only the first N draws see grass and averaged the rest in unchanged. That is
not a variance reduction, it is a bias: at the default of one grass draw and
four samples, direct light on grass-shadowed ground measured 0.499 against
0.114 with every draw seeing grass and 0.432 with no grass at all - the shadow
was three quarters gone. Same measurement after the cache: 0.114 at one draw,
0.114 at four.

**Scatter rays see grass by default.** Skipping them measured no frame-time
change - with the mask on, traversal through the cards is not where grass
costs - and let bounce rays see sky through the canopy: bounce light on
grass-shadowed ground rose from 0.102 to 0.131. The control stays for
experiments; the default is on.

**Two acceleration structures and two rays are not needed and would be worse:**
a second traversal plus a merge, for what one mask bit already expresses.

## Why the obvious version does nothing

Rejecting grass candidates inside the `Proceed()` loop has **no measurable
effect on frame time**, and cannot: by the time a candidate is reported, the BVH
descent and the triangle intersection are already paid. The skip saves the alpha
fetch and nothing else.

`SceneTracingGeometry` already records the same lesson for procedural sprites —
"rejecting them inside the candidate loop still paid for descending to them,
intersecting them and loading their material first" — and solved it by leaving
them out of the structure. Grass cannot be left out: it is real geometry the
light path needs for the first shadow ray. The instance mask is the
traversal-level equivalent, and is cheaper than exclusion because the geometry
stays in the structure and the decision is per ray.

## Option surface

| written form | type | default | meaning |
|---|---|---|---|
| `ptgrassscatter` | bool | `true` | scatter rays see grass |
| `ptgrassshadows` | bool | `true` | shadow rays see grass, traced once per light per vertex |

Both are `OptionApply::Live`, in the registry, the command line, `reone.cfg`, and
a "Ray culling" section of the path-tracing settings tab. The shader side is
`kPtFlagGrassScatter` (trace flag bit 15) and `kPtFlagGrassShadows` (bit 16).

`reone.cfg` persists whatever the last run had, so a headless batch that passed
`--ptgrassscatter 0` leaves that key behind and the next run without the flag
inherits it over the default. Pass both flags explicitly in any comparison.

## Acceptance

Frame time on a grass-heavy module, path tracing, grass on, with and without
each control — and grass shadows still present under the leaves at the default
of one draw.

Measured 2026-08-29 on danm14ab, path tracing, grass on, 1920x1080, 1 spp,
2 bounces, denoiser and anti-aliasing off, headless. Per-frame cost is the
interval between the frame-300 and frame-2100 captures of one run divided by
1800, so module load is outside it; one warm-up discarded, two samples each:

| configuration | ms/frame |
|---|---|
| pre-change behaviour (scatter sees grass, every draw sees grass) | 11.20 |
| old default (`ptgrassscatter 0`, one grass draw) | 10.64 |
| grass out of the light path entirely | 10.42 |
| pre-change, `ptneesamples 4` | 13.74 |
| default, `ptneesamples 4` | 12.16 |
| grass off (`--grass 0`) | 4.22 |
| `--mode retro`, grass on / off | 2.91 / 2.66 |

Within-group spread is 0.05-0.5 ms. The default saves 5%, and 11.5% at four
next-event draws, where three of the four shadow traversals through the cards
are dropped. Frame 2100 of the default differs from the pre-change frame by
1.25 mean-abs against a run-to-run floor of 1.13; removing grass shadowing
altogether reads 2.72, so the single draw keeps the shadows.

The premise above overstates what traversal costs. Grass adds 7 ms to the
traced frame but only 0.25 ms to the raster one, and taking it out of every
ray recovers 0.8 ms. An Nsight Graphics GPU Trace (`--vkdebuglabels 1`, clocks
locked to base, so times run ~1.4x wall clock) of one frame with and without
grass says where the rest is:

| pass | grass on | grass off | grass costs |
|---|---:|---:|---:|
| shadowPass, of which `grass cards` x9 casters | 5.59 | 1.60 | +3.99 (cards 2.99) |
| GpuScene::update (merge + card generation) | 0.84 | 0.07 | +0.77 |
| tracing instances (record compute, excluding AS build) | 2.34 | 0.01 | +2.33 |
| AS build: TLAS | 2.16 | 0.03 | +2.13 |
| AS build: BLAS (merged, + card templates) | 0.92 | 0.67 | +0.25 |
| geometryPass | 0.20 | 0.12 | +0.08 |
| trace rays | 2.29 | 2.40 | -0.11 |
| GPU frame | 15.5 | 7.2 | +8.3 |

Traversal costs nothing with the mask on; every millisecond grass costs is
spent before a ray is cast. Three items, in order of size: the cards are drawn
into all nine shadow casters' maps, cascades and cube faces alike (3 ms); the
instance-record pass clears and refills ~507k 64-byte records each frame
through four contended atomics (2.3 ms); and the TLAS is rebuilt over ~507k
instances every frame with `PREFER_FAST_TRACE` (2.1 ms). One instance per
cluster instead of per card would shrink the last two together.

Two of those are gone. Path tracing no longer schedules the shadow pass at
all - the tracer never read the maps, only the blended tail's `getShadow` did,
and it now returns lit because the traced plan publishes `numShadowLights` as
zero. Grass is drawn into the directional cascades only; a point light's cube
rasterized every card in the module for a sub-pixel shadow. Same measurement
afterwards: the traced frame with grass is 7.64 ms, from 10.64; without grass
3.34, from 4.22. Nsight puts the remaining grass cost in the instance-record
pass and the TLAS build.

Also seen in the trace: `NRD denoise` runs 1.5 ms with `--ptdenoise 0` -
`TracingPipeline::render` calls the denoiser unconditionally and the option
gates only whether its output is used.

The first measurement attempt was invalid twice over, and both traps are worth
avoiding again:

- The engine was launched with an argument list built in a PowerShell variable
  named `$args`. That is an automatic variable, so the assignment is shadowed and
  `-ArgumentList $args` passes nothing: every run got default options and no
  `--headless`, which put windows on the desktop and measured startup. Use
  `[string[]]$engineArgs` and echo the resolved command line before launching.
- Frame time was differenced between a 300- and a 900-frame run with no warm-up.
  A session's first launch pays shader compilation, so the differencing
  assumption fails; it produced a negative per-frame time, which is the
  signature of this mistake. Even with a warm-up, two-run differencing puts
  module load on both sides, and load alone varied 12.3-13.7 s between
  identical runs: ±1 ms/frame of noise, which is why the interval is taken
  between two captures inside one run instead.

Two more that follow from the same runs: `quit` at the end of a commands file is
not a guarantee — a command that throws can stop the file being processed, so
track the pid and verify the process is gone — and every scripted run is
`--headless 1`, never a window.
