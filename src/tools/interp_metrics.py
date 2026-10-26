"""interp_metrics.py <dir> <n>...: how far each in-between image (mid_<n>.png) is from the middle of its neighbours.

The frames come from LARECOMP_WINDOW_SHOTS (fin_<n>.png: the game's frame n, mid_<n>.png: the image shown before it);
for each n with fin_<n-1> too: 
  motion   mean |fin_n - fin_(n-1)|   how much the picture moves in one game frame
  err      mean |mid_n - (fin_(n-1) + fin_n)/2|   how far the in-between image is from the plain average
  ratio    err / motion   a clean interpolation of smooth motion stays below ~0.5; ghosts and deformations push it up
  worst    share of pixels whose error exceeds the motion's own 99th percentile
  novel%   share of in-between pixels found in neither neighbour within 4 px: what the interpolation invented
           (ghosts, smears, blocks). Flagged above 0.5%; ==prev / ==cur: the in-between image is a neighbour (no blend)
Also writes <dir>/heat_<n>.png (the error, amplified) for the frames above the threshold (ratio > 0.6).
Frames are compared at 480x270 (decoded with ffmpeg: FFMPEG env)."""
import os
import subprocess
import sys

import numpy as np

W, H = 480, 270
ff = os.environ.get('FFMPEG', 'ffmpeg')


def load(path):
    raw = subprocess.run([ff, '-v', 'error', '-i', path, '-vf', f'scale={W}:{H}', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-'],
                         capture_output=True, check=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(H, W, 3).astype(np.float32)


def save(path, img):
    subprocess.run([ff, '-v', 'error', '-y', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', f'{W}x{H}', '-i', '-', path],
                   input=np.clip(img, 0, 255).astype(np.uint8).tobytes(), check=True)


d = sys.argv[1]
ns = [int(x) for x in sys.argv[2:]]
print(f'{"n":>5} {"motion":>7} {"err":>7} {"ratio":>6} {"worst%":>7} {"novel%":>6}')
bad = 0
for n in ns:
    paths = [f'{d}/fin_{n - 1}.png', f'{d}/mid_{n}.png', f'{d}/fin_{n}.png']
    if not all(os.path.exists(p) for p in paths):
        continue
    a, m, b = (load(p) for p in paths)
    motion = float(np.abs(b - a).mean())
    diff = np.abs(m - (a + b) / 2).mean(axis=2)
    err = float(diff.mean())
    ratio = err / motion if motion > 0.05 else 0.0
    thr = max(float(np.percentile(np.abs(b - a).mean(axis=2), 99)), 12.0)
    worst = float((diff > thr).mean()) * 100
    # novelty: how far each in-between pixel is from the nearest pixel of either neighbour within 4 px (a moved object
    # is found in one of them; a ghost, a smear or a white block is not)
    nov = np.full(diff.shape, 1e9, np.float32)
    for src in (a, b):
        for dy in range(-4, 5, 2):
            for dx in range(-4, 5, 2):
                sh = np.roll(np.roll(src, dy, 0), dx, 1)
                nov = np.minimum(nov, np.abs(m - sh).mean(axis=2))
    novel = float((nov > 24).mean()) * 100
    e_prev = float(np.abs(m - a).mean()); e_cur = float(np.abs(m - b).mean())
    kind = ' ==prev' if e_prev < 0.02 * max(motion, 1) + 0.02 else (' ==cur' if e_cur < 0.02 * max(motion, 1) + 0.02 else '')
    flag = (kind or ' <--') if novel > 0.5 or kind else ''
    bad += bool(flag)
    print(f'{n:5d} {motion:7.2f} {err:7.2f} {ratio:6.2f} {worst:7.2f} {novel:6.2f}{flag}')
    if flag.strip() == '<--':
        save(f'{d}/heat_{n}.png', np.repeat(np.clip(nov * 4, 0, 255)[:, :, None], 3, axis=2))
print(f'{bad} of {len(ns)} frames flagged')
