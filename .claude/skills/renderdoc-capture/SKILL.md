---
name: renderdoc-capture
description: Capture and inspect a reone frame with RenderDoc, unattended. Use when a shader renders wrongly and you need to see what the GPU actually received - bound buffers, uniform contents, draw parameters - rather than guessing. Also covers the automated screenshot A/B harness. Triggers on: shader renders wrong, geometry missing, RenderDoc, frame capture, uniform buffer contents, compare shader builds.
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
engine.exe --game "<GAME_DIR>" --slangshaders=1 \
    --commands-file warp.txt --capture out.tga --captureframe 3
```

- `--commands-file` runs console commands at startup; `warp <module>` is the useful one.
  It runs during init, so the module is loaded before the first frame.
- `--capture <path>` writes a TGA on frame `--captureframe`, then exits.
- **Capture the earliest frame that looks settled.** Frame 2 already renders a
  complete scene in danm14ab; there is no need to wait.
- A capture run uses a **fixed 1/60 timestep** so that two runs see the same
  sequence of frames. Without it the same frame number lands on different
  animation state every run and the diff is meaningless. Two runs at the same
  `--captureframe` are bit-identical; if they are not, something is genuinely
  nondeterministic and that is the bug, not the harness.

Run it twice with `--slangshaders=0` and `=1`, then diff. TGA here is BGR and
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

## RenderDoc, scripted

`renderdoccmd capture` has no option to capture a chosen frame, and triggering by
keypress does not suit an unattended run. The engine therefore calls RenderDoc's
in-application API itself: `--renderdoc 1` triggers a capture on the frame before
the screenshot. `extern/renderdoc_app.h` is vendored from the installation.

```
& "C:\Program Files\RenderDoc\renderdoccmd.exe" capture --wait-for-exit \
    --working-dir "<BIN>" --capture-file "<BIN>\name" \
    "<BIN>\engine.exe" --game "<GAME_DIR>" --slangshaders=1 \
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

## Traps that cost real time here

- **Stale shader modules.** `cmake --build . --target engine` does not run the
  SPIR-V transpile. Three debugging probes ran against a module older than the
  edit being tested and produced meaningless answers. Always build the default
  target, and check `slang/x.slang` is older than `build/bin/spirv/x.spv`.
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
- **The mouse cursor is in the capture.** It is drawn wherever the OS pointer
  happens to be, so it appears in every diff as a few hundred sharp pixels in an
  arbitrary place. Rule it out before investigating.
- **Diff by region against the noise floor, not the whole frame.** A whole-frame
  percentage hides everything. Comparing HUD regions separately, each against
  two runs of the *same* build, is what exposed a real minimap regression: the
  minimap is bit-stable run to run, so its 17.6% could not be animation, while
  everything else in the frame sat at its own noise level.
- **Know which frames are deterministic.** GUI frames are bit-identical run to
  run. A gameplay frame is not: about a third of the image differs between two
  runs of the *same* build. Before attributing a scene diff to a change, run the
  same build twice and compare that noise floor first.
- **Beware bimodal noise.** The scene variance is not a smooth distribution:
  two runs either agree to within 1% or differ across 33%. A single pair of runs
  therefore "proves" whatever you were hoping for about half the time. Disabling
  SSAO looked like a clean fix on the first try and reversed on the second.
  Repeat any A/B that changes a conclusion.
