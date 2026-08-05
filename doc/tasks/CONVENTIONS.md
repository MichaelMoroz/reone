# Conventions and traps rescued from the retired Vulkan plans

Extracted 2026-08-05 from three documents that the `doc/tasks/` consolidation
replaces: `doc/vulkan-rt-backend.md`, `doc/vulkan-remaining-plan.md` and
`doc/vulkan-opengl-remaining-difference.md`. Their status blocks, architecture
sections, dependency tables and per-item task rows are obsolete and are
deliberately not carried over — the fourteen-translation-unit inventory, backend
selection, the GL services under Vulkan, and the OpenGL-parity programme all
describe a tree that no longer exists.

What is carried over is the part that stays true when the architecture around it
changes: conventions an implementer violates by accident, traps that were paid
for once in real time, one decision with consequences that are still pending,
and the measurement rules and ruled-out list that would otherwise be
re-derived.

Claims below were spot-checked against the `path-tracing` working tree. Where a
check confirmed the claim the file:line is given; where the claim could not be
confirmed from source alone, that is said inline rather than asserted.

---

## 1. Conventions that must hold

Load-bearing. Breaking one produces a plausible-looking wrong image rather than
an error.

### 1.1 Descriptor sets

Uniforms are **set 0**, textures are **set 1**. Both number their bindings from
zero, so they cannot share a set — a texture declares
`[[vk::binding(n, 1)]]`.

*Current-tree note:* this still holds for the raster and 2D paths
(`slang/vk2d.slang:11`, `slang/sky.slang:7`). The convention has since grown two
more sets rather than being replaced: the tracer puts its scene resources —
output image, TLAS, instance materials, merged vertex/index/material buffers,
the bindless texture and texture-array arrays, the sky cube — in **set 1**
(`slang/tracing/resources.slang:44-62`), and its outputs in **set 2**
(`slang/tracing/outputs.slang:22-42`). Compute modules that take no uniform
block use set 0 for their storage buffers instead (`slang/skin.slang:9-12`). The
rule to carry forward is therefore *set index is part of the shader's contract
with the C++ side and is never left implicit*, with 0/1 the raster default.

### 1.2 Uniform block bindings

The ten uniform blocks are pinned by binding index in `slang/uniforms.slang` and
mirrored by `include/reone/graphics/uniformlayout.generated.h`, which asserts
every offset and size at compile time. **That header caught a real std140 bug.
Regenerate it; never edit it.**

Verified: the bindings are 0 `GlobalUniforms`, 1 `LocalUniforms`, 2
`BoneUniforms`, 3 `DanglyUniforms`, 4 `AABBUniforms`, 5 `ParticleUniforms`, 6
`GrassUniforms`, 7 `WalkmeshUniforms`, 8 `TextUniforms`, 9
`ScreenEffectUniforms` (`slang/uniforms.slang:159-168`), and the C++ side lists
their sizes in the same order (`src/libs/graphics/vulkan/descriptors.cpp:73`).
Regeneration is the opt-in `uniformlayout` CMake target, which runs `slangc`
reflection over `slang/uniformreflect.slang` and then `uniformgen`
(`src/apps/uniformgen/CMakeLists.txt`); the generated header is committed, so
only someone changing a block needs to run it.

### 1.3 Vertex attribute locations

Declared in **three places that must agree**:

1. `Mesh::VertexLayout` (`include/reone/graphics/mesh.h`),
2. the Slang `[[vk::location(n)]]`,
3. `VulkanMesh::attributeDescriptions` (`src/libs/graphics/vulkan/mesh.cpp:44`).

Two quirks carried from the OpenGL layout and still live:

- **`offTanSpace` covers three consecutive `vec3`s**, in the order bitangent,
  tangent, tangent-space normal.
- **Bone indices are stored as floats**, not integers.

A location absent from a given mesh is filled with zeros rather than omitted,
because omitting it would leave the pipeline's declared input unsupplied — see
§2.8 (`src/libs/graphics/vulkan/mesh.cpp:48-64`).

### 1.4 Clip space

Vulkan depth is **0..1**. Use the GLM `_ZO` variants — `perspectiveRH_ZO`,
`orthoRH_ZO` — and **never** define `GLM_FORCE_DEPTH_ZERO_TO_ONE`; when the
OpenGL backend still existed that macro would have changed it too, and the
per-call-site form remains the right habit because it keeps the convention
visible where the matrix is built.

Clip-space **y points down**, so a screen-space orthographic projection passes
`(0, w, 0, h)`, not the OpenGL `(0, w, h, 0)`.

Verified at `src/libs/graphics/vulkan/renderer2d.cpp:57-62`,
`src/libs/scene/graph.cpp:765,804-811,858`,
`include/reone/graphics/camera/{orthographic,perspective}.h`.

### 1.5 Texture v orientation

The same texture bytes are read bottom-up by OpenGL and top-down by Vulkan, and
the quad mesh pairs position `(0,0)` with uv `(0,1)`
(`src/libs/graphics/meshregistry.cpp:56-59`). **The 2D vertex shader therefore
flips v unconditionally.**

The consequence to remember: an image that is *already* the right way up must
have that flip cancelled. `Vulkan2DRenderer` composes the flip's own inverse
ahead of the caller's uv matrix for externally registered textures — render
targets crossing the seam — via `cancelVFlip`
(`src/libs/graphics/vulkan/renderer2d.cpp:126-153,168-171`). Anything new that
draws a render target through the 2D path has to make the same choice
explicitly.

### 1.6 Screenshot row order

**Screenshots must match the window.** `captureFrame` reverses rows, because a
Vulkan image copy yields them top-down while `glReadPixels` yielded them
bottom-up, and everything downstream — `TgaWriter`, the comparison harness —
assumes the latter order. It also swizzles the swapchain's B8G8R8A8 into the
RGB8 `Texture` wants. Verified at
`src/libs/graphics/vulkan/renderer.cpp:399-415`.

This convention outlived the OpenGL backend that set it: the tooling, not the
API, is what fixes the order now.

### 1.7 `captureFrame` before `endFrame`

A presented swapchain image has undefined contents, so the readback must happen
while the frame is still open. `captureFrame` throws if no frame is begun
(`src/libs/graphics/vulkan/renderer.cpp:360`) and flushes the frame itself
mid-recording; that mid-frame flush consumes the acquire semaphore, which the
renderer tracks explicitly (`_imageAvailableConsumed`,
`include/reone/graphics/vulkan/renderer.h:176`). Anything else that wants a
mid-frame readback inherits that interaction.

---

## 2. Traps already paid for

Recorded so they are not paid for twice. Every one cost real time. Each is
stated with its symptom, because the symptom is what you will actually be
looking at.

### 2.1 VMA needs its dynamic function path with volk

*Symptom:* a crash inside `vmaCreateAllocator`, before any allocation, with no
diagnostic at all.

With both the static and dynamic function macros off, VMA calls through null
pointers. The macros belong in **CMake**, not in one translation unit, so that
every unit agrees with the one that defines the implementation. Still in place:
`VMA_STATIC_VULKAN_FUNCTIONS=0` / `VMA_DYNAMIC_VULKAN_FUNCTIONS=1` at
`src/libs/graphics/vulkan/CMakeLists.txt:90-91`.

This is the head of a family, not an isolated bug: any Vulkan library that
resolves its own entry points into a process where volk owns them is the same
hazard. It is why NRD was integrated by manual dispatch rather than through NRI,
and it is the first thing to check when a new vendored Vulkan dependency
crashes silently at startup.

### 2.2 The present semaphore is per swapchain image, not per frame in flight

*Symptom:* validation complains, or a wait blocks, on a semaphore that appears
to have been signalled.

Presentation consumes the semaphore against a particular *image* and there is no
signal that it has done so, so a per-frame semaphore can be reused while a
present is still holding it. Confirmed still structured this way, with the
reasoning recorded at the declaration:
`include/reone/graphics/vulkan/renderer.h:186-191`, sized by
`_swapchain.imageCount()` and indexed by `_imageIndex`
(`src/libs/graphics/vulkan/renderer.cpp:180-181,498,517`).

### 2.3 Dynamic rendering does not transition attachments

*Symptom:* the validation layer reports a layout mismatch on the first use of a
freshly created image.

Images created `UNDEFINED` need **one explicit transition** before the first
pass declares a layout they are not already in. There is no render pass object
to do it for you.

### 2.4 One blend state per attachment is mandatory

Even when every attachment's blend state is identical, the array must have one
entry per attachment. A short array is an error, not a broadcast.

### 2.5 Image layout belongs to the object, not the caller

Whether an attachment is currently readable depends on what the previous frame
did, which the caller cannot know. The G-buffer therefore tracks its own layout.
Any generalised target set must do the same. The dump path relies on this: it
reads back each target at `entry.layout`, the layout the target itself reports
(`src/libs/graphics/vulkan/scenepipeline.cpp:1101`).

### 2.6 A descriptor set bound to a recording command buffer cannot be rewritten

*Symptom:* a flood of validation errors from a single `vkUpdateDescriptorSets`
— 81 of them, in the original incident.

Pointing one shared texture set at a different image per draw invalidates the
command buffer that already bound it. The workaround was per-frame, per-texture
sets; **descriptor indexing is the real fix**, and the tracer already uses it
(`bindlessTextures[]` / `bindlessTextureArrays[]`,
`slang/tracing/resources.slang:55-58`). Anything still allocating a set per
draw should be moved rather than made to work.

### 2.7 A descriptor's image view type must match the shader's declaration

`Sampler2DArray` and `SamplerCube` units need array and cube views
respectively. Supplying a 2D view is an **error, not a coercion** — the driver
will not reinterpret it for you.

### 2.8 A vertex input the pipeline does not supply is an error, not a default

There is no implicit zero for an attribute the pipeline declares and the mesh
lacks. This is why `attributeDescriptions` explicitly binds absent attributes to
read zeros instead of dropping the location.

### 2.9 KotOR textures are DXT and upload compressed

Nearly every texture the game ships is DXT, so this is the common path rather
than a special case. `DXT1 → VK_FORMAT_BC1_RGBA_UNORM_BLOCK`,
`DXT5 → VK_FORMAT_BC3_UNORM_BLOCK`, uploaded unchanged — no decode
(`src/libs/graphics/vulkan/resources.cpp:153-162`). See §5.3 for the DXT1
three-colour-block semantics question, which was tested and closed.

### 2.10 Empty uniform blocks hide failures

*Symptom:* one geometry variant draws nothing while the others look fine, and
the pipeline appears healthy.

Three of the four model variants render correctly with zeroed uniform blocks.
`danglyVertex` takes its position **wholly** from `DanglyUniforms`
(`slang/pbr_model.slang:101`), so with a zeroed block it silently vanishes. **A
variant rendering nothing is not necessarily a broken pipeline** — check whether
its block is being filled before suspecting the pipeline, and conversely do not
take "the other three work" as evidence that uniform upload is correct.

### 2.11 OBSOLETE — "building a named target skips `compile_spirv`"

**This trap no longer applies and must not be reproduced as live advice.** It
cost two separate investigations when it was real, so it is recorded here only
so that older documents quoting it can be recognised as stale.

Shaders are no longer built. Slang is linked into the engine and compiles
`slang/` at startup, cached on disk under a hash of every source file
(`src/libs/graphics/vulkan/shadercompiler.cpp`), and there is a
`recompileshaders` console command. `src/apps/shaderpack` and the `compile_spirv`
step are gone. A shader edit needs no rebuild — restart, or recompile at
runtime. A source error logs and keeps the last good module rather than taking
the frame down.

**The replacement advice:** when a shader edit appears not to take, *check the
log for a compile error* before suspecting descriptors, pipelines or anything
else. The old instruction — disassemble the module and inspect its decorations —
no longer has a `.spv` file to inspect.

### 2.12 A verification path can hide the bug it should catch

*Symptom:* ten teardown validation errors that nobody had ever seen.

They were invisible because every run used `--capture`, whose readback waits on
the queue and therefore left the device idle *by accident*. The instrument was
suppressing the fault it existed to expose. The general lesson: when a class of
bug is never observed, check whether the observation procedure prevents it.

### 2.13 An "intermittent" crash may not be

`imguiHandle` ran on every SDL event with no ImGui context under Vulkan. It
faulted only when an event arrived before the first frame, which looked exactly
like a race and was not one — it was a deterministic ordering bug with a
data-dependent trigger. Before reaching for a threading explanation, ask what
has to have happened *first* for the crash to be possible.

---

## 3. The hybrid primary-visibility decision

**This is a decision, not a description of current behaviour.** Decided
2026-08-01; as of this extraction it has **not landed**. The tracer still traces
camera rays: in primary-ray mode the pipeline allocates only the output target
and returns before G-buffer allocation, and `render()` calls
`callbacks.renderPrimary()` and returns before the raster passes
(`src/libs/graphics/vulkan/scenepipeline.cpp:171-187,770-806`;
`slang/rayquery.slang:126-137` traces the primary hit directly). `AGENTS.md`
states the same, and the staleness audit flagged the rt-backend document's
present-tense phrasing as its most consequential error. Track it as phase-F V1.

### 3.1 The decision

**Raster owns primary visibility.** Raster produces the G-buffer; the tracer
does transport from it. Camera rays are not traced.

Of the traced primary path, **exactly one thing survives: a debug shader that
emits a ray-traced G-buffer, for validation.** If the two renderers describe the
same scene, their G-buffers must match — so the traced one becomes a permanent
instrument for proving that, and the merged-geometry work gains a check that
does not depend on judging an image. Everything else in the traced visibility
walk goes. (The traced-versus-raster G-buffer agreement numbers already serve
this role today; the tracer writes G-buffer-shaped outputs for exactly this
comparison, `slang/tracing/outputs.slang:39-42`.)

### 3.2 What it fixes for free

- **The denoiser guides stop being wrong.** viewZ, normal-roughness, motion and
  the demodulation factors come from raster's opaque pass, so a transmissive
  quad can never write them. That is a defect solved by construction rather than
  by teaching the tracer to skip transmissive hits.
- **Motion vectors become exact.** Raster already computes `prevClipPos` per
  vertex; the traced ones are reconstructed.
- **Depth exists in every mode**, which is what a world-space depth-tested debug
  pass needs and what a traced frame otherwise has to fake from its own view-Z
  output.
- **Cutouts and hashed alpha resolve once**, in the pass that already does them.

### 3.3 What it costs

- **Coverage-as-transmission was a primary-ray model, and the primary ray is
  gone.** Shading a blended surface, weighting by alpha and continuing with
  `1 - alpha` describes a camera ray walking a stack of smoke quads. A raster
  G-buffer is opaque-only, so **transmissive surfaces have to be re-homed**. The
  model itself survives where it is still needed: shadow rays and secondary
  bounces still cross smoke.
- **The black-band defect on menu smoke largely dissolves rather than being
  fixed.** Its truncation half — the primary path capping transmission at twelve
  layers and discarding the background behind them — cannot happen when the
  background *is* the G-buffer. The self-shadowing half is a real lighting
  problem and remains.
- **Raster consuming the shared GPU scene stops being optional.** It was
  originally justified by CPU overhead on weak hardware; under hybrid it is the
  shared primary-visibility path for both renderers, so one geometry pipeline
  becomes a requirement rather than a preference. (This part has since landed
  independently.)

### 3.4 The open question it creates

**How transmissive surfaces are lit once raster composites them.** The decision
removes the mechanism that lit them and does not supply a replacement. This is
the item to carry forward; it is not answered anywhere in the source documents.

Unaffected either way: GUI, text and movies stay on the raster path, none of
which has any business being traced, and the 2D renderer is already
backend-clean, so that part costs nothing.

---

## 4. Measurement rules

These were written for an OpenGL-versus-Vulkan comparison that no longer exists.
The numbers are historical; the rules are not. Each of the four in §4.1 had
already cost this project real time when it was written down.

### 4.1 Pin the flags that change what is sampled

**Pass `--anisofilter 0`.** The original evidence was cross-backend: the
driver's anisotropic filtering differed between its OpenGL and Vulkan paths on
the same GPU, and with it on, one scene read **0.6671** instead of **0.0083** on
the G-buffer diffuse target — the filtering difference was larger than
everything else combined and swamped any measurement made through it. Both
backends requested 16× and clamped identically, so it was not a configuration
mismatch and not something the engine could fix.

*What survives the loss of the comparison:* anisotropy is **baked into the
sampler cache at upload** (§6, tier 3), so it cannot be changed live and cannot
be normalised after the fact; and it is capable of dominating a target-level
difference by two orders of magnitude. Any comparison across binaries, drivers
or machines must pin it explicitly. The option still exists
(`src/apps/engine/optionsparser.cpp:126,238`) and now defaults to `2`
(`include/reone/graphics/options.h:199`), so omitting it does *not* give you
zero. This belongs with the standing rule that every capture flag is passed
explicitly because an untracked `build/bin/reone.cfg` wins anything you omit.

**Compare RGB, not RGBA.** Alpha is 255 everywhere, so including it divides
every figure by exactly four thirds — silently, and consistently enough to look
like a real improvement.

**`--dumptargets` was once a frame stale.** Before the frame-flush fix, a dump
read the image before the frame's command buffer was submitted and returned
frame N−1. Any `.npy` from before that commit is off by one frame. The flag no
longer requires `--capture`. This is retained as a caveat on *archived* dumps,
not on new ones.

**`--slangshaders` no longer exists**, and comparisons made with it on are
worthless. It made OpenGL run Slang SPIR-V, which silently dropped instanced
geometry — grass and hair vanished — so those runs were comparing against an
OpenGL that was not drawing the scene. Retained because the *class* of error
recurs: a toggle that changes what is drawn, not just how, invalidates every
number taken under it.

### 4.2 Validate a pass against its own frame, with the feature off

**Never against another backend, another mode, or a stored baseline from a
different build** — a pass inherits whatever difference preceded it, so a
cross-cut figure measures the accumulated upstream state as much as the pass.

The example that established this: Vulkan's SSAO changed **its own** frame by
**1.8691** over **68%** of pixels, where OpenGL's SSAO changed its own frame by
**0.1106** over **12%**. No cross-backend figure would have separated those; the
cross-backend number for SSAO and SSR *together* was about **0.169/255** of
whole-frame difference, which reads as negligible and concealed both.

Two corollaries from the same episode:

- **Isolate features when measuring.** SSAO and SSR were tested together once
  and the whole result attributed to SSR. Re-isolated: SSAO about **0.113**, SSR
  about **0.068**.
- **A scene-dependent figure is not a verdict.** The frame used had little
  reflective geometry, which is most of why SSR looked small.

### 4.3 Whole-frame numbers are properties of the game state

The clearest demonstration: a merge touching area, creature and player logic —
**no shader or renderer code at all** — shifted the OpenGL output by **0.2949**
while leaving every G-buffer target bit-identical. Opaque geometry was unchanged
and `oit_accum` moved by **0.0030**; the transparency resolve divides
accumulated colour by accumulated weight, so a small change there is amplified
before the filter chain amplifies it again.

**Re-measure the baseline after merging anything.** Comparing a new figure
against one taken before a merge is comparing two different scenes. The
frame-900 baselines quoted in the retired documents (1.2516, 1.2179) are invalid
for this reason and for the stale-dump and `--slangshaders` reasons above; no
replacement figure was ever established, and none is needed now that the
comparison target is gone.

### 4.4 Diff by region against a same-build noise floor

Not by whole-frame percentage. That is what exposed a real minimap regression
hiding inside animation noise. The complementary caution from the same period:
gameplay-frame differences were *bimodal* — pairs either agreed within 1% or
differed across 33% — so **a single A/B pair endorses whatever you hoped for
about half the time.** (The determinism situation has since changed; the current
harness rules in `AGENTS.md` and the diagnostics skill are authoritative. The
durable part is that a single pair is not evidence.)

### 4.5 Dump format and normalisation rules

`--dumptargets <dir>` writes every exposed target as `.npy`. The rules that make
two dumps subtractable:

- **One channel order.** The output target carries the swapchain's BGRA format
  while every G-buffer target is RGBA; the dump swaps BGRA to RGBA so the reader
  does not have to know which target is which. Getting this wrong once turned an
  0.9 difference into an apparent **11.7** and invented a colour cast that was
  not there (`src/libs/graphics/vulkan/scenepipeline.cpp:1104-1113`).
- **One row order.** Cube arrays are unrolled **face after face** — layer 0 +X
  through −Z, then layer 1, and so on — with Vulkan's row order normalised to
  the OpenGL order the tooling expects.
- **One colour space.** NRD consumes its radiance targets in YCoCg; the dump
  converts `traced_diffuse`, `traced_specular`, `denoised_diffuse` and
  `denoised_specular` back to RGB so that every dumped target is in the same
  space (`scenepipeline.cpp:1134-1152`).
- **Depth is published linear, not projective.** `g_buffer_depth` is converted
  from device depth to positive linear view-space distance in world units, using
  the Vulkan `[0,1]` form (`near*far / (far - d*(far-near))`), **not** OpenGL's
  `2*n*f` denominator — matching the traced depth target so the two compare
  directly (`scenepipeline.cpp:1114-1131`).
- **Compressed sources are decoded.** BC1 and BC3 source environment maps are
  decoded to RGBA8 in the dump so their texels compare directly.
- Half-float targets are widened to float32 on the way out.

Two derived rules worth stating separately, because they are what the format
rules are *for*: a target that is not exposed cannot be compared, so exposing a
new target is part of building the pass that writes it; and **one enumeration
should serve both the dump and the render-target viewer** rather than two lists
that must be kept in step — two enumerations that have to agree is exactly how
the duplicated feature masks became a hazard.

### 4.6 Build and run hygiene that outlived its context

- **If `engine.exe` is locked, a previous run is still alive.** The link fails
  and the next measurement silently uses the old binary. Check the binary's
  timestamp against your edit before believing any run.
- The companion rule — "build the `engine` target, never a sublibrary alone,
  because only that target repacks `shaderpack.erf`" — **is obsolete**; see
  §2.11. Shaders are compiled at runtime and there is no shaderpack.

---

## 5. Ruled out with evidence

Each of these was **tested rather than reasoned about**, and each is a dead end.
They are recorded so that a future IBL, DXT or sampler-state investigation does
not start here. The figures are from the OpenGL-comparison era and are not
reproducible now; the conclusions are what matters.

### 5.1 The IBL convolution mathematics

Not the cause. The prefilter pair had the same 1024 samples, the same
`resolution = 512.0`, the same GGX, Hammersley and mip-selection formula. The
one real difference in this area was the irradiance sampling LOD (§5.5), and it
is fixed.

### 5.2 Prefiltered array allocation and LOD mapping

Not the cause. Five mips at 128/64/32/16/8; roughness written as `mip / 4`;
resolved with `roughness * kMaxReflectionLOD`. The one real defect here was
storage precision (§5.4).

### 5.3 DXT1 three-colour-block semantics

**Real, but it does not manifest in this content.** OpenGL used
`GL_COMPRESSED_RGB_S3TC_DXT1_EXT` while Vulkan uses
`VK_FORMAT_BC1_RGBA_UNORM_BLOCK`, and the two differ on three-colour blocks.
Switching Vulkan to `BC1_RGB` produced **bit-identical** output in all three
test scenes. Do not re-open this on suspicion; only a demonstrated
three-colour-block artefact justifies revisiting it. The `BC1_RGBA` mapping is
still what ships (`src/libs/graphics/vulkan/resources.cpp:160`).

### 5.4 Half-float storage for derived IBL arrays — this one was real

Vulkan stored its irradiance and prefiltered cube arrays as
`R16G16B16A16_SFLOAT`, retaining the convolution's fractional results, while
OpenGL rendered into `RGB8` and quantised after each render. The resolve
therefore sampled a different prefiltered value at every non-base roughness
level. Switching to `R8G8B8A8_UNORM` closed it: prefiltered mips 1–4 went to
0.00006 or below, mip 0 to 0.00171.

The durable content: **a derived map's storage precision is part of its
definition, not an implementation detail** — a convolution written into a
higher-precision target is a different filter, visibly so at every level but the
base.

### 5.5 Implicit versus explicit LOD in the irradiance convolution — also real

OpenGL's irradiance convolution used implicit-LOD `texture(...)` lookups, whose
direction gradients choose the source mip. Vulkan forced every lookup to
`SampleLevel(..., 0.0)`. This does not affect prefiltering, which explicitly
chooses its own LOD, but it changed the cosine convolution enough to be visible
on almost every reflective surface. Irradiance difference fell from **0.06120**
to **0.000003** once an implicit `Sample(...)` was used.

The durable content: **for a cosine convolution, the sampler's implicit mip
selection is doing part of the filtering.** Forcing level 0 is not a neutral
choice.

### 5.6 The source cube hypothesis

Tested directly and rejected, twice. Levels 0 through 5 of the active BC1 cube
differed only by one RGB level, spread across their interiors rather than
concentrated at face edges. OpenGL also exposed a final 1×1 level where Vulkan
ends at 2×2, but **adding an equivalent terminal level changed neither the
derived maps nor the frame.**

### 5.7 Missing 2D texture mips

Not the cause for the scene investigated — the relevant environment sources are
cube maps. The 2D upload path nevertheless adopted the rule it was being
compared against: **generate a mip chain only when no authored chain is
present.** That rule stands on its own merits.

### 5.8 Hashed alpha

Not a difference. The integer hash matched line for line between the Slang and
GLSL implementations from the commit that replaced the sine hash onward.
`slang/lib/hashedalpha.slang` is still the single implementation.

### 5.9 Sampler state

Not a difference. Filters, wrap modes, LOD clamps and anisotropy were built from
the same `Texture::Properties` on both sides.

### 5.10 Two small known differences, recorded rather than fixed

- **Resolve alpha.** OpenGL wrote `fragColor` alpha as
  `step(0.0001, mainTexSample.a)`; Vulkan writes `1.0`. Invisible to an RGB
  comparison, but it feeds compositing — so if a compositing artefact ever
  appears at a resolve boundary, this is where to look first. *Not re-verified
  against the current resolve.*
- **Post-processing order in retro.** OpenGL ran FXAA and sharpen before lens
  flares; Vulkan runs flares first, matching its own PBR ordering. Only visible
  with a filter enabled. *Whether this is still the ordering was not checked.*

### 5.11 One fidelity fact worth keeping from the retro comparison

The original environment-map application is **`env * (1 - alpha)`** — that is
what the PBR path's reflection strength was reconstructed from. It came from
`glsl/f_rtr_opaqmodel.glsl`, which no longer exists; `slang/retro_resolve.slang`
now applies a `(1.0 - diffuseSample.a)` term at line 145, which appears to be
the same rule, but the equivalence was not verified term by term for this
extraction.

---

## 6. Runtime settings are not uniformly runtime-changeable

From `vulkan-remaining-plan.md` §2. The specific option list has drifted, but
**the three-tier framing is coherent and is the part worth keeping**: a settings
UI that presents twelve checkboxes behaving in three different ways is lying to
the user.

**Tier 1 — free.** Read per frame; change it and it takes effect next frame.
Originally: FXAA, sharpen, SSAO, SSR, draw distance, TAA jitter.

**Tier 2 — needs targets or the swapchain rebuilt.** Shadow resolution (sizes
shadow targets at init), width and height (size every target), vsync (swapchain
present mode).

**Tier 3 — needs assets or the scene reloaded.** Texture quality (affects what
is loaded), anisotropic filtering (**baked into the sampler cache at upload** —
this is the same fact §4.1 depends on), and render mode selection.

The design consequence:

- tier 1 as live controls;
- tier 2 as controls that trigger an **explicit rebuild**, which the pipeline
  already knows how to do on resize;
- tier 3 either disabled with a note, or offered with a "requires reload" marker
  and no pretence that it applies now.

*Current-tree notes.* An editor settings window exists
(`Editor::graphicsSettings`, `src/apps/engine/editor.cpp:895`) and already tells
the user that some settings take effect after reloading — the tier concept is
partly implemented, not merely proposed. **Grass has left tier 3**: it was the
document's marquee example, gated at area load, and the recommendation was to
move that gate to draw time; grass is now generated on the GPU from integer
hashes and `--grassdensity` is supplied to the merge upload every frame, so it
is tier 1 already. The document's reasoning about *why* it should move is still
the right shape for the next option that turns out to be gated at load time for
no structural reason.

### 6.1 Resolution changes at runtime — still unfinished, as far as could be told

Aspect ratio and the interface's screen centre are read from the options rather
than cached, but **nothing re-applies them**, so changing resolution while a
module is loaded leaves the view stretched and the interface off-centre. Both
need a *trigger*, not just a live value:

- each camera applies its projection from `load`, `deserialize` or
  `updateProjection` depending on the class, and none of those runs again;
- `GUI::_rootOffset` is computed inside `load` and used by every control.

Doing it properly means a virtual on the camera base that the area can call over
its cameras, and lifting the scaling switch out of `GUI::load` so it can be
re-run. **Until then, treat resolution as needing a restart.** *This was not
re-verified against the current tree; the mechanism described is plausible but
the file-level claims date from before several refactors.*

---

## 7. What was deliberately dropped

For the record, so nobody goes looking for it in the consolidated list:

- The fourteen-translation-unit inventory, the two-backend seam description
  (`IRenderer` / `I2DRenderer` with two implementations each), backend selection
  via `--backend`, and the "GL services under Vulkan" section. OpenGL was
  deleted in `df1aa375`; there is one backend.
- All status blocks and dependency tables, including the "not chosen" denoiser
  and "not started" upscaler rows — both landed.
- The per-item task rows from `vulkan-remaining-plan.md`. Four of its six items
  are done: the render-target viewer, the live grass setting, the Vulkan retro
  pipeline, and the registration rework. Bloom (its item 3) and SSAO/SSR (item 4)
  were carried into the consolidated task list on their merits, not as document
  history; the bloom highlight-extraction formula
  `selfIllumed * step(0.95, color) * color` is the one implementation detail
  from that section worth remembering, and the current PBR resolve does not
  appear to write a second highlight attachment.
- The parity numbers themselves. `danm14ab` at 0.005771 and `tar_m02aa` at
  0.001807 described a floor against a renderer that no longer exists; both
  scenes were at the point where two shader compilers cannot be expected to
  round identically, with over 93% of differing pixels within one RGB level.
  They are history, not targets.
- Per-mesh BLAS with refit, from §10.2 — superseded by one scene BLAS rebuilt
  per frame from merged geometry.
- The `RTDebug` render mode as a listed mode. The traced-G-buffer *instrument*
  survives as §3.1 describes; the mode enumeration currently has three entries,
  not four.
