"""Inventory of the Maxwell shader code blocks inside the game's romfs (read-only).

Usage: shader_inventory.py <romfs dir> [--csv out.csv]

Finds every BNSH container (loose .bnsh*/.bnsh_vsh/.bnsh_fsh files and the ones embedded in .gfpak, .arc, .ptcl),
then every code block inside it, and counts how many are distinct by content. A code block starts with the NVN
header (word 0x12345678, then 0x30 bytes in all) followed by the shader program header (SPH), whose type field
(bits 10..13 of the first word) names the stage: 1 vertex, 5 fragment (runtime/nvn/objects.c, set_shaders).

The block length is an estimate, not read from the container's `grsc` tables: blocks are stored one after the other,
so a block runs to the next marker, and the last one to the end of the code region (the container's name/relocation
offsets, whichever comes first). Identical code with different neighbours can count twice, so the distinct numbers
are an upper bound; the container counts (distinct whole containers) are a looser one. Nothing is written but the
optional CSV (one row per distinct block: hash, stage, bytes, where it was first seen, how many times).
"""
import collections
import hashlib
import os
import struct
import sys

SIG = b'BNSH\x00\x00\x00\x00\x05\x01\x02\x00'  # magic, pad, version 5.1.2.0 (all of the game's containers)
MARK = struct.pack('<I', 0x12345678)
STAGES = {1: 'vertex', 5: 'fragment', 2: 'tess_ctrl', 3: 'tess_eval', 4: 'geometry'}


def find_all(data, needle, start=0, end=None):
    end = len(data) if end is None else end
    i = data.find(needle, start, end)
    while i != -1:
        yield i
        i = data.find(needle, i + 1, end)


def blocks_of(data, base, size):
    """Yield (offset, length) of each code block of the container at data[base:base + size]."""
    name_off, reloc_off = struct.unpack_from('<I', data, base + 0x10)[0], struct.unpack_from('<I', data, base + 0x18)[0]
    marks = [m for m in find_all(data, MARK, base, base + size) if (m - base) % 0x100 == 0]
    for k, m in enumerate(marks):
        if k + 1 < len(marks):
            end = marks[k + 1]
        else:
            tails = [base + o for o in (name_off, reloc_off) if m < base + o <= base + size]
            end = min(tails) if tails else base + size
        yield m, end - m


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    csv = sys.argv[sys.argv.index('--csv') + 1] if '--csv' in sys.argv else None
    if not args:
        sys.exit(__doc__)
    root = args[0]
    containers, distinct_containers = collections.Counter(), collections.defaultdict(set)
    nblocks = collections.Counter()
    seen = {}  # block hash -> [stage, bytes, first source, count]
    for dp, _, names in os.walk(root):
        for name in names:
            path = os.path.join(dp, name)
            if os.path.getsize(path) > 400 * 1024 * 1024:
                continue
            with open(path, 'rb') as f:
                data = f.read()
            src = os.path.splitext(name)[1].lower() or '(none)'
            for base in find_all(data, SIG):
                if base + 0x20 > len(data):
                    continue
                size = struct.unpack_from('<I', data, base + 0x1C)[0]
                if not 0x100 < size < 16 * 1024 * 1024 or base + size > len(data):
                    continue
                if data[base + 0x60:base + 0x64] != b'grsc':  # (a real container: its shader block follows the header)
                    continue
                containers[src] += 1
                distinct_containers[src].add(hashlib.sha1(data[base:base + size]).digest())
                for off, length in blocks_of(data, base, size):
                    sph0 = struct.unpack_from('<I', data, off + 0x30)[0] if off + 0x34 <= len(data) else 0
                    stage = STAGES.get((sph0 >> 10) & 0xF, 'compute/other')
                    h = hashlib.sha1(data[off:off + length]).hexdigest()[:16]
                    nblocks[src] += 1
                    if h in seen:
                        seen[h][3] += 1
                    else:
                        seen[h] = [stage, length, os.path.relpath(path, root), 1]
    print(f'containers: {sum(containers.values())} total, '
          f'{len(set().union(*distinct_containers.values())) if distinct_containers else 0} distinct')
    for src, n in containers.most_common():
        print(f'  {src:10s} containers {n:6d} (distinct {len(distinct_containers[src]):6d})  code blocks {nblocks[src]:6d}')
    by_stage = collections.Counter(v[0] for v in seen.values())
    print(f'code blocks: {sum(nblocks.values())} total, {len(seen)} distinct, '
          f'{sum(v[1] for v in seen.values()) / 1e6:.1f} MB distinct (estimate, see the docstring)')
    for stage, n in by_stage.most_common():
        mb = sum(v[1] for v in seen.values() if v[0] == stage) / 1e6
        print(f'  {stage:14s} {n:6d} blocks  {mb:7.1f} MB')
    if csv:
        with open(csv, 'w') as f:
            f.write('hash,stage,bytes,first_source,count\n')
            for h, (stage, length, src, n) in sorted(seen.items()):
                f.write(f'{h},{stage},{length},{src},{n}\n')


if __name__ == '__main__':
    main()
