"""addrmap.py <from main> <to main> <addr>... : the same code or data in another build of the game's main (Let's Go
Eevee <-> Pikachu, or an update). Addresses are guest addresses (main at 0x7000000000).

Code: the instructions from the address on are compared with their PC-relative and immediate offset fields masked
(branches, adr/adrp, literal loads, add/ldr/str immediates), growing the window until the match is unique.
Data (prefix d:, e.g. d:0x7000f7eeb8): the first adrp + add/ldr/str that computes it in <from> is mapped as code, and
the same pair is decoded in <to>.
"""
import struct
import sys

from nso import Nso

BASE = 0x7000000000


def norm_word(w):
    if (w & 0x7C000000) == 0x14000000: return w & 0xFC000000          # b / bl
    if (w & 0xFF000010) == 0x54000000: return w & 0xFF00001F          # b.cond
    if (w & 0x7E000000) == 0x34000000: return w & 0xFF00001F          # cbz / cbnz
    if (w & 0x7E000000) == 0x36000000: return w & 0xFFF8001F          # tbz / tbnz
    if (w & 0x1F000000) == 0x10000000: return w & 0x9F00001F          # adr / adrp
    if (w & 0x3B000000) == 0x18000000: return w & 0xFF00001F          # ldr literal
    if (w & 0x7F800000) == 0x11000000: return w & 0xFFC003FF          # add / sub immediate (adrp page offsets)
    if (w & 0x3B000000) == 0x39000000: return w & 0xFFC003FF          # ldr / str unsigned offset
    return w


class Image:
    def __init__(self, path):
        self.nso = Nso(path)
        off, size = self.nso.text
        self.text_off, self.text_size = off, size
        self.words = struct.unpack_from(f'<{size // 4}I', self.nso.image, off)
        self.norm = b''.join(struct.pack('<I', norm_word(w)) for w in self.words)

    def word(self, addr):
        return self.words[(addr - BASE - self.text_off) // 4]

    def find(self, pat):
        hits, i = [], self.norm.find(pat)
        while i >= 0:
            if i % 4 == 0: hits.append(BASE + self.text_off + i)
            i = self.norm.find(pat, i + 1)
        return hits


def map_code(a, b, addr):
    i = (addr - BASE - a.text_off) * 1
    for n in (8, 12, 16, 24, 32, 48, 64, 96, 128):
        pat = a.norm[i:i + 4 * n]
        if len(a.find(pat)) != 1: continue  # (not unique even in its own build: longer)
        hits = b.find(pat)
        if len(hits) == 1: return hits[0], n
        if not hits: return None, n
    return None, 0


def adrp_target(pc, w):
    imm = ((w >> 29) & 3) | (((w >> 5) & 0x7FFFF) << 2)
    if imm & (1 << 20): imm -= 1 << 21
    return (pc & ~0xFFF) + (imm << 12)


def lo12(w):
    if (w & 0x7F800000) == 0x11000000: return (w >> 10) & 0xFFF, (w >> 5) & 31  # add
    if (w & 0x3B000000) == 0x39000000: return ((w >> 10) & 0xFFF) << (w >> 30), (w >> 5) & 31  # ldr/str (scaled)
    return None, None


def data_refs(img, target):
    base = BASE + img.text_off
    for k, w in enumerate(img.words):
        if (w & 0x9F000000) != 0x90000000: continue  # adrp
        pc = base + 4 * k
        page = adrp_target(pc, w)
        if page != target & ~0xFFF: continue
        rd = w & 31
        for j in range(1, 6):
            if k + j >= len(img.words): break
            off, rn = lo12(img.words[k + j])
            if off is not None and rn == rd and page + off == target: return pc, j
    return None, None


def map_data(a, b, target):
    pc, j = data_refs(a, target)
    if pc is None: return None, 'no reference'
    pc2, n = map_code(a, b, pc)
    if pc2 is None: return None, f'reference {pc:#x} not found'
    w0, w1 = b.word(pc2), b.word(pc2 + 4 * j)
    off, _ = lo12(w1)
    return adrp_target(pc2, w0) + off, f'via {pc:#x} -> {pc2:#x}'


def main():
    a, b = Image(sys.argv[1]), Image(sys.argv[2])
    print(f'# {a.nso.build_id[:16]} -> {b.nso.build_id[:16]}')
    for arg in sys.argv[3:]:
        if arg.startswith('d:'):
            t = int(arg[2:], 0)
            r, why = map_data(a, b, t)
            print(f'{t:#x} -> {r:#x}  data {why}' if r else f'{t:#x} -> ?  data {why}')
        else:
            t = int(arg, 0)
            r, n = map_code(a, b, t)
            print(f'{t:#x} -> {r:#x}  ({n} instructions)' if r else f'{t:#x} -> ?  (not unique / not found)')


if __name__ == '__main__':
    main()
