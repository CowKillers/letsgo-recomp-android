"""perf_summary.py [log]: frame rate and hitches of a play session from the renderer's 2-second stats lines.

  [vk] t=<s> frame <n> <fps> fps max <ms>ms ... (<draws> draws, <skipped> skipped, ... compile <n>, ...)

Prints the frame rate range, the windows with a frame longer than 50 ms (hitches), draws skipped while shaders were
still compiling (objects missing for a moment) and audio underruns. Default log: logs/jogo.log (jogar.bat).
"""
import re
import sys

STATS = re.compile(r't=([\d.]+) frame (\d+) ([\d.]+) fps max (\d+)ms.*?\((\d+) draws, (\d+) skipped.*?compile (\d+)')


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'logs/jogo.log'
    lines = open(path, errors='replace').read().splitlines()
    rows = [tuple(float(x) for x in m.groups()) for m in map(STATS.search, lines) if m]
    if not rows:
        print('no stats lines in', path)
        return
    steady = rows[2:] or rows  # (the first seconds: boot, shader compiles)
    fps = [r[2] for r in steady]
    print('%s: %.0f s of play, %d samples' % (path, rows[-1][0], len(rows)))
    print('fps: min %.1f, mean %.1f, max %.1f' % (min(fps), sum(fps) / len(fps), max(fps)))
    hitches = [(r[0], int(r[3])) for r in rows if r[3] > 50]
    print('windows with a frame over 50 ms: %d of %d %s' % (len(hitches), len(rows), hitches[:12]))
    skipped = [(r[0], int(r[5])) for r in rows if r[5] > 0]
    print('draws skipped while shaders compiled: %d in %d windows; shaders compiled: %d' %
          (sum(s for _, s in skipped), len(skipped), sum(int(r[6]) for r in rows)))
    under = [l for l in lines if 'underrun' in l.lower()]
    print('audio underrun lines: %d %s' % (len(under), under[-2:] if under else ''))


if __name__ == '__main__':
    main()
