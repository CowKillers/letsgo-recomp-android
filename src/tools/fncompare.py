"""fncompare.py: which game functions an event runs, against runs without it (LARECOMP_FNPROF profiles).

  fncompare.py --base a.txt [b.txt ...] --event NAME=x.txt [--event NAME2=y.txt] [--top N]

Profiles are build/fnprof*.txt from runtime/fnprof.c (addr count first-entry-order first-caller-lr). For each event:
the functions it ran that no base run ran, in order of first entry, with the function that first called them (from
the guest lr) and the nearest exported symbol. With two events: the functions both ran (the shared part, e.g.
"start an encounter") and those only one of them ran (where they part ways).
"""
import argparse
import bisect
import glob
import os
import re

GEN = os.path.join(os.path.dirname(__file__), '..', 'generated')


def load(path):
    rows = {}
    for line in open(path):
        p = line.split()
        if len(p) >= 4:
            rows[int(p[0], 16)] = (int(p[1]), int(p[2]), int(p[3], 16))
        elif len(p) >= 2:
            rows[int(p[0], 16)] = (int(p[1]), 0, 0)
    return rows


_starts = None


def func_of(addr):
    """the generated function holding a guest address"""
    global _starts
    if _starts is None:
        s = set()
        for f in glob.glob(os.path.join(GEN, '*', '*.c')):
            for m in re.finditer(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{', open(f, encoding='utf-8', errors='replace').read(), re.M):
                s.add(int(m.group(1), 16))
        _starts = sorted(s)
    i = bisect.bisect_right(_starts, addr) - 1
    return _starts[i] if i >= 0 else 0


def mod(addr):
    for name, base in (('subsdk1', 0x7008000000), ('subsdk0', 0x7006000000), ('sdk', 0x7004000000), ('main', 0x7000000000)):
        if addr >= base:
            return '%s+%x' % (name, addr - base)
    return hex(addr)


def show(title, rows, keys, top):
    print('\n== %s: %d functions (first %d by first entry)' % (title, len(keys), min(top, len(keys))))
    for a in sorted(keys, key=lambda k: rows[k][1])[:top]:
        n, first, lr = rows[a]
        caller = func_of(lr) if lr else 0
        print('  %-18s %x  calls %-7d first #%-7d from %x (in %x %s)' % (mod(a), a, n, first, lr, caller, mod(caller) if caller else ''))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--base', nargs='+', required=True)
    ap.add_argument('--event', action='append', required=True)
    ap.add_argument('--top', type=int, default=40)
    a = ap.parse_args()
    base = set()
    for b in a.base:
        base |= set(load(b))
    events = []
    for e in a.event:
        name, path = e.split('=', 1)
        rows = load(path)
        events.append((name, rows, set(rows) - base))
    for name, rows, only in events:
        print('%s: %d functions, %d not in the base runs' % (name, len(rows), len(only)))
    if len(events) == 2:
        (n1, r1, o1), (n2, r2, o2) = events
        show('both %s and %s (shared)' % (n1, n2), r1, o1 & o2, a.top)
        show('only %s' % n1, r1, o1 - o2, a.top)
        show('only %s' % n2, r2, o2 - o1, a.top)
    else:
        for name, rows, only in events:
            show(name, rows, only, a.top)


if __name__ == '__main__':
    main()
