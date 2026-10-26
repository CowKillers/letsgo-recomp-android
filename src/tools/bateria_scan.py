"""bateria_scan.py: reads the raw frame files of tools/bateria.sh (LARECOMP_SHOT_RAW: u32 frame + 480x270 RGB per record).

  bateria_scan.py blink <raw> <first> <last>   frames where something shows up or vanishes for one frame (as blink_scan.py,
                                                on luma, straight from the raw file: no PNGs, no ffmpeg); prints the lines and
                                                "STRONG n" (>= 0.5% of the image) as the last line
  bateria_scan.py diff <raw A> <raw B>          per-frame difference between two runs of the same route: mean absolute luma
                                                difference, % of pixels off by > 24; prints a summary and the worst frames
  bateria_scan.py cmp <A runs...> -- <B runs...>   the A x A spread against A x B (3 + 3 runs): the verdict OK / OUTSIDE
  bateria_scan.py noise <raw A1> <raw A2> <raw B> [k]
                                                B against the noise between two runs of the same mode: frames where B differs
                                                from A1 and from A2 by more than k (default 1.5) x the A1-A2 difference
                                                (+ a floor): the frames that really changed, not run-to-run jitter
"""
import sys
from multiprocessing import Pool

import numpy as np

W, H = 480, 270
REC = 4 + W * H * 3
R = 4


def load(path):
    raw = np.memmap(path, dtype=np.uint8, mode='r')
    n = len(raw) // REC
    raw = raw[:n * REC].reshape(n, REC)
    nums = raw[:, :4].copy().view('<u4').ravel()
    pix = raw[:, 4:].reshape(n, H, W, 3)
    return {int(k): i for i, k in enumerate(nums)}, pix


def luma(pix, i):
    p = pix[i].astype(np.float32)
    return p[..., 0] * 0.299 + p[..., 1] * 0.587 + p[..., 2] * 0.114


def novelty(a, b):
    """Per pixel of a: the smallest difference to b shifted by up to R px (steps of 2)."""
    best = None
    src = np.pad(b, ((R, R), (R, R)), mode='edge')
    for dy in range(-R, R + 1, 2):
        for dx in range(-R, R + 1, 2):
            d = np.abs(a - src[R + dy:R + dy + H, R + dx:R + dx + W])
            best = d if best is None else np.minimum(best, d)
    return best


def blink_one(args):
    path, idx = args
    index, pix = load(path)
    p, c, n = (luma(pix, i) for i in idx)
    nov = lambda a, b: novelty(a, b) > 24
    steady = ~nov(p, n) & ~nov(n, p)
    appear = float((nov(c, p) & nov(c, n) & steady).mean()) * 100
    vanish = float((nov(p, c) & nov(n, c) & steady).mean()) * 100
    return appear, vanish


def cmd_blink(path, first, last):
    index, _ = load(path)
    frames = [f for f in range(first + 1, last) if f - 1 in index and f in index and f + 1 in index]
    with Pool(4) as pool:
        res = pool.map(blink_one, [(path, (index[f - 1], index[f], index[f + 1])) for f in frames], chunksize=8)
    strong = 0
    for f, (a, v) in zip(frames, res):
        if a > 0.15 or v > 0.15:
            print(f'{f}: appears {a:.2f}%  vanishes {v:.2f}%')
        if a >= 0.5 or v >= 0.5:
            strong += 1
    print(f'STRONG {strong}')


def series(a, b):
    ia, pa = load(a)
    ib, pb = load(b)
    common = sorted(set(ia) & set(ib))
    out = {}
    for f in common:
        x, y = luma(pa, ia[f]), luma(pb, ib[f])
        d = np.abs(x - y)
        out[f] = (float(d.mean()), float((d > 24).mean() * 100))
    return out, len(set(ia) ^ set(ib))


def cmd_diff(a, b):
    s, missing = series(a, b)
    if not s:
        sys.exit('no common frames')
    m = np.array([v[0] for v in s.values()]); p = np.array([v[1] for v in s.values()])
    print(f'frames {len(s)} (not in both: {missing})  mean abs diff: avg {m.mean():.3f} p95 {np.percentile(m, 95):.3f} max {m.max():.3f}'
          f'  pixels off >24: avg {p.mean():.3f}% p95 {np.percentile(p, 95):.3f}% max {p.max():.3f}%')
    for f, v in sorted(s.items(), key=lambda kv: -kv[1][1])[:8]:
        print(f'  frame {f}: {v[0]:.3f}  {v[1]:.3f}%')


def cmd_noise(a1, a2, b, k=1.5):
    n, _ = series(a1, a2)
    d1, _ = series(a1, b)
    d2, _ = series(a2, b)
    bad = []
    for f in sorted(set(n) & set(d1) & set(d2)):
        lim = max(n[f][1] * k, 0.2)  # (% of pixels; 0.2% floor)
        if d1[f][1] > lim and d2[f][1] > lim:
            bad.append((f, d1[f][1], d2[f][1], n[f][1]))
    tot = len(set(n) & set(d1) & set(d2))
    print(f'frames {tot}: {len(bad)} differ from both runs of the other mode beyond the noise (k={k})')
    for f, x, y, z in sorted(bad, key=lambda t: -min(t[1], t[2]))[:10]:
        print(f'  frame {f}: vs A1 {x:.2f}%  vs A2 {y:.2f}%  (A1-A2 {z:.2f}%)')
    print(f'DIFFER {len(bad)}')


def cmd_cmp(group_a, group_b, pct=1.0):
    """Pairs within A (the noise between runs of one mode) against pairs A x B: per pair, how many frames differ by more than
    pct % of the pixels. B is as it should be when its A x B counts sit inside the A x A spread."""
    def count(x, y):
        s, _ = series(x, y)
        return sum(1 for v in s.values() if v[1] > pct), len(s)
    aa = [count(group_a[i], group_a[j]) for i in range(len(group_a)) for j in range(i + 1, len(group_a))]
    ab = [count(x, y) for x in group_a for y in group_b]
    f = lambda l: f'{min(c for c, _ in l)}-{max(c for c, _ in l)} (avg {sum(c for c, _ in l) / len(l):.0f}) of {l[0][1]}'
    print(f'frames off by >{pct}% of pixels:  A x A {f(aa)}   A x B {f(ab)}')
    print(f'CMP {"OK" if max(c for c, _ in ab) <= max(c for c, _ in aa) * 1.5 + 5 else "OUTSIDE"}')


if __name__ == '__main__':
    c = sys.argv[1:2]
    if c == ['blink']: cmd_blink(sys.argv[2], int(sys.argv[3]), int(sys.argv[4]))
    elif c == ['diff']: cmd_diff(sys.argv[2], sys.argv[3])
    elif c == ['noise']: cmd_noise(*sys.argv[2:5], *(float(x) for x in sys.argv[5:6]))
    elif c == ['cmp']:
        sep = sys.argv.index('--')
        cmd_cmp(sys.argv[2:sep], sys.argv[sep + 1:])
    else: sys.exit(__doc__)
