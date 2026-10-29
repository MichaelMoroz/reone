# Conventions, traps, and measurement rules

Things an implementer violates by accident, traps paid for once in real time, and the
rules that make two captures comparable. **Breaking something in §1 produces a
plausible-looking wrong image rather than an error.**

## 1. Conventions that must hold

**Descriptor sets.** Set index is part of the shader's contract with the C++ side and
is never left implicit. Uniforms are set 0, textures set 1, both numbering bindings from
zero so they cannot share a set; the tracer extends this with scene resources in set 1
and outputs in set 2.

**Uniform block bindings.** The ten blocks are pinned by binding index in
`slang/uniforms.slang` and mirrored field by field in `uniforms.h`. **A mismatch is
silent** — the arena slice is the right size and the shader reads the wrong words out of
it. There is **no generated header**; do not look for one and do not add one. The blocks
are checked at **startup** by the same Slang-reflection verifier that checks the scene
tables, exhaustively per field, so swapping two adjacent `float4`s is caught where a
size comparison could not. A mismatch names the field and both offsets — that the engine
can segfault before printing it is TOOL-024.

**Vertex attribute locations** are declared in three places that must agree:
`Mesh::VertexLayout`, the Slang `[[vk::location(n)]]`, and
`VulkanMesh::attributeDescriptions`. The third has almost no traffic left, since scene
geometry is pulled from merged storage buffers by `SV_VertexID`, but **the offsets did
not stop mattering — they moved:** the compute resolve reads the source vertex stream by
the same offsets, so two quirks are now a contract between `Mesh::VertexLayout` and
`scene_resolve.slang` — **`offTanSpace` covers three consecutive `vec3`s** (bitangent,
tangent, tangent-space normal), and **bone indices are stored as floats**. A location
absent from a mesh is filled with zeros rather than omitted.

**Clip space.** Vulkan depth is 0..1. Use the GLM `_ZO` variants and **never** define
`GLM_FORCE_DEPTH_ZERO_TO_ONE`; the per-call-site form keeps the convention visible where
the matrix is built. `glm::project`/`unProject` are a trap — unlike the named builders
they take their convention from that macro, so with it undefined they select `_NO` and
disagree with the engine's matrices; use `projectZO`/`unProjectZO`. Clip-space **y points
down**, so a screen-space orthographic projection passes `(0, w, 0, h)`.

**Texture v orientation and screenshot row order.** The quad mesh pairs position
`(0,0)` with uv `(0,1)`, so **the 2D vertex shader flips v unconditionally**, and an image
already the right way up must have that flip cancelled — `Renderer2D` composes the inverse
*ahead of* the caller's uv matrix, not after, or a caller selecting a sub-rect gets the
mirrored part of the texture. Separately, `captureFrame` reverses rows and swizzles
B8G8R8A8 into RGB8, because everything downstream assumes the bottom-up order
`glReadPixels` produced — **the tooling, not the API, is what fixes the order now.** It
must run **before `endFrame`**, since a presented swapchain image has undefined contents;
it flushes mid-recording, and that flush consumes the acquire semaphore, which the renderer
tracks explicitly.

**Raw texture ids carry their resource epoch.** A bindless id is meaningful only in the
resource generation that assigned it. **Any container keeping one across a frame boundary
must keep the generation beside it, compare `resourceGeneration()` once at the start of
its prepare, and discard and re-lower the whole container when they differ.** Incremental
patch paths are part of the same cache boundary. The check is O(1) per container per
frame and invalidation does the work — **never scan objects to validate ids or repair
entries one at a time**, since render-side CPU is O(changes), not O(objects).

**Per-recording GPU state is immutable.** **Per-frame GPU state visible to more than one
recording must be allocated per recording from a fence-recycled pool or monotonic ring,
never rewritten in place.** Waiting at the next reuse boundary makes the old storage
recyclable; it does not make a descriptor, buffer slice or image view safe to change
after an earlier command captured it. An audit found zero rewrite-in-place instances
remaining — keep it so.

## 2. Traps already paid for

| Symptom | Cause, and what to do |
|---|---|
| Crash inside `vmaCreateAllocator`, no diagnostic | **VMA needs its dynamic function path with volk**, and the macros belong in **CMake**, not one translation unit. **Head of a family:** any Vulkan library resolving its own entry points into a process where volk owns them is the same hazard — why NRD was integrated by manual dispatch rather than NRI, and the first thing to check when a vendored dependency crashes silently at startup |
| A wait blocks on a semaphore that appears signalled | **The present semaphore is per swapchain image, not per frame in flight** — presentation consumes it against an *image* with no signal that it has |
| Layout mismatch on first use of a fresh image | **Dynamic rendering does not transition attachments**; images created `UNDEFINED` need one explicit transition |
| Blend state ignored on some attachments | **One per attachment is mandatory**, even when identical. A short array is an error, not a broadcast |
| An attachment is unexpectedly unreadable | **Image layout belongs to the object, not the caller** — the dump path transitions out of whatever the image says it is in |
| A flood of validation errors from one `vkUpdateDescriptorSets` | **A descriptor set bound to a recording command buffer cannot be rewritten.** Descriptor indexing is the real fix; anything still allocating a set per draw should be moved rather than made to work |
| A sampler reads garbage | **A descriptor's image view type must match the shader's declaration** — a 2D view for a `SamplerCube` is an error, not a coercion |
| A mesh missing an attribute fails to draw | **A vertex input the pipeline does not supply is an error, not a default** |
| One geometry variant draws nothing while the others look fine | **Empty uniform blocks hide failures** — one variant took its position *wholly* from a block that was never filled and silently vanished. **A variant rendering nothing is not necessarily a broken pipeline**, and "the other three work" is not evidence that uniform upload is correct |
| A shader edit appears not to take | **Usually a compile error.** Slang compiles at startup, logs, and keeps the last good module, so *check the log first*. Modules cache under `<temp>/reone/slang-cache`, keyed on a hash of **every** file under `slang/` — which is why editing one shader invalidates the whole cache |
| A class of bug is never observed | **A verification path can hide the bug it should catch** — ten teardown errors stayed invisible because every run used a capture flag whose readback left the device idle *by accident*. **Check whether the observation procedure prevents what it is looking for** |
| An "intermittent" crash | **May not be.** An ImGui handler faulted only when an event arrived before the first frame — a deterministic ordering bug with a data-dependent trigger. **Before reaching for a threading explanation, ask what has to have happened *first*** |
| `0xC0000374` or a hang, two runs in three, at a moving point | **`ComPtr::attach()` takes ownership without adding a reference**, and `loadModule()` returns a module the session also retains. **`attach` is for a reference you already own; a getter handing you a pointer the callee still holds is a borrow, and a borrow needs assignment.** Related: **releasing Slang's global session takes the heap with it**, so it is created once and never released — and *two refcount mistakes on one object produced the same signature*, so **a partially fixed heap corruption is more misleading than an unfixed one** |
| `C1083: Cannot open compiler generated file: ''` | **An `ExternalProject_Add` inherits the outer generator**, here Ninja Multi-Config, and a sub-build that mishandles it emits an unexpanded `${CONFIGURATION}`. Pin such sub-builds to a single-config generator and pass `CMAKE_BUILD_TYPE` and `CMAKE_MAKE_PROGRAM` explicitly |
| `install() given no DESTINATION` on a configure that worked yesterday | **`GNUInstallDirs` must be included unconditionally** — guarding it left Windows relying on a *dependency* including it. **A variable arriving as a side effect of an optional dependency is a build that breaks when an unrelated option is turned off** |
| A ray-query path is unexpectedly unavailable | **Shaders do not handle raw GPU device addresses** — representing one as `uint64_t` in Slang declares the SPIR-V `Int64` capability **even when the shader only copies it** |

Also: **KotOR textures are DXT and upload compressed** — `DXT1 → BC1_RGBA`,
`DXT5 → BC3`, no decode. The common path, not a special case.

## 3. Capture rules

Pass every flag explicitly. **An untracked `build/bin/reone.cfg` is graded away from
defaults and wins anything you omit.**

- **`--dev 0`** and **`--grassdensity 1`**, or the frame-time readout writes text into
  the image and `reone.cfg`'s graded density decides the frame.
- **`--anisofilter 0`** for anything compared across binaries, drivers or machines.
  Anisotropy is baked into the sampler cache at upload, so it cannot be normalised after
  the fact, and it can dominate a target-level difference by two orders of magnitude. It
  defaults to `2`, so omitting it does *not* give you zero.
- **`--antialiasing fxaa`** (or `off`) **on both sides of a cross-mode comparison.**
  Jitter is no longer a dial — `computeJitter` returns zero unless the active AA slot is
  FSR — and **the flag's default is mode-dependent**: FSR in path tracing, FXAA
  elsewhere, so a cross-mode capture that passes nothing jitters the traced side and not
  the raster side, exactly the comparison this rule forbids. Jitter once read a 2.94%
  difference where the truth was 0.67%.
- **Compare RGB, not RGBA.** Alpha is 255 everywhere, so including it divides every
  figure by four thirds — silently, and consistently enough to look like a real
  improvement.
- **Traced output is nondeterministic. Compare distributions, never a stored number**,
  and run `--ptdenoise 0` when the question is the tracer, since NRD is the whole of the
  run-to-run variance.

**Captures are deterministic per binary, not across binaries.** The capture frame counts
from process start, so anything shifting load timing — adding one shader module was
enough — shifts animation phase at a fixed frame. **The upload hash is stable within a
binary and across modes: that equality is the invariant, its absolute value across
commits is not.** And **a capture taken too early proves nothing about the thing it
names** — one measurement appeared to show the free camera never reaching the renderer,
when the captures had frozen before the module finished loading and were comparing two
loading screens.

## 4. Measurement rules

**Look before you measure.** Render the thing and look at it before building a metric.
Every metric built on one particular day either missed the real defect or misled — a sky
continuity check said nothing about buildings baked into the horizon, and a "holes"
triage ranked a correct starfield worst.

**Validate a pass against its own frame, with the feature off** — never against another
mode or a stored baseline from a different build, since a pass inherits whatever
difference preceded it. The episode that established this: one backend's SSAO changed
**its own** frame by 1.8691 over 68% of pixels where the other's changed its own by
0.1106 over 12%, while the cross-backend figure for SSAO and SSR *together* read as
negligible and concealed both. **Isolate features when measuring**, and remember **a
scene-dependent figure is not a verdict.**

**Two numbers differing by less than the spread of either are not a result.** One
unchanged build measured one module across 6.26–7.09 ms over five samples — a 12.2%
spread — so **treat a difference under about 10% on a single module as unproven.**

**Whole-frame numbers are properties of the game state.** A merge touching area,
creature and player logic — no renderer code at all — shifted output by 0.2949 while
leaving every G-buffer target bit-identical, so **re-measure the baseline after merging
anything.** **Diff by region against a same-build noise floor**, not by whole-frame
percentage: gameplay-frame differences were *bimodal*, pairs either agreeing within 1% or
differing across 33%, so **a single A/B pair endorses whatever you hoped for about half
the time.**

**Whatever the merged stream is going to carry must be carrying it before raster reads
it.** Add a class while the tracer is the only consumer and the change is inspectable
against the traced image alone; add it after raster has switched and a difference could
be the new consumer or the missing class.

**Gate work on a consumer.** A step that only widens a seam for a later step should land
with that step — one seam widening came to +191 net lines across 16 files to admit two
extra shadow casters that change zero pixels. **Delete, do not hollow:** the measure is
what *disappears*, not whether the old thing is unreachable.

Other traps that cost time: **a run immediately after a build pays a one-time shader and
pipeline cache cost**, which produced a nonsense 1.755 ms reading once · **`spirv-dis`
aborts on unknown capabilities**, so a disassembly-based check reads a truncated module
and confirms whatever absence it was testing for · **NaN renders as black, white or
garbage depending on the path it takes**, so measure with `numpy.isfinite`, not by eye ·
**a red build trains everyone to ignore red builds** · **a whole-frame statistic cannot
see small localized content**, so use counts · **read the code, not the plan** · **if the
binary is locked, a previous run is still alive**, and since **Release and RelWithDebInfo
share `build/bin`** a timestamp does not say which configuration wrote it · **the
graphics slot measures CPU time recording the frame, not the GPU executing it**
(TOOL-002).

**Dump format.** What makes two `--dumptargets` `.npy` files subtractable: **one channel
order** — the BGRA swap is conditional on the target's own format, and getting this wrong
once turned an 0.9 difference into an apparent **11.7** and invented a colour cast that
was not there; **one row order**, cube arrays unrolling face after face; **one colour
space**, converting NRD's YCoCg radiance back to RGB; **depth published linear, not
projective**; and compressed sources decoded, half-floats widened. Two derived rules are
what the format rules are *for*: a target that is not exposed cannot be compared, **so
exposing a new target is part of building the pass that writes it**; and one enumeration
serves the dump, the viewer and the capture paths rather than two lists that must be kept
in step.

## 5. Ruled out with evidence

Each was **tested rather than reasoned about**, so a future IBL, DXT or sampler-state
investigation does not start here. **Not the cause:** the IBL convolution mathematics
(same sample count, resolution, GGX, Hammersley and mip selection); prefiltered array
allocation and LOD mapping; the source cube hypothesis, rejected twice; sampler state;
and hashed alpha, which was not a difference and no longer exists. **DXT1
three-colour-block semantics** are *real but do not manifest in this content* —
`BC1_RGB` produced bit-identical output in all three test scenes. **Missing 2D texture
mips** were not the cause, but the upload path adopted the rule anyway: **generate a mip
chain only when no authored chain is present.**

Two were **real**, and their lessons generalise. **Storing derived IBL arrays at
half-float** retained the convolution's fractional results where the reference quantised
after each render — **a derived map's storage precision is part of its definition, not an
implementation detail.** And **forcing the irradiance convolution to explicit LOD 0**
changed it visibly, the difference falling from 0.06120 to 0.000003 with an implicit
sample — **for a cosine convolution the sampler's implicit mip selection is doing part of
the filtering.**

One fidelity fact worth keeping: **the original environment-map application is
`env * (1 - alpha)`**, which is what the PBR path's reflection strength was reconstructed
from.

## 6. Runtime settings are not uniformly runtime-changeable

Three tiers, and they are the code rather than a framing —
`OptionApply::Live` / `Reapply` / `Restart`, declared once per option in one table and
read by the editor, the console and the command line alike. **The classification is a
property of the option, not of the place it is edited.** *Live* takes effect next frame;
*Reapply* needs targets or the swapchain rebuilt (width, height, vsync, shadow
resolution, anti-aliasing, render mode); *Restart* needs assets or the scene reloaded
(texture quality, anisotropy, fullscreen, headless).

Two refinements: **a tier can depend on the value, not just the option** — `mode` is
`Live` between the two raster resolves and `Reapply` only when it crosses into or out of
path tracing — and **`Reapply` writes to a staged copy, never to the live options**,
because writing the live struct at set-time would leave the running frame describing
targets that were never allocated; `gfx apply` commits.

**Resolution changes at runtime are still unfinished.** Aspect ratio and the interface's
screen centre are read from the options rather than cached, but **nothing re-applies
them.** `gfx set width … ; gfx apply` **succeeds and reports success** — it resizes the
window, recreates the swapchain and rebuilds every pipeline, and touches neither the
cameras nor the GUI. **The user is told the change applied, which is worse than being
told to restart. Treat a resolution change as needing a restart regardless of what the
console says.** Doing it properly means a virtual on the camera base that the area can
call over its cameras, and lifting the scaling switch out of `GUI::load` so it can be
re-run (TOOL-021).
