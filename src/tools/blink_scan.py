"""blink_scan.py <dir> <first> <last>: frames that blink - content in frame n found in neither n-1 nor n+1 within 4 px
(something that appears for one frame) or missing from n but present in both neighbours (something that vanishes for
one frame). Frames are frame_NNNNN.png (LARECOMP_SCREENSHOT_EVERY=1), compared at 480x270."""
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


def nearest(m, srcs):
    nov = np.full(m.shape[:2], 1e9, np.float32)
    for src in srcs:
        for dy in range(-4, 5, 2):
            for dx in range(-4, 5, 2):
                nov = np.minimum(nov, np.abs(m - np.roll(np.roll(src, dy, 0), dx, 1)).mean(axis=2))
    return nov


d, first, last = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
frames = {}
for n in range(first, last + 1):
    p = f'{d}/frame_{n:05d}.png'
    if os.path.exists(p):
        frames[n] = load(p)
for n in range(first + 1, last):
    if n - 1 not in frames or n not in frames or n + 1 not in frames:
        continue
    a, m, b = frames[n - 1], frames[n], frames[n + 1]
    appear = float((nearest(m, (a, b)) > 24).mean()) * 100     # in n, in neither neighbour
    vanish = float((np.minimum(nearest(a, (m,)), nearest(b, (m,))) > 24).mean()) * 100  # in both neighbours, not in n
    if appear > 0.15 or vanish > 0.15:
        print(f'{n}: appears {appear:.2f}%  vanishes {vanish:.2f}%')
