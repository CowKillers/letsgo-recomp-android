"""xrefs.py: who calls / references what in the game's main module (debug aid, like addr2sym.py).

  xrefs.py calls <addr>      start of the function holding <addr>, and every bl to that start (with the caller's
                             function start and the 8 instructions before the call)
  xrefs.py imm <value>       instructions with that immediate (mov / cmp / add ...), e.g. 2 or 0x1e
  xrefs.py float <value>     32-bit float constants equal to <value> in the module (literal pools / data) and the
                             instructions that load them (adrp + ldr / add pairs)

The disassembly of main is cached in build/xrefs-main.pkl (the module never changes).
"""
import os
import pickle
import re
import struct
import sys

import capstone

from nso import Nso

BASE = 0x7000000000
CACHE = os.path.join(os.path.dirname(__file__), '..', 'build', 'xrefs-main.pkl')


def load():
    if os.path.exists(CACHE):
        with open(CACHE, 'rb') as f:
            return pickle.load(f)
    n = Nso('../extracted/exefs/main')
    data = bytes(n.image)
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    md.skipdata = True
    text_end = n.text[0] + n.text[1]
    ins = [(a, m, op) for a, _, m, op in md.disasm_lite(data[:text_end], BASE)]
    out = {'data': data, 'ins': ins}
    os.makedirs(os.path.dirname(CACHE), exist_ok=True)
    with open(CACHE, 'wb') as f:
        pickle.dump(out, f)
    return out


def func_start(ins, idx):
    """index of the first instruction of the function holding ins[idx]: just after the previous ret or
    unconditional b (approximate: a function with an early ret inside looks like two)"""
    j = idx
    while j > 0 and ins[j - 1][1] not in ('ret', 'b', 'br'):
        j -= 1
    return j


def index_of(ins, addr):
    lo, hi = 0, len(ins)
    while lo < hi:
        mid = (lo + hi) // 2
        if ins[mid][0] < addr:
            lo = mid + 1
        else:
            hi = mid
    return lo


def show(ins, a, b):
    for k in range(max(a, 0), min(b, len(ins))):
        print('   %x  %-6s %s' % ins[k])


def main():
    db = load()
    ins = db['ins']
    cmd, val = sys.argv[1], sys.argv[2]
    if cmd == 'calls':
        i = index_of(ins, int(val, 0))
        s = func_start(ins, i)
        start = ins[s][0]
        print('function start %x' % start)
        target = '#0x%x' % start
        for k, (a, m, op) in enumerate(ins):
            if m in ('bl', 'b') and op == target:
                cs = ins[func_start(ins, k)][0]
                print('\n%s from %x (function %x):' % (m, a, cs))
                show(ins, k - 8, k + 1)
    elif cmd == 'imm':
        v = int(val, 0)
        pat = re.compile(r'#(0x[0-9a-f]+|\d+)\b')
        for a, m, op in ins:
            if m in ('mov', 'movz', 'cmp', 'orr') and any(int(x, 0) == v for x in pat.findall(op)):
                print('%x  %-6s %s' % (a, m, op))
    elif cmd == 'float':
        f = struct.pack('<f', float(val))
        data = db['data']
        hits = [m.start() for m in re.finditer(re.escape(f), data) if m.start() % 4 == 0]
        print('%d copies of %s (%s)' % (len(hits), val, f.hex()))
        for h in hits[:40]:
            addr = BASE + h
            page, off = addr & ~0xfff, addr & 0xfff
            refs = []
            for k, (a, m, op) in enumerate(ins):
                if m == 'adrp' and op.endswith('#0x%x' % page):
                    for k2 in range(k + 1, min(k + 6, len(ins))):
                        if ('#0x%x]' % off) in ins[k2][2] or (off == 0 and ins[k2][2].endswith(']') and ins[k2][1].startswith('ldr')):
                            refs.append(ins[k2][0])
            print('  %x  refs: %s' % (addr, ' '.join('%x' % r for r in refs[:12])))


if __name__ == '__main__':
    main()
