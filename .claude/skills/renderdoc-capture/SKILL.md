---
name: renderdoc-capture
description: Compare two reone builds or backends empirically, and inspect what the GPU actually received. Covers the deterministic screenshot harness, numeric render-target dumps for localising a difference to a pass, and scripted RenderDoc capture. Use when a shader renders wrongly, when OpenGL and Vulkan disagree, or when you need bound buffers and uniform contents rather than a guess. Triggers on: shader renders wrong, geometry missing, compare backends, GL vs Vulkan, A/B, frame capture, RenderDoc, uniform buffer contents, G-buffer.
---

# Capturing and inspecting a reone frame

Guessing at shader faults from the rendered image is slow and gets it wrong. Two
tools make it empirical: an unattended screenshot harness for A/B comparison, and
a scripted RenderDoc capture for seeing what the GPU actually received.

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

Two runs at the same `--captureframe` produce **byte-identical** images, in the
menu and in gameplay, on either backend. If they do not, something is genuinely
nondeterministic and that is the bug, not the harness.

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

Run it twice with whatever you are comparing - for example, `--backend gl`
against `--backend vulkan` - then diff. TGA here is BGR and bottom-up:

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

## Render target dumps, for localising a difference to a pass

A screenshot is the end of a long chain, so when two backends disagree it says
nothing about where. `--dumptargets <dir>` writes every target the scene
pipeline exposes as a `.npy`, on the same frame as the screenshot:

```
engine.exe --backend vulkan --pbr 1 --dumptargets out_vk --captureframe 900 ...
engine.exe --backend gl     --pbr 1 --dumptargets out_gl --captureframe 900 ...
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
- **Compare the same renderer.** `reone.cfg` here has `pbr=0`, so plain
  `--backend gl` runs the *retro* pipeline while Vulkan always runs PBR
  deferred - two different renderers, not two backends. Every comparison in one
  whole session was made this way before a zero-target dump gave it away. Pass
  `--pbr 1` explicitly on both sides.
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
