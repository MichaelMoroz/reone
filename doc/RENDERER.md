# How the renderer is put together

What exists today. [DESIGN.md](DESIGN.md) is what does not yet.

Three modes, `--mode retro|pbr|path-tracing`. **Retro is preservation** — a deviation
from the original is a bug, and the reference engines settle it. **PBR and path
tracing are the improvement**, held to looking right rather than to matching 2003.

## The frame

Raster owns primary visibility in **every** mode:

```
raster geometry → opaque G-buffer          (scene_draw over merged geometry)
raster blended  → transparency, admission order, premultiplied
PT pass 1       → rays from opaque surfaces (the G-buffer is the ray-origin set)
denoise (NRD, two instances) → composite → bloom → AA → sharpen
```

`slang/path_trace.slang` says it in the source: *"There is no camera ray."* The primary
surface is reconstructed from the G-buffer's **triangle id**, which recovers the
*geometric* normal exactly — an interpolated 8-bit stored normal could not, and that is
why the attachment is a triangle id rather than a material id. **There is no camera-ray
fallback and no option to restore one.**
**The trap this creates:** the `traced_*` outputs still exist and still look like a
cross-renderer check, but are now filled from the raster-derived surface, so they agree
with raster by construction. **Do not read a zero difference on `traced_*` versus
`g_buffer_*` as evidence that the two geometry paths agree.** They still test the
material chain and the encoding. TRC-021 owns finding a real oracle.

## The assembly contract

One assembly, owned by the composite, fed by whichever provider shades the frame:

    final = noiseFree + (diffuse + directDiffuse) * diffFactor + specular * specFactor

`noiseFree` carries emission at the primary, sky on a miss and sky-class radiance, with
the fog blend in `.a`, zero on terminators so the sky is never fogged. `directDiffuse`
is demodulated primary direct with **the mode's own visibility** — shadow maps in PBR,
shadow rays traced. `diffuse` is demodulated indirect; `specular` is demodulated
analytic GGX plus prefiltered IBL plus authored mirror in PBR, traced specular in the
tracer. `diffFactor`/`specFactor` come from the surface model and are **identical
across modes by construction** — correlation +1.0000, max difference 5e-4. `viewZ` is
the terminator sentinel: a terminating pixel carries all radiance in `noiseFree` and
the composite adds nothing.
**Ownership.** `ScenePipeline` owns the fifteen channel images and the composite
dispatch; the trace kernel binds them by name each frame, and `pbr_channels` writes seven
through the resolve set's storage bindings, **partially bound, so retro declares none and
is untouched by the layout growth.** PBR hands back raw channels at zero jitter, which
makes the composite's sampled reads the raw texel at the pixel centre, while **retro
allocates no channels and keeps its own resolve untouched** — it is the preservation mode
and shares only the push constants and descriptor layouts.

**Decided and built:** the PBR `min(1, light)` clamp before albedo is dropped (tat_m18ab
+3.1% brighter, a sand glare patch gone); the `materialDiffuse` tint on direct is dropped,
subsumed by `diffFactor`; per-light `kD` and ambient-lobe Fresnel are dropped, because the
shared factors *are* the surface model; the self-illum specular gate is dropped. Emission
follows the tracer's rule — radiance into `noiseFree`, not albedo-multiplied — fog is
shared, and lightmap is one `lightmapIntensity` defaulting 1.0 PBR / 0.0 traced, **the one
documented divergence, now a default.** **What stays per mode, deliberately:** the
occlusion answer (map or ray), the tracer's NEE selection and penumbra, PBR's SSR tail
(RAS-037), and each mode's dial **defaults** — never the dials themselves, never the
assembly. Retro also keeps Odyssey's lighting maths *including its bugs*, so **any
refactor that shares lighting code between retro and PBR must keep both functions rather
than converging them.**

## The scene boundary

**`GpuScene` owns merged world-space geometry, stable primitive identity, and the
lifetime of both. Consumers own their own lowering.** It **must not require ray tracing**
— the merge is a plain compute dispatch and the BLAS/TLAS sit on top of its output, so
**the tracer layers acceleration structures over the scene; it does not define it.**

Three boundary decisions that survived review. **Sky-room classification is core; the
bake is not** — detection decides which meshes are *admitted*, and **the consumer requests
replacement, never reaching back into admission afterwards.** **Curated overrides and
`TraceClass` stay tracer-side**, with the core keeping source material *identity*. And
**the opaque/non-opaque partition needs an explicit reason or it belongs to the tracer**,
because `geometryIndex` means "may be committed as opaque in hardware" versus "must run
candidate semantics" — ray policy, and **a core that emits the partition without knowing
why has hidden ray policy behind a neutral name.** **Buffer usage and synchronisation are
part of the contract:** a core cannot issue one trace-shaped barrier and call the buffer
generally consumable.
**Four identity concepts, deliberately not one:** object identity (on `SceneNode`,
creation to destruction, keys caches across frames); content version (tells a cache its
contents are stale); renderable data (one frame); and the AS index (backend, one frame).
Conflating these is what makes retained-mode designs fail — in particular identity and
version, because **an id stable across a texture swap is doing its job correctly and
telling a BLAS cache nothing.**

**The frame boundary is the only consistency point.** No publish/subscribe, no change
notification, no dirty flags: disappearance is an id ceasing to appear and change is a
version mismatch, **so neither requires the scene to announce anything, which is what
keeps destruction and module transitions free of synchronisation.** **The scene publishes,
the renderer derives** — the renderer never owns scene objects, and the scene never holds
a backend handle. **Culling is a policy applied to the snapshot, not a property of an
object.** The TLAS rule that falls out: **every eligible object goes in — no relevance
test, no frustum test. Build it the expensive way first; a reflection missing geometry is
not a performance result you can interpret.**

## The RHI

**No `Vulkan` identifier appears outside `src/libs/graphics/vulkan/`.** Both gate lines
are zero:

    rg 'vk[A-Z]|Vk[A-Z]|vma[A-Z]'       src include --glob '!**/graphics/vulkan/**'
    rg 'VK_[A-Z0-9_]+|ImGui_ImplVulkan' src include --glob '!**/graphics/vulkan/**'

**The boundary is a directory, not an allowlist somebody maintains:** the RHI *is*
`vulkan/`, defined by what remains after its clients left — 18 parent interfaces under
`include/reone/graphics/` with `Vulkan*` children inside, and `vulkan/` holding the RHI,
two vendor bindings that take native handles by construction, and the renderer that is
the RHI's own face.

Two rules govern anything added. **The seam expresses intent, not Vulkan calls** — the
default failure mode of an RHI is a call-per-call mirror, as large as Vulkan and buying
nothing because a client written against it still thinks in Vulkan; `RayQuery` began
with 27 distinct Vulkan types and mirroring them would have doubled the seam for one
client, where *build a structure over this geometry* / *trace these rays against it*
kept them inside. And **the parents carry API-neutral types, not just API-neutral
names** — a parent whose signature says `VkFormat` is a Vulkan interface wearing an `I`,
and a first attempt at that took the token count outside `vulkan/` from 2 sites to 16.
**"Minimise the RHI" governs within that choice:** a neutral type earns its place by a
client needing it, never by symmetry.

## Raster is permanent

Raster is not the next GL. The **path-traced look** is the destination on modern hardware
and unavailable without ray tracing; the **original look** is reachable by raster either
way — and **someone on a 5090 may choose the original look, which is an art-direction
choice, not a fallback.** So raster must render both looks *and* work with no ray tracing
at all: it is load-bearing, not transitional.

Whether raster consuming `GpuScene` pays on weak hardware is an **empirical question, not
an assumption** — a per-frame compute merge on a GPU with weak compute may cost more than
the draws it saves, and *"raster keeps per-mesh draws on hardware where the merge does not
pay"* is an acceptable answer (RAS-021). The fallback is N draws over the same `GpuScene`
records, not a resurrected registry.
