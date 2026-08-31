"""Amplified difference image between the last two frames of a capture run."""
import sys
import struct
import glob
import os
import numpy as np
from PIL import Image


def load_tga(path):
    d = open(path, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp = d[16]
    desc = d[17]
    n = w * h * (bpp // 8)
    a = np.frombuffer(d[18 + idlen:18 + idlen + n], dtype=np.uint8)
    a = a.reshape(h, w, bpp // 8)[..., :3][..., ::-1]
    if not (desc & 0x20):
        a = a[::-1]
    return a.astype(np.float64)


src, out, gain = sys.argv[1], sys.argv[2], float(sys.argv[3])
files = sorted(glob.glob(os.path.join(src, '*.tga')))
a, b = load_tga(files[-2]), load_tga(files[-1])
d = np.abs(b - a)
print(f'{os.path.basename(files[-2])} -> {os.path.basename(files[-1])}')
print(f'mean={d.mean():.4f} max={d.max():.0f} '
      f'>2/255: {100.0 * (d.mean(axis=2) > 2).mean():.3f}% of pixels')

Image.fromarray(np.clip(d * gain, 0, 255).astype(np.uint8)).save(out)

# Where the residual actually lives, as a coarse grid, so a hotspot that is a
# small share of the frame still shows up as a number.
g = d.mean(axis=2)
h, w = g.shape
cells = [(g[y:y + h // 6, x:x + w // 8].mean(), x // (w // 8), y // (h // 6))
         for y in range(0, h - h // 6 + 1, h // 6)
         for x in range(0, w - w // 8 + 1, w // 8)]
cells.sort(reverse=True)
print('hottest cells (col,row of an 8x6 grid): ' +
      ', '.join(f'({c[1]},{c[2]})={c[0]:.2f}' for c in cells[:5]))

# Downscale for viewing, keeping peaks: max-pool so a one-pixel edge survives.
s = 3
hh, ww = (h // s) * s, (w // s) * s
pooled = np.clip(d[:hh, :ww].reshape(hh // s, s, ww // s, s, 3).max(axis=(1, 3)) * gain, 0, 255)
Image.fromarray(pooled.astype(np.uint8)).save(out.replace('.png', '_pooled.png'))
print(f'wrote {out} and its max-pooled 1/{s} view')
