"""Per-channel frame-to-frame difference on a frozen scene.

With the simulation frozen, each channel either is deterministic or is not.
The visibility ray's outputs - noise-free, albedo, viewZ, normal, motion -
depend only on the camera, so consecutive frames must match exactly. The
sampled channels depend on the per-frame RNG and will not.
"""
import sys
import os
import numpy as np

a_dir, b_dir = sys.argv[1], sys.argv[2]
names = sorted(n for n in os.listdir(a_dir) if n.startswith(('traced_', 'denoised_')))

print(f'{"channel":<26} {"mean|d|":>10} {"max|d|":>10} {"differing%":>11}  verdict')
for n in names:
    a = np.load(os.path.join(a_dir, n)).astype(np.float64)
    b = np.load(os.path.join(b_dir, n)).astype(np.float64)
    if a.shape != b.shape:
        print(f'{n:<26} shape mismatch {a.shape} vs {b.shape}')
        continue
    d = np.abs(a - b)
    frac = 100.0 * (d > 1e-6).mean()
    tag = 'identical' if frac == 0.0 else f'VARIES'
    print(f'{n[:-4]:<26} {d.mean():>10.5f} {d.max():>10.4f} {frac:>10.3f}%  {tag}')
