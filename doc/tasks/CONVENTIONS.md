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

Re-audited 2026-08-09 against the tree at `8611ae52`. Every `file:line` below
was re-derived at that date, and entries whose subject had ceased to exist were
restated rather than left standing. The historical *figures* in §4 and §5 are
from the OpenGL-comparison era and cannot be re-derived; they are labelled where
they appear.

---


> **The S5 relocation moved the render passes** from
> `src/libs/graphics/vulkan/` to `src/libs/graphics/rendering/`, and much of
> `rayquery.cpp` became `rendering/tracingpipeline.cpp` and
> `vulkan/tracingstructure.cpp`. Coordinates below have been re-derived; a
> `vulkan/` path quoted from an older document is the pre-S5 one. See
> `DESIGN.md`, "S5 as built".

## 1. Conventions that must hold

Load-bearing. Breaking one produces a plausible-looking wrong image rather than
an error.

### 1.1 Descriptor sets

Uniforms are **set 0**, textures are **set 1**. Both number their bindings from
zero, so they cannot share a set — a texture declares
`[[vk::binding(n, 1)]]`.

*Current-tree note:* this still holds for the raster and 2D paths
(`slang/vk2d.slang:27`, `slang/sky.slang:23`). The convention has since grown a
third set rather than being replaced: the tracer puts its scene resources —
output image, TLAS, instance materials, merged vertex/index/material buffers,
the bindless texture and texture-array arrays, the sky cube, and since the
hybrid landed the raster G-buffer it reads its primary from — in **set 1**
(`slang/tracing/resources.slang:59-102`), and its outputs in **set 2**
(`slang/tracing/outputs.slang:34-59`). The compute resolve does the same: it
imports `uniforms` for set 0 and puts all eleven of its storage buffers in set 1
(`slang/scene_resolve.slang:99-111`). The rule to carry forward is therefore
*set index is part of the shader's contract with the C++ side and is never left
implicit*, with 0/1 the default everywhere.

### 1.2 Uniform block bindings

The ten uniform blocks are pinned by binding index in `slang/uniforms.slang` and
mirrored field by field on the C++ side in `include/reone/graphics/uniforms.h`.
**A block's C++ mirror is load-bearing and a mismatch is silent** — the arena
slice is the right size and the shader reads the wrong words out of it.

Verified: the bindings are 0 `GlobalUniforms`, 1 `LocalUniforms`, 2
`BoneUniforms`, 3 `DanglyUniforms`, 4 `AABBUniforms`, 5 `ParticleUniforms`, 6
`GrassUniforms`, 7 `WalkmeshUniforms`, 8 `TextUniforms`, 9
`ScreenEffectUniforms` (`slang/uniforms.slang:176-185`), and the C++ side lists
their sizes in the same order
(`src/libs/graphics/vulkan/descriptors.cpp:70-82`).

*Changed 2026-08-05, `6bf67c3a`.* There is no longer a generated header. The
`uniformlayout` CMake target, `src/apps/uniformgen`, `slang/uniformreflect.slang`
and `uniformlayout.generated.h` are all gone; the blocks are now checked at
**startup** by the same Slang-reflection verifier that already checked the scene
tables, under `LayoutRules::DefaultConstantBuffer`
(`src/libs/graphics/vulkan/shadercompiler.cpp:720`; storage buffers take
`DefaultStructuredBuffer` at `:611`). The trade-off was taken deliberately: a
static_assert failed the build, this fails at launch — and it is exhaustive per
field, so swapping two adjacent `float4`s is caught where a size comparison
could not. The generated header had in fact been missing
`LocalUniforms::iblRoughness` the whole time.

**So: do not go looking for a header to regenerate, and do not add one.** A
layout mismatch reports itself as e.g. "GlobalUniforms::cameraPosition is at 272
in C++, 256 in Slang". That the engine's own startup path can segfault on the
same mismatch before printing it is known and filed as TOOL-024.

### 1.3 Vertex attribute locations

Declared in **three places that must agree**:

1. `Mesh::VertexLayout` (`include/reone/graphics/mesh.h:121-131`),
2. the Slang `[[vk::location(n)]]`,
3. `VulkanMesh::attributeDescriptions` (`src/libs/graphics/vulkan/mesh.cpp:46-88`).

*Current-tree note:* the third place has almost no traffic left. Scene geometry
is pulled from merged storage buffers by `SV_VertexID`
(`slang/scene_draw.slang:239`), so `slang/sky.slang:26-27` is now the only
shader declaring `[[vk::location(n)]]` at all. The layout offsets did not stop
mattering — they moved: the compute resolve reads the source vertex stream by
the same offsets (`slang/scene_resolve.slang:698-702`), so the two quirks below
are now a contract between `Mesh::VertexLayout` and `scene_resolve.slang` as
much as between it and the pipeline.

Two quirks carried from the OpenGL layout and still live:

- **`offTanSpace` covers three consecutive `vec3`s**, in the order bitangent,
  tangent, tangent-space normal (`mesh.cpp:75-81`).
- **Bone indices are stored as floats**, not integers (`mesh.cpp:83-84`,
  `scene_resolve.slang:621-631`).

A location absent from a given mesh is filled with zeros rather than omitted,
because omitting it would leave the pipeline's declared input unsupplied — see
§2.8 (`src/libs/graphics/vulkan/mesh.cpp:50-63`).

### 1.4 Clip space

Vulkan depth is **0..1**. Use the GLM `_ZO` variants — `perspectiveRH_ZO`,
`orthoRH_ZO` — and **never** define `GLM_FORCE_DEPTH_ZERO_TO_ONE`; when the
OpenGL backend still existed that macro would have changed it too, and the
per-call-site form remains the right habit because it keeps the convention
visible where the matrix is built.

Clip-space **y points down**, so a screen-space orthographic projection passes
`(0, w, 0, h)`, not the OpenGL `(0, w, h, 0)`.

Verified at `src/libs/graphics/rendering/renderer2d.cpp:48-59`,
`src/libs/scene/graph.cpp:800,839-846,893`,
`include/reone/graphics/camera/orthographic.h:42`,
`include/reone/graphics/camera/perspective.h:41`. `GLM_FORCE_DEPTH_ZERO_TO_ONE`
appears nowhere in the tree.

### 1.5 Texture v orientation

The same texture bytes are read bottom-up by OpenGL and top-down by Vulkan, and
the quad mesh pairs position `(0,0)` with uv `(0,1)`
(`src/libs/graphics/meshregistry.cpp:55-59`). **The 2D vertex shader therefore
flips v unconditionally.**

The consequence to remember: an image that is *already* the right way up must
have that flip cancelled. `Renderer2D` composes the flip's own inverse
ahead of the caller's uv matrix for externally registered textures — render
targets crossing the seam — via `cancelVFlip`
(`src/libs/graphics/rendering/renderer2d.cpp:122-149,167`). Cancelled *before*
the caller's transform, not after, or a caller selecting a sub-rect gets the
mirrored part of the texture. Anything new that draws a render target through
the 2D path has to make the same choice explicitly.

### 1.6 Screenshot row order

**Screenshots must match the window.** `captureFrame` reverses rows, because a
Vulkan image copy yields them top-down while `glReadPixels` yielded them
bottom-up, and everything downstream — `TgaWriter`, the comparison harness —
assumes the latter order. It also swizzles the swapchain's B8G8R8A8 into the
RGB8 `Texture` wants. Verified at
`src/libs/graphics/vulkan/renderer.cpp:480-495`. (This is the *swapchain*
capture. `--dumptargets` is a separate path with its own channel rule; see
§4.5.)

This convention outlived the OpenGL backend that set it: the tooling, not the
API, is what fixes the order now.

### 1.7 `captureFrame` before `endFrame`

A presented swapchain image has undefined contents, so the readback must happen
while the frame is still open. `captureFrame` throws if no frame is begun
(`src/libs/graphics/vulkan/renderer.cpp:440-443`) and flushes the frame itself
mid-recording; that mid-frame flush consumes the acquire semaphore, which the
renderer tracks explicitly (`_imageAvailableConsumed`,
`include/reone/graphics/vulkan/renderer.h:217`, honoured at
`renderer.cpp:509,544,588,596`). Anything else that wants a
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
`src/libs/graphics/vulkan/CMakeLists.txt:87-88`.

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
`include/reone/graphics/vulkan/renderer.h:224-231`, sized by
`_swapchain.imageCount()` and indexed by `_imageIndex`
(`src/libs/graphics/vulkan/renderer.cpp:277,582,601`).

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
did, which the caller cannot know. Each image therefore tracks its own layout
(`VulkanImage::_layout`, `include/reone/graphics/vulkan/image.h:290`). Any
generalised target set must do the same. The dump path relies on this: it
transitions out of whatever the image says it is in, rather than out of a layout
the caller supplied (`src/libs/graphics/vulkan/image.cpp:1018`).

### 2.6 A descriptor set bound to a recording command buffer cannot be rewritten

*Symptom:* a flood of validation errors from a single `vkUpdateDescriptorSets`
— 81 of them, in the original incident.

Pointing one shared texture set at a different image per draw invalidates the
command buffer that already bound it. The workaround was per-frame, per-texture
sets; **descriptor indexing is the real fix**, and the tracer already uses it
(`bindlessTextures[]` / `bindlessTextureArrays[]`,
`slang/tracing/resources.slang:70,73`). Anything still allocating a set per
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
(`src/libs/graphics/vulkan/resources.cpp:161-170`). See §5.3 for the DXT1
three-colour-block semantics question, which was tested and closed.

### 2.10 Empty uniform blocks hide failures

*Symptom:* one geometry variant draws nothing while the others look fine, and
the pipeline appears healthy.

The original case: three of the four model variants rendered correctly with
zeroed uniform blocks, while `danglyVertex` took its position **wholly** from
`DanglyUniforms` and so silently vanished. **A variant rendering nothing is not
necessarily a broken pipeline** — check whether its block is being filled before
suspecting the pipeline, and conversely do not take "the other three work" as
evidence that uniform upload is correct.

*The specific instance is gone.* `slang/pbr_model.slang` and the four forward
model variants were deleted in `901cee65`; dangly positions now arrive in a
storage buffer (`danglyPositions`, `slang/scene_resolve.slang:612-613`) rather
than a per-draw uniform block. `DanglyUniforms` still exists as binding 3 and
`scene_resolve.slang:64` still defines a `danglyPosition()` that reads it, but
nothing calls it. The trap is kept because the shape recurs wherever an input
defaults to a valid-looking zero.

### 2.11 OBSOLETE — "building a named target skips `compile_spirv`"

**This trap no longer applies and must not be reproduced as live advice.** It
cost two separate investigations when it was real, so it is recorded here only
so that older documents quoting it can be recognised as stale.

Shaders are no longer built. Slang is linked into the engine and compiles
`slang/` at startup, and there is a `recompileshaders` console command
(`src/apps/engine/engine.cpp:273`). `src/apps/shaderpack` and the
`compile_spirv` step are gone. A shader edit needs no rebuild — restart, or
recompile at runtime. A source error logs and keeps the last good module rather
than taking the frame down.

**The replacement advice:** when a shader edit appears not to take, *check the
log for a compile error* before suspecting descriptors, pipelines or anything
else. There is still a `.spv` to disassemble if you need one, but it is not in
the build tree: modules are cached as
`<temp>/reone/slang-cache/<name>-<hash>.spv`, where the hash covers **every**
file under `slang/` (`src/libs/graphics/vulkan/shadercompiler.cpp:238,276-296`).
That whole-tree hash is why editing any one shader invalidates the entire cache,
and why a stale module is not a failure mode here the way a stale `.spv` was.

### 2.12 A verification path can hide the bug it should catch

*Symptom:* ten teardown validation errors that nobody had ever seen.

They were invisible because every run used `--capture`, whose readback waits on
the queue and therefore left the device idle *by accident*. The instrument was
suppressing the fault it existed to expose. The general lesson: when a class of
bug is never observed, check whether the observation procedure prevents it.

### 2.13 An "intermittent" crash may not be

`imguiHandle` ran on every SDL event with no ImGui context under Vulkan
(`src/apps/engine/engine.cpp:96`). It faulted only when an event arrived before
the first frame, which looked exactly like a race and was not one — it was a
deterministic ordering bug with a data-dependent trigger. Before reaching for a
threading explanation, ask what has to have happened *first* for the crash to be
possible.

### 2.14 `Slang::ComPtr::attach()` takes ownership without adding a reference

*Symptom:* the test suite exits at `0xC0000374` or hangs, on roughly two runs in
three, at a point that moves between runs.

`ISession::loadModule()` returns a module **the session also retains**, so
attaching it released a reference this code never held and destroyed the
session's own cache entry underneath it. The corruption was committed long
before anything noticed, which is why the crash site wandered. Use assignment,
which addRefs (`src/libs/graphics/vulkan/shadercompiler.cpp:222-225`, fixed
2026-08-09 in `6d1f79ce`).

The general form, and the reason this is here rather than in a commit message:
`attach` is for a reference you already own. A getter that hands you a pointer
the callee still holds is a **borrow**, and a borrow needs assignment. Twelve
consecutive suite runs pass now; the same loop before the change produced two
heap-corruption exits and a hang.

### 2.15 An `ExternalProject_Add` inherits the outer generator

*Symptom:* `C1083: Cannot open compiler generated file: ''` from a vendored
sub-dependency, under Ninja and not under the Visual Studio generator.

The inherited generator is Ninja **Multi-Config**, and a sub-build that
mishandles multi-config emits an unexpanded `${CONFIGURATION}` into its object
paths. Pin such sub-builds to a single-config generator and pass
`CMAKE_BUILD_TYPE` and `CMAKE_MAKE_PROGRAM` through explicitly — `ninja` is
often reachable only via the outer configure. Worked example: the Tracy tools
block, `CMakeLists.txt:114-142`.

### 2.16 `GNUInstallDirs` must be included unconditionally

*Symptom:* `install() given no DESTINATION`, on a configure that worked
yesterday.

The `install` rules read `CMAKE_INSTALL_BINDIR` and `CMAKE_INSTALL_LIBDIR` and
nothing else defines them. Guarding the include on Linux left Windows configures
relying on a *dependency* including `GNUInstallDirs` for its own reasons — which
Tracy did, so the breakage stayed hidden until `ENABLE_TRACY` defaulted off
(`CMakeLists.txt:414-419`). A variable arriving as a side effect of an optional
dependency is a build that breaks when an unrelated option is turned off.

---

## 3. The hybrid primary-visibility decision

**Decided 2026-08-01. Landed; this now describes current behaviour.** V1b
(`eebcf1d3`, 2026-08-06) made the geometry pass run in path tracing; V1c
(`4980518c`) made the tracer *consume* it. In path-tracing mode the pipeline now
runs `geometryPass`, publishes the G-buffer as sampled, and only then calls
`callbacks.renderPrimary()`
(`src/libs/graphics/rendering/scenepipeline.cpp:1036-1054`).
`slang/path_trace.slang:22` states it in the source: "There is no camera ray."
The primary surface is reconstructed in `slang/tracing/primary.slang` from the
G-buffer's triangle id, and the kernel traces only outwards
(`path_trace.slang:200-216`).

*Sections 3.2 to 3.4 were written as predictions and are kept in that voice;
each is annotated with what actually happened.* Other documents in this folder
still describe V1c as pending — `MASTER.md` TRC-020/TRC-021, `DECISIONS.md`,
`AGENTS.md:76` — and are wrong on that point as of 2026-08-09.

### 3.1 The decision

**Raster owns primary visibility.** Raster produces the G-buffer; the tracer
does transport from it. Camera rays are not traced.

Of the traced primary path, **exactly one thing survives: a debug shader that
emits a ray-traced G-buffer, for validation.** If the two renderers describe the
same scene, their G-buffers must match — so the traced one becomes a permanent
instrument for proving that, and the merged-geometry work gains a check that
does not depend on judging an image. Everything else in the traced visibility
walk goes.

*What happened:* the outputs survive
(`slang/tracing/outputs.slang:50-54`, still labelled "canonical primary-hit
G-buffer for direct raster/tracer comparison") but **the instrument is weaker
than intended, and this is the trap.** Since V1c the tracer derives them from
the raster G-buffer's triangle id rather than from a camera ray, so they no
longer test visibility at all — agreement is guaranteed for the part that used
to be worth checking. What they still test is the material chain and the
encoding: the triangle is re-fetched from merged geometry and its material
re-evaluated (`slang/tracing/primary.slang`). **Do not read a zero difference on
`traced_*` versus `g_buffer_*` as evidence that the two geometry paths agree.**
MASTER's TRC-021 ("pin the traced G-buffer as the RTDebug validation
instrument… must land before V1c deletes it") was overtaken by V1c landing
first.

### 3.2 What it fixes for free

- **The denoiser guides stop being wrong.** viewZ, normal-roughness, motion and
  the demodulation factors come from raster's opaque pass, so a transmissive
  quad can never write them. That is a defect solved by construction rather than
  by teaching the tracer to skip transmissive hits.
- **Motion vectors become exact.** Raster already computes `prevClipPos` per
  vertex; the traced ones are reconstructed. *Held, but not from the G-buffer:*
  raster writes a **screen** displacement and NRD is owed a world-space one, and
  one 2D vector does not determine a 3D one. Inferring it — two screen equations
  plus a tangent-plane constraint — cannot represent movement along the surface
  normal, so anything leaving that plane read as zero and the denoiser threw
  away history it should have kept; **measured, per-frame instability over a
  traced sequence nearly doubled.** The previous world position is instead
  interpolated from the triangle's own `prevPosition`, exactly as a traced hit
  does it (`slang/tracing/primary.slang:267-279`).
- **Depth exists in every mode**, which is what a world-space depth-tested debug
  pass needs and what a traced frame otherwise has to fake from its own view-Z
  output.
- **Cutouts resolve once**, in the pass that already does them. (The prediction
  said "cutouts and hashed alpha"; hashed alpha has since been deleted outright
  — see §5.8.) This one held by construction: the G-buffer's cutout gate is the
  same `candidateAlpha` / `kAlphaTestThreshold` test `tracing/trace.slang`
  applies, over the same merged geometry, so the reconstruction needs no
  special handling for a cutout pixel at all.

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

*What happened to the re-homing:* transmissive surfaces went to raster's
existing blended pass, which now runs in path tracing on the same tail as in the
raster modes (`scenepipeline.cpp:1058-1067`; `blendedPass` at `:501`, depth-test
against the opaque G-buffer, never depth-write, premultiplied blend, no cull).
The tracer stopped traversing blended surfaces, so nothing else draws them.

### 3.4 The open question it creates

**How transmissive surfaces are lit once raster composites them.** The decision
removes the mechanism that lit them and does not supply a replacement.

*Still open.* Where they are drawn is settled — raster's blended pass, above.
How they are *lit* is not: the blended pass currently carries additive layers
(saber blades, glow planes), which need no lighting, and the general lit-
transparency case is still MASTER's RAS-003/RAS-004 (G8) and TRC-019.

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
(`src/apps/engine/optionsparser.cpp:233,453`) and now defaults to `2`
(`include/reone/graphics/options.h:679`), so omitting it does *not* give you
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

- **One channel order.** The dump swaps BGRA to RGBA so the reader does not have
  to know which target is which. Getting this wrong once turned an 0.9
  difference into an apparent **11.7** and invented a colour cast that was not
  there (`src/libs/graphics/rendering/scenepipeline.cpp:1373-1379`). The swap is
  now conditional on the target's own format: the scene output used to inherit
  the swapchain's BGRA in the raster modes and is RGBA float in every mode
  today, so nothing hits it — it is kept for the next target that does.
- **One row order.** Cube arrays are unrolled **face after face** — layer 0 +X
  through −Z, then layer 1, and so on — with Vulkan's row order normalised to
  the OpenGL order the tooling expects (`scenepipeline.cpp:1434-1460`).
- **One colour space.** NRD consumes its radiance targets in YCoCg; the dump
  converts `traced_radiance_diffuse`, `traced_radiance_specular`,
  `denoised_diffuse` and `denoised_specular` back to RGB so that every dumped
  target is in the same space (`scenepipeline.cpp:1403-1425`).
- **Depth is published linear, not projective.** `g_buffer_depth` is converted
  from device depth to positive linear view-space distance in world units, using
  the Vulkan `[0,1]` form (`near*far / (far - d*(far-near))`), **not** OpenGL's
  `2*n*f` denominator — matching the traced depth target so the two compare
  directly (`scenepipeline.cpp:1382-1401`).
- **Compressed sources are decoded.** BC1 and BC3 source environment maps are
  decoded to RGBA8 in the dump so their texels compare directly.
- Half-float targets are widened to float32 on the way out.

Two derived rules worth stating separately, because they are what the format
rules are *for*: a target that is not exposed cannot be compared, so exposing a
new target is part of building the pass that writes it; and **one enumeration
should serve both the dump and the render-target viewer** rather than two lists
that must be kept in step — two enumerations that have to agree is exactly how
the duplicated feature masks became a hazard. That second rule has since landed:
`ScenePipeline::targetEntries` is the one list, and the dump, the viewer and the
capture paths all call it
(`src/libs/graphics/rendering/scenepipeline.cpp:1234,1277,1306,1358,1563`).

### 4.6 Build and run hygiene that outlived its context

- **If `engine.exe` is locked, a previous run is still alive.** The link fails
  and the next measurement silently uses the old binary. Check the binary's
  timestamp against your edit before believing any run.
- **A timestamp does not tell you which configuration wrote it.** Under Ninja
  Multi-Config, Debug goes to `build/debug/bin` but **Release and RelWithDebInfo
  share `build/bin`** (`src/apps/engine/CMakeLists.txt:40`, and the same line in
  every other app), so a RelWithDebInfo build silently replaces the Release
  binary a measurement was taken with. Pin `--config` for a measurement series.
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
still what ships (`src/libs/graphics/vulkan/resources.cpp:168`).

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

**Hashed alpha no longer exists.** `slang/lib/hashedalpha.slang` was deleted in
`901cee65` along with the forward shaders that were its only importers; all that
survives is an unread `kFeatureHashedAlphaTest` constant
(`slang/uniforms.slang:199`). Coverage is a plain cutout test now. If hashed
alpha is ever wanted back, it is a reimplementation, not a re-enable.

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
now applies `sampleAuthoredMirror(material, eyeReflection) * (1.0 -
diffuseSample.a)` at lines 240-241, which appears to be the same rule, but the
equivalence was not verified term by term.

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

*Current-tree notes.* **The three tiers are now the code, not a framing.** They
are `OptionApply::Live` / `Reapply` / `Restart`
(`include/reone/graphics/optionsregistry.h:38-58`), declared once per option in
one table (`src/libs/graphics/optionsregistry.cpp:204-678`) and read by the
editor, the console and the command line alike — "the classification is a
property of the option, not of the place it is edited". The mapping is what this
section predicted: width, height, vsync, shadowres, antialiasing and the render
mode are `Reapply`; texquality, anisofilter, fullscreen and headless are
`Restart`. Two refinements the prediction did not have:

- **A tier can depend on the value, not just the option.** `mode` is `Live`
  between the two raster resolves and `Reapply` only when it crosses into or out
  of path tracing, so `graphicsOptionApply` is asked about a *change*, not about
  a slot (`optionsregistry.cpp:251`, `src/apps/engine/engine.cpp:798-800`).
- **`Reapply` writes to a staged copy, never to the live options.** Writing the
  live struct at set-time would leave the running frame describing targets that
  were never allocated; `gfx apply` is what commits, and `applyGraphicsRebuild`
  does the window resize, `waitIdle` and pipeline invalidation together
  (`engine.cpp:735-758,767-781`).

An editor settings window exists
(`Editor::graphicsSettings`, `src/apps/engine/editor.cpp:825`) and reads the
same table. **Grass has left tier 3**: it was the
document's marquee example, gated at area load, and the recommendation was to
move that gate to draw time; grass is now generated on the GPU from integer
hashes and `--grassdensity` is supplied to the merge upload every frame
(`src/apps/engine/optionsparser.cpp:101,293`), so it is tier 1 already. The
document's reasoning about *why* it should move is still the right shape for the
next option that turns out to be gated at load time for no structural reason.

### 6.1 Resolution changes at runtime — still unfinished

Aspect ratio and the interface's screen centre are read from the options rather
than cached, but **nothing re-applies them**, so changing resolution while a
module is loaded leaves the view stretched and the interface off-centre. Both
need a *trigger*, not just a live value:

- each camera computes `aspect` once, where it builds its projection, and
  nothing runs that again — `src/libs/game/object/camera/{animated,dialog,
  firstperson,static,thirdperson}.cpp`, all at the same shape,
  `opts.width / opts.height` immediately before `setPerspectiveProjection`;
- `GUI::_rootOffset` is computed inside `GUI::load`
  (`src/libs/gui/gui.cpp:52,63-83`) and used by every control.

Doing it properly means a virtual on the camera base that the area can call over
its cameras, and lifting the scaling switch out of `GUI::load` so it can be
re-run.

*Re-verified 2026-08-09, and the trap has sharpened:* `width` and `height` are
now `OptionApply::Reapply`, so `gfx set width … ; gfx apply` **succeeds and
reports success**. What it actually does is resize the window, recreate the
swapchain and rebuild every render pipeline (`Engine::applyGraphicsRebuild`,
`src/apps/engine/engine.cpp:735-758`) — and touch neither the cameras nor the
GUI. The user is now told the change applied, which is worse than being told to
restart. **Treat a resolution change as needing a restart regardless of what the
console says.**

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
  from that section worth remembering. Confirmed 2026-08-09: no shader in
  `slang/` mentions bloom and nothing writes a highlight attachment; bloom is
  MASTER's RAS-033, unbuilt.
- The parity numbers themselves. `danm14ab` at 0.005771 and `tar_m02aa` at
  0.001807 described a floor against a renderer that no longer exists; both
  scenes were at the point where two shader compilers cannot be expected to
  round identically, with over 93% of differing pixels within one RGB level.
  They are history, not targets.
- Per-mesh BLAS with refit, from §10.2 — superseded by one scene BLAS rebuilt
  per frame from merged geometry.
- The `RTDebug` render mode as a listed mode. The traced-G-buffer *instrument*
  survives as §3.1 describes, weakened; `RenderMode` has three entries — `Retro`,
  `PBR`, `PathTracing` (`include/reone/graphics/options.h:120-125`).
