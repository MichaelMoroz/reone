# Ray culling: keeping grass out of the light path

**Built.** Controls are `--ptgrassscatter` and `--ptgrassshadows`, both
`OptionApply::Live`, both defaulting on. Primary visibility is rasterized in every
mode, **so none of this decides whether grass is drawn — only whether it is in the
light path.**

Grass dominates frame cost when it is on — Dantooine builds ~507,000 cards — and most
of what they cost the tracer buys nothing: a bounce ray crossing them gathers indirect
light the ground already dominates, and the second and later next-event draws re-trace
the same cards for a shadow the first draw resolved.

## The mechanism: TLAS instance masks

`TraceRayInline`'s third argument is an inclusion mask, ANDed with each TLAS instance's
own 8-bit mask. **A zero result skips that instance during traversal, in hardware,
before any candidate is reported.** Grass already rides its own TLAS instances, and the
instance writer already emitted a mask byte — it wrote `0xff` for everything, which is
why the inclusion mask had never selected anything. Merged geometry now takes `0x01`
and a grass card `0x02`. `ptShadowRay` takes the inclusion mask and, in one traversal,
keeps the cards' attenuation separate; `ptDirectLight` caches that grass factor per
light across its `neeSamples` draws, so **grass shadowing costs one traversal through
the cards per distinct light per vertex, and every draw stays identically
distributed.**

**Why the draws must stay identically distributed.** The first version let only the
first N draws see grass and averaged the rest in unchanged. **That is not a variance
reduction, it is a bias:** at one grass draw and four samples, direct light on
grass-shadowed ground measured 0.499 against 0.114 with every draw seeing grass and
0.432 with no grass at all — **the shadow was three quarters gone.** After the cache:
0.114 at one draw, 0.114 at four.

**Scatter rays see grass by default.** Skipping them measured no frame-time change and
let bounce rays see sky through the canopy, raising bounce light on grass-shadowed
ground from 0.102 to 0.131. **Two acceleration structures and two rays are not needed
and would be worse:** a second traversal plus a merge, for what one mask bit expresses.

**Why the obvious version does nothing.** Rejecting grass candidates inside the
`Proceed()` loop has **no measurable effect on frame time, and cannot**: by the time a
candidate is reported the BVH descent and the triangle intersection are already paid,
and the skip saves the alpha fetch. The same lesson was recorded for procedural
sprites and solved by leaving them out of the structure — **but grass cannot be left
out**, since it is real geometry the light path needs for the first shadow ray. The
instance mask is the traversal-level equivalent, **cheaper than exclusion because the
geometry stays in the structure and the decision is per ray.**

Finally, **`reone.cfg` persists whatever the last run had**, so a batch that passed
`--ptgrassscatter 0` leaves that key behind and the next run inherits it over the default —
**pass both flags explicitly in any comparison.**

## What grass actually costs

Measured 2026-08-29 on danm14ab, path tracing, 1920×1080, 1 spp, 2 bounces, denoiser
and AA off, headless; the interval between the frame-300 and frame-2100 captures **of
one run**, one warm-up discarded. Pre-change 11.20 ms/frame; the default 10.64; grass
out of the light path entirely 10.42; grass off 4.22; `--mode retro` 2.91. At
`ptneesamples 4`, 13.74 against 12.16. **The default saves 5%, and 11.5% at four
next-event draws**, while keeping the shadows — frame 2100 differs from the pre-change
frame by 1.25 mean-abs against a run-to-run floor of 1.13, where removing grass
shadowing altogether reads 2.72.

**Traversal is not where grass costs.** An Nsight trace with and without grass put the
+8.3 ms GPU-frame difference almost entirely *before* any ray: the cards drawn into all
nine shadow casters' maps (+3.99, of which 2.99 is the cards), the instance-record pass
clearing and refilling ~507k 64-byte records through four contended atomics (+2.33),
and the TLAS rebuilt over ~507k instances every frame (+2.13). Trace rays itself was
−0.11. **One instance per cluster instead of per card would shrink the last two
together — that is TRC-051.**

Two of the three are since gone. Path tracing no longer schedules the shadow pass at
all — the tracer never read the maps, only the blended tail's `getShadow` did, and it
now returns lit because the traced plan publishes `numShadowLights` as zero. Grass is
drawn into the directional cascades only; **a point light's cube had been rasterizing
every card in the module for a sub-pixel shadow.** Afterwards: 7.64 ms with grass, from
10.64.

Also seen in that trace: **`NRD denoise` runs 1.5 ms with `--ptdenoise 0`** —
`TracingPipeline::render` calls the denoiser unconditionally and the option gates only
whether its output is used.

## Two measurement traps from these runs

**The engine was launched with an argument list in a PowerShell variable named `$args`**
— an automatic variable, so the assignment is shadowed and `-ArgumentList $args` passes
nothing: every run got default options and no `--headless`, which put windows on the
desktop and measured startup. **Echo the resolved command line before launching.** And
**frame time was differenced between a 300- and a 900-frame run with no warm-up** — a
session's first launch pays shader compilation, so the assumption fails and it produced a
**negative per-frame time**, the signature of this mistake. Even with a warm-up, two-run
differencing puts module load on both sides, and load alone varied 12.3-13.7 s between
identical runs, so **take the interval between two captures inside one run.**

Two that follow: **`quit` at the end of a commands file is not a guarantee**, since a
command that throws can stop the file being processed — track the pid and verify the
process is gone — and every scripted run is `--headless 1`, never a window.
