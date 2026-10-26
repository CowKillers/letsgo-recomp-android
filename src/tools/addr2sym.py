"""addr2sym.py <guest addr>... : module, nearest symbol and disassembly around each guest address (debug aid).
Module bases as in runtime/rt.h."""
import bisect
import sys

import capstone

from nso import Nso

MODS = [('main', 0x7000000000), ('sdk', 0x7004000000), ('subsdk0', 0x7006000000), ('subsdk1', 0x7008000000)]
_cache = {}


def mod_for(addr):
    for name, base in MODS:
        if name not in _cache:
            n = Nso(f'../extracted/exefs/{name}')
            syms = sorted((base + v, s) for s, info, sh, v, sz in n.syms if sh and s)
            _cache[name] = (n, base, syms, [a for a, _ in syms])
        n, base, syms, keys = _cache[name]
        if base <= addr < base + n.size:
            return name, n, base, syms, keys
    return None


def main():
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    ctx = int(sys.argv[sys.argv.index('-d') + 1], 0) if '-d' in sys.argv else 0
    for a in [int(x, 0) for x in sys.argv[1:] if x != '-d' and not (sys.argv.index(x) > 0 and sys.argv[sys.argv.index(x) - 1] == '-d')]:
        m = mod_for(a)
        if not m:
            print(f'{a:#x}: outside the modules')
            continue
        name, n, base, syms, keys = m
        i = bisect.bisect_right(keys, a) - 1
        sym = f'{syms[i][1]}+{a - syms[i][0]:#x}' if i >= 0 else '?'
        print(f'{a:#x}: {name}+{a - base:#x}  {sym}')
        if ctx:
            off = a - base - ctx
            for ins in md.disasm(bytes(n.image[off:off + 2 * ctx + 4]), base + off):
                print(f'   {"=>" if ins.address == a else "  "} {ins.address:#x} {ins.mnemonic} {ins.op_str}')


if __name__ == '__main__':
    main()
