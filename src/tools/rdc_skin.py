"""rdc_skin.py: what the GPU did with the skinned draws of RenderDoc captures (docs/PLANO-3D.md, fase 3).

Runs inside qrenderdoc (it carries the `renderdoc` module and its own Python 3.8). qrenderdoc hangs at start-up
when it finds the VS Code Python debugger in the user profile, so point USERPROFILE elsewhere:

  USERPROFILE=<empty dir> "/c/Program Files/RenderDoc/qrenderdoc.exe" --python tools/rdc_skin.py
  env: RDC_DIR=build/rdc (every .rdc in it), OUT=build/rdc/skin.txt

Captures come from `RENDERDOC=1 tools/repro.sh <name> <frame> LARECOMP_RENDERDOC_AT=<frame-10>` (under RenderDoc the
runtime also writes build/rdc-shaders.txt: the hash of each vertex shader's SPIR-V -> its guest address). For each draw whose vertex shader reads location 11
(bone indices) it lists the post-VS extent in NDC and how many vertices land far outside the screen ("exploded").
The first exploded draw of each vertex shader is exported to build/rdc/draw_<eid>/ in the layout of the renderer's
skin dump (info.txt, verts.bin, cbN.bin - see tools/skintest/make_harness.py) plus gpu_pos.bin (the GPU's gl_Position
of each vertex), and one exploded vertex is run through RenderDoc's shader debugger: its inputs as the shader saw them.
"""
import glob
import os
import struct
import traceback

import renderdoc as rd

RDC_DIR = os.environ.get('RDC_DIR', 'build/rdc')
OUT = os.environ.get('OUT', os.path.join(RDC_DIR, 'skin.txt'))
out = open(OUT, 'w')
NVN_FMT = {'R32G32B32_FLOAT': 0x22, 'R32G32B32A32_FLOAT': 0x2e, 'R32G32_FLOAT': 0x16, 'R16G16B16A16_FLOAT': 0x29,
           'R16G16_FLOAT': 0x11, 'R8G8B8A8_UINT': 0x27, 'R8G8B8A8_UNORM': 0x25, 'R16G16B16A16_UINT': 0x2c,
           'R32G32B32A32_UINT': 0x2f}


def p(*a):
    out.write(' '.join(str(x) for x in a) + '\n')
    out.flush()


def fmt_name(f):
    try:
        return f.Name()
    except Exception:
        return str(f)


def walk(actions):
    for a in actions:
        if a.flags & rd.ActionFlags.Drawcall:
            yield a
        yield from walk(a.children)


SHADERS = {}  # FNV-1a 64 of the SPIR-V -> guest address (the runtime writes build/rdc-shaders.txt under RenderDoc)
try:
    for line in open(os.environ.get('SHADER_MAP', 'build/rdc-shaders.txt')):
        h, _, addr = line.split()
        SHADERS[int(h, 16)] = addr
except OSError:
    pass


def shader_name(refl):
    h = 0xcbf29ce484222325
    for b in bytes(refl.rawBytes):
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return 'vs_' + SHADERS.get(h, '?%016x' % h)


def cbuf(ctl, pipe, refl, i):
    """bytes of constant block i of the vertex shader as bound for this draw"""
    b = pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
    d = getattr(b, 'descriptor', b)
    res = getattr(d, 'resource', None) or getattr(d, 'resourceId', None)
    off, size = d.byteOffset, d.byteSize
    size = min(size, refl.constantBlocks[i].byteSize or size, 65536)
    return ctl.GetBufferData(res, off, size)


def indices_of(ctl, pipe, a):
    """the vertex index of each of the draw's indices (no base vertex)"""
    if not (a.flags & rd.ActionFlags.Indexed):
        return list(range(a.vertexOffset, a.vertexOffset + a.numIndices))
    ib = pipe.GetIBuffer()
    w = ib.byteStride or 2
    raw = ctl.GetBufferData(ib.resourceId, ib.byteOffset + a.indexOffset * w, a.numIndices * w)
    f = {1: 'B', 2: 'H', 4: 'I'}[w]
    return list(struct.unpack('<%d%s' % (len(raw) // w, f), raw))


def post_vs(ctl, n):
    """gl_Position of each of the draw's n indices"""
    mo = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    st = mo.vertexByteStride
    if mo.indexResourceId != rd.ResourceId.Null() and mo.indexByteStride:
        w = mo.indexByteStride
        raw = ctl.GetBufferData(mo.indexResourceId, mo.indexByteOffset, n * w)
        idx = struct.unpack('<%d%s' % (len(raw) // w, {1: 'B', 2: 'H', 4: 'I'}[w]), raw)
        idx = [i + mo.baseVertex for i in idx]
    else:
        idx = list(range(n))
    top = max(idx) + 1 if idx else 0
    data = ctl.GetBufferData(mo.vertexResourceId, mo.vertexByteOffset, top * st)
    pos = [struct.unpack_from('4f', data, i * st) if (i + 1) * st <= len(data) else (0, 0, 0, 1) for i in range(top)]
    return [pos[i] for i in idx]


def exploded(v):
    x, y, z, w = v
    if abs(w) < 1e-6:
        return abs(x) + abs(y) > 1e-3
    return abs(x / w) > 4 or abs(y / w) > 4


def export(ctl, pipe, refl, a, vins, vb, verts, gpos, name):
    d = os.path.join(RDC_DIR, 'draw_%d' % a.eventId)
    os.makedirs(d, exist_ok=True)
    stride = vb.byteStride
    top = min(max(verts) + 1, 8192)
    raw = ctl.GetBufferData(vb.resourceId, vb.byteOffset + max(a.baseVertex, 0) * stride, top * stride)
    open(os.path.join(d, 'verts.bin'), 'wb').write(raw)
    pos = [(0.0, 0.0, 0.0, 0.0)] * top
    for v, g in zip(verts, gpos):
        if v < top:
            pos[v] = g
    open(os.path.join(d, 'gpu_pos.bin'), 'wb').write(b''.join(struct.pack('4f', *g) for g in pos))
    with open(os.path.join(d, 'info.txt'), 'w') as f:
        f.write('vs %s stride %d\n' % (name.replace('vs_', ''), stride))
        for v in vins:
            if v.vertexBuffer == 0 and v.name.startswith('a'):
                f.write('attr %s fmt %#x off %d\n' % (v.name[1:], NVN_FMT.get(fmt_name(v.format), 0), v.byteOffset))
        for i, cb in enumerate(refl.constantBlocks):
            data = cbuf(ctl, pipe, refl, i)
            num = cb.name.upper().replace('CB', '')
            if num.isdigit():
                open(os.path.join(d, 'cb%s.bin' % num), 'wb').write(data)
                f.write('cb %s size %d\n' % (num, len(data)))
    return d


def debug_vertex(ctl, a, vtx, idx):
    try:
        trace = ctl.DebugVertex(vtx, 0, idx, 0)
    except TypeError:
        trace = ctl.DebugVertex(vtx, 0, idx)
    if not trace or not trace.debugger:
        p('    (shader debugging unavailable for this draw)')
        return
    for v in trace.inputs:
        vals = [v.value.u32v[i] for i in range(v.columns)] if 'a11' in v.name or 'a7' in v.name else \
            ['%.4f' % v.value.f32v[i] for i in range(v.columns)]
        p('    input %s = %s' % (v.name, vals))
    ctl.FreeTrace(trace)


def analyse(path):
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(path, '', None) != rd.ResultCode.Succeeded:
        p('cannot open', path)
        return
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res != rd.ResultCode.Succeeded:
        p('cannot replay', path, res)
        return
    draws = list(walk(ctl.GetRootActions()))
    p('\n#### %s: %d draws' % (os.path.basename(path), len(draws)))
    seen = set()
    for a in draws:
        ctl.SetFrameEvent(a.eventId, True)
        pipe = ctl.GetPipelineState()
        refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
        if not refl or not any(s.regIndex == 11 for s in refl.inputSignature):
            continue
        name = shader_name(refl)
        vbs = pipe.GetVBuffers()
        vins = pipe.GetVertexInputs()
        try:
            verts = indices_of(ctl, pipe, a)
            gpos = post_vs(ctl, a.numIndices)
        except Exception:
            p('  eid %d %s: %s' % (a.eventId, name, traceback.format_exc().splitlines()[-1]))
            continue
        bad = [i for i, g in enumerate(gpos) if exploded(g)]
        ndc = [(g[0] / g[3], g[1] / g[3]) for g in gpos if abs(g[3]) > 1e-6]
        ext = (min(x for x, _ in ndc), max(x for x, _ in ndc), min(y for _, y in ndc), max(y for _, y in ndc)) \
            if ndc else (0, 0, 0, 0)
        rt = pipe.GetOutputTargets()
        p('  eid %5d %-16s idx %5d vb+%-9d stride %d ndc x[%.2f %.2f] y[%.2f %.2f] w[%.2f %.2f] exploded %d/%d' % (
            a.eventId, name, a.numIndices, vbs[0].byteOffset if vbs else -1, vbs[0].byteStride if vbs else 0,
            ext[0], ext[1], ext[2], ext[3], min(g[3] for g in gpos), max(g[3] for g in gpos), len(bad), len(gpos)))
        if bad and name not in seen and vbs:
            seen.add(name)
            d = export(ctl, pipe, refl, a, vins, vbs[0], verts, gpos, name)
            i = bad[0]
            v = verts[i]
            p('    exported to %s; shader debugger on vertex %d (index #%d), GPU gl_Position %s' % (
                d, v, i, ['%.3f' % x for x in gpos[i]]))
            debug_vertex(ctl, a, v, i)
    ctl.Shutdown()
    cap.Shutdown()


try:
    files = sorted(glob.glob(os.path.join(RDC_DIR, '*.rdc')))
    p('captures:', files)
    for f in files:
        analyse(f)
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
