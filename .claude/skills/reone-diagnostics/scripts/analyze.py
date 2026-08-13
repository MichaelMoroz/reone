"""Convergence analysis for a frozen-simulation capture sequence.

With the world frozen from --freezeframe on, the only thing still changing
between consecutive frames is temporal accumulation. So mean |frame N -
frame N-1| is the residual the denoiser and TAA have not removed yet, and a
working temporal filter drives it towards zero.
"""
import sys
import struct
import glob
import os
import re
import numpy as np


def load_tga(path):
    d = open(path, 'rb').read()
    idlen = d[0]
    w, h = struct.unpack_from('<HH', d, 12)
    bpp = d[16]
    desc = d[17]
    n = w * h * (bpp // 8)
    a = np.frombuffer(d[18 + idlen:18 + idlen + n], dtype=np.uint8)
    a = a.reshape(h, w, bpp // 8)[..., :3][..., ::-1]  # BGR -> RGB
    if not (desc & 0x20):
        a = a[::-1]
    return a.astype(np.float64)


files = sorted(glob.glob(os.path.join(sys.argv[1], '*.tga')))
if not files:
    sys.exit('no captures found')

def frame_index(path):
    """Trailing run of digits, whatever separates it from the stem.

    The engine numbers a multi-frame capture `<stem>-0001.tga`; older captures
    written by the retired --capture CLI use `<stem>_0350.tga`. Splitting on one
    separator throws on the other, which fails the whole run rather than the one
    file, so match the digits themselves.
    """
    stem = os.path.splitext(os.path.basename(path))[0]
    match = re.search(r'(\d+)$', stem)
    if not match:
        sys.exit(f'capture has no frame number in its name: {path}')
    return int(match.group(1))


frames = [(frame_index(f), load_tga(f)) for f in files]
frames.sort()

first = frames[0][1]
print(f'{len(frames)} frames, {first.shape[1]}x{first.shape[0]}')
print(f'frame {frames[0][0]}: mean={first.mean():.2f} min={first.min():.0f} '
      f'max={first.max():.0f} nonzero={100.0 * (first > 0).mean():.1f}%')
if first.max() == 0:
    sys.exit('FAIL: captures are black - the run rendered nothing')

# Edge mask from the spatial gradient of the last frame: cutout and geometric
# edges are where a temporal filter fails first, and a whole-frame mean hides
# them behind a large flat interior.
last = frames[-1][1].mean(axis=2)
gx = np.abs(np.diff(last, axis=1, prepend=last[:, :1]))
gy = np.abs(np.diff(last, axis=0, prepend=last[:1, :]))
grad = gx + gy
edges = grad > np.percentile(grad, 97.0)
print(f'edge mask: {100.0 * edges.mean():.2f}% of pixels\n')

print(f'{"frames":>12} {"mean|d|":>9} {"p99|d|":>8} {"max|d|":>7} '
      f'{"changed%":>9} {"edge mean|d|":>13}')
rows = []
for (na, a), (nb, b) in zip(frames, frames[1:]):
    d = np.abs(b - a)
    dm = d.mean(axis=2)
    row = (na, nb, d.mean(), np.percentile(d, 99), d.max(),
           100.0 * (dm > 0.5).mean(), dm[edges].mean())
    rows.append(row)
    print(f'{na:>5}->{nb:<6} {row[2]:>9.4f} {row[3]:>8.2f} {row[4]:>7.0f} '
          f'{row[5]:>9.3f} {row[6]:>13.4f}')

n = len(rows)
head = rows[:max(1, n // 5)]
tail = rows[-max(1, n // 5):]
hm = np.mean([r[2] for r in head])
tm = np.mean([r[2] for r in tail])
he = np.mean([r[6] for r in head])
te = np.mean([r[6] for r in tail])
print(f'\nfirst {len(head)} steps: mean|d|={hm:.4f}  edge mean|d|={he:.4f}')
print(f'last  {len(tail)} steps: mean|d|={tm:.4f}  edge mean|d|={te:.4f}')
print(f'ratio last/first: overall {tm / hm if hm else float("nan"):.3f}  '
      f'edge {te / he if he else float("nan"):.3f}')

print(f'\n|last - first| mean = {np.abs(frames[-1][1] - frames[0][1]).mean():.4f}')

# A blend-factor filter settles at a small non-zero residual rather than
# reaching zero, so the test is not "does it hit zero" but "does it decay
# geometrically from cold to a settled floor". That needs the history to have
# been restarted at the first captured frame.
cold = rows[0][2]
settled = np.median([r[2] for r in tail])
decay = cold / settled if settled else float('inf')
flat = abs(tm - hm) < 0.25 * hm
print(f'\ncold step {cold:.4f} -> settled {settled:.4f}  (factor {decay:.2f})')
if decay >= 2.0:
    print(f'verdict: ACCUMULATING - residual decays {decay:.1f}x from cold to '
          f'its steady state, which is what a working temporal filter does')
elif flat:
    print('verdict: NOT ACCUMULATING - residual is flat from the first frame, '
          'so no history is being blended in at all')
else:
    print(f'verdict: WEAK - only a {decay:.2f}x decay from cold; the filter is '
          'contributing far less than its blend factor implies')
