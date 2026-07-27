# What is left on the Vulkan backend

Six pieces, ordered so each one is useful before the next starts. Every
file:line below is a thing to read before changing it.

## 1. The render target viewer has nothing to show on Vulkan

`VulkanRenderPipeline::targets()` returns an empty vector -
`src/libs/scene/render/pipeline/vulkan.cpp:1120`. Its comment says the viewer
is part of the ImGui editor and the editor does not run on Vulkan, which
stopped being true when the editor was brought up. So the list is empty for
one reason only: nothing was ever put in it.

The OpenGL side is the reference, `src/libs/scene/render/pipeline/pbr.cpp:298`:

    G-buffer diffuse, eye normal, lightmap, self-illum, motion, depth
    SSAO, SSR
    Deferred opaque 1, Deferred opaque 2
    OIT accum, OIT revealage
    Output

Vulkan can supply all of these except SSAO, SSR and the two deferred-opaque
targets, which do not exist there yet - items 3 and 4 add three of them.

Two things stand in the way, and only the second is real work.

`RenderTargetInfo` carries a `graphics::Texture *`, and the editor hands
ImGui an OpenGL texture id at `src/apps/engine/editor.cpp:301`. Vulkan targets
are `VulkanImage`s, not `Texture`s. So the viewer needs a backend-neutral
handle: on Vulkan that is a descriptor set from `ImGui_ImplVulkan_AddTexture`,
cached per image and destroyed **before** the image view it points at. Targets
are recreated on resize, so that ordering is not optional - a stale set is a
use-after-free that no single-frame capture will catch.

The dump path at `dumpTargets` already enumerates every Vulkan target with its
name and current layout. That enumeration is most of what the viewer wants;
prefer one list serving both over two lists that must agree. Two enumerations
that have to stay in step is exactly how the duplicated feature masks became a
hazard.

Note the depth and motion targets need their `RenderTargetKind` respected -
raw depth in a preview pane is unreadable without the conversion the OpenGL
viewer already does.

## 2. A settings tool, and which settings can actually change

They are not uniformly runtime-changeable. Three tiers, from reading where
each option is consulted:

**Free - read per frame, change and it takes effect next frame**

    fxaa, sharpen      filterChainPass, vulkan.cpp:893-912, pbr.cpp:417-421
    ssao, ssr          bound conditionally per frame
    drawDistance, taaJitter

**Needs targets or the swapchain rebuilt**

    shadowResolution   sizes shadow targets at init - pbr.cpp:122,135,
                       vulkan.cpp:193
    width, height      size every target
    vsync              swapchain present mode

**Needs assets or the scene reloaded**

    grass              read when the area builds its grass nodes,
                       src/libs/game/object/area.cpp:463 - toggling it later
                       does nothing until the area reloads
    textureQuality     affects what is loaded
    anisotropicFiltering  baked into the sampler cache at upload
    pbr                selects the whole pipeline

So the tool should be honest about the tiers rather than presenting twelve
checkboxes that behave in three different ways:

- tier 1 as live controls;
- tier 2 as controls that trigger an explicit rebuild, which the pipeline
  already knows how to do on resize;
- tier 3 either disabled with a note, or offered with a "requires reload"
  marker and no pretence that it applies now.

`grass` is the interesting one. It is a per-frame skip in every other respect;
only node creation is gated. Moving that gate from area load to draw time
would promote it to tier 1 cheaply, and is worth doing while the tool is being
written. That also aligns with the registration rework, where the renderer
decides what to draw.

The tool belongs beside the existing editor windows and should work on both
backends - the options struct is shared, and nothing here is Vulkan-specific
except the swapchain rebuild.

### Unfinished: resolution changes at runtime

Aspect ratio and the interface's screen centre are now read from the options
rather than cached, but nothing re-applies them, so changing the resolution
while a module is loaded still leaves the view stretched and the interface
off-centre. Both need a trigger, not just a live value:

- each camera applies its projection from `load`, `deserialize` or
  `updateProjection` depending on the class, and none of those runs again;
- `GUI::_rootOffset` is computed inside `load` and used by every control.

Doing it properly means a virtual on the camera base that the area can call
over its cameras, and lifting the scaling switch out of `GUI::load` so it can
be re-run. Until then, treat resolution as needing a restart.

## 3. Bloom

OpenGL renders self-illuminated highlights into a second colour attachment in
the deferred resolve - `fragHilights` in `glsl/f_pbr_combine.glsl`, landing in
`cbDeferredOpaque2` - blurs it, and adds it back when resolving transparency,
`glsl/f_oit_blend.glsl:20`.

Vulkan's `oitBlendFragment` substitutes zero for that term, with a comment
saying it belongs with the bloom port. This is now the last known reason
transparency cannot reach zero difference between the backends: at a saber-crop
revealage of 0.956, a mean blurred highlight of about 0.057 accounts for the
entire residual.

What is needed:

- a second colour attachment on the Vulkan resolve, carrying
  `selfIllumed * step(0.95, color) * color` as the OpenGL one does;
- a blur. `gausBlur9Fragment` and `gausBlur13Fragment` are already ported in
  `slang/postprocess.slang` from the FXAA work, and the OpenGL pipeline runs
  the blur separably, once per axis - see where it blurs before transparent
  geometry in `pbr.cpp`;
- feeding the result into `oitBlendFragment` in place of the zero;
- both new targets exposed through `targets()` and `dumpTargets`, so the pair
  can be diffed directly rather than inferred from the composite.

Verification is unusually clean here: the saber crop is 4.5450 today, and the
prediction is that most of what remains is this term. If it does not move by
roughly that much, the diagnosis was wrong and should be revisited rather than
patched around.

## 4. SSAO and SSR

Both exist on OpenGL - `glsl/f_pbr_ssao.glsl`, `glsl/f_pbr_ssr.glsl` - and
neither exists on Vulkan. Measured together they are worth about 0.169/255 of
whole-frame difference in danm14ab frame 900, which is small, but that frame
has little reflective geometry and the figure is scene-dependent.

Isolate them when measuring. They were tested together once and the result
attributed to SSR alone; re-isolated, SSAO was about 0.113 and SSR about 0.068.

Both are screen-space passes over the existing G-buffer, so this is a port of
two fragment shaders plus two targets, not new plumbing. The resolve already
has `#ifdef R_SSAO` and `R_SSR` branches on the OpenGL side and the Slang
resolve has the same shape with the terms dropped out, so the wiring points
are already marked.

## 5. The retro pipeline

Selected with `--pbr 0`, and it is the only thing in the engine that renders
the game the way the original did. It exists on OpenGL only:
`src/libs/scene/render/pipeline/retro.cpp` (297 lines) and
`src/libs/scene/render/pass/retro.cpp` (336). Vulkan has no counterpart, so
`--pbr 0 --backend vulkan` has nothing to run.

It is a smaller job than the line count suggests, because it is **forward, not
deferred** - no G-buffer, no resolve, no environment derivation. Draw opaque
models with lighting applied in the fragment shader, then transparency, then
post-processing.

Four shaders have no Slang counterpart:

    glsl/f_rtr_opaqmodel.glsl
    glsl/f_rtr_grass.glsl
    glsl/f_rtr_walkmesh.glsl
    glsl/f_rtr_aabb.glsl

Everything else the retro pass uses is already ported for the PBR pipeline:
directional and point shadows, `oitModel` and `oitParticles`, and billboards.
So the work is those four fragment shaders plus a Vulkan pipeline that
sequences them, and it inherits the OIT and shadow work already done.

`f_rtr_opaqmodel.glsl:81-84` is worth reading first regardless - it is the
original environment-map application, `env * (1 - alpha)`, which is what the
PBR path's reflection strength was reconstructed from. Porting it puts the
reference and the reimplementation in the same binary.

**The harness does not cover this pipeline.** `RetroRenderPipeline` exposes no
targets and dumps nothing, so `--dumptargets` produces an empty directory and
every parity number in this document is a PBR number. Two backends can only be
compared here by screenshot, which is precisely the situation the dump path
exists to avoid. Exposing even the output target would restore lossless
comparison; doing it on both backends at once is the cheapest moment, since
the OpenGL side needs it too.

Ordering note: this could equally come before bloom or SSAO. It is placed here
because those two close measurable gaps in a pipeline that is already
comparable, whereas this one first has to become comparable at all.

## 6. Renderer registration

Has its own document - `doc/renderer-registration-plan.md`. It is last here
because it is the only item that changes the shape of the scene/renderer
boundary, and because items 1 to 5 all produce things it will want to move:
more targets to expose, more settings to toggle, and two more screen-space
passes whose culling policy the renderer will own.

Two cheap pieces of preparation can be done at any time, independently:

- check whether grass cluster placement is deterministic per face, and make it
  a hash of (face index, cluster index) if it is not;
- count total grass instances for a real outdoor area, to find out whether
  "grass everywhere" is a TLAS that can simply be built.

## Working notes that apply to all of it

Build the `engine` target, never a sublibrary alone: shaders come from
`build/bin/shaderpack.erf` and only that target repacks them. An edit to
`glsl/` or `slang/` that is not repacked runs the previous shader and reports
success.

If `engine.exe` is locked, a previous run is still alive: the link fails and
the next measurement silently uses the old binary.

The former 1.2516 whole-frame RGB mean absolute-difference baseline for
danm14ab frame 900 is invalid. It used a stale Vulkan dump (fixed in commit
3b4f5897) and compared against OpenGL's Slang SPIR-V path, which dropped grass
and character hair. No replacement figure is known yet.

An earlier figure of 1.2179 appears in commit messages up to this point, and
it was correct for its time. The merge of pull request #1 moved it: that
change touches area, creature and player logic and no shader or renderer code,
but it shifts the OpenGL output alone by 0.2949 while leaving every G-buffer
target bit-identical. Opaque geometry is unchanged; `oit_accum` moves by
0.0030, and the transparency resolve divides accumulated colour by accumulated
weight, so a small change there is amplified before the filter chain amplifies
it again.

The lesson is that this number is a property of the game state as much as of
the renderer, and any change to what the scene contains at frame 900 rebases
it. Re-measure the baseline after merging anything, rather than comparing a
new figure against one taken before.
