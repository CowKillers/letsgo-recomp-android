"""rdc_draw.py: one skinned draw of a RenderDoc capture, vertex by vertex (docs/PLANO-3D.md, fase 3).

Runs inside qrenderdoc like tools/rdc_skin.py (USERPROFILE pointed elsewhere):
  env RDC=<capture.rdc> EID=<event id> OUT=<text file> [N=64]

For the draw's first N distinct vertices: the vertex bytes the GPU read (bone indices a11, weights a12) and the
gl_Position it produced, so collapsed vertices (w == 1, all on one point) can be told apart from good ones by their
inputs. Then RenderDoc's shader debugger runs one collapsed and one good vertex and lists every variable change of
the collapsed one that differs between them.
"""
import os
import struct
import traceback

import renderdoc as rd

RDC = os.environ['RDC']
EID = int(os.environ['EID'])
N = int(os.environ.get('N', '64'))
out = open(os.environ.get('OUT', 'build/rdc/draw.txt'), 'w')


def p(*a):
    out.write(' '.join(str(x) for x in a) + '\n')
    out.flush()


def find(actions, eid):
    for a in actions:
        if a.eventId == eid:
            return a
        r = find(a.children, eid)
        if r:
            return r
    return None


def var_str(v):
    try:
        if v.type in (rd.VarType.UInt, rd.VarType.SInt, rd.VarType.Bool):
            return '[' + ' '.join(str(v.value.u32v[i]) for i in range(v.columns)) + ']'
        return '[' + ' '.join('%.4g' % v.value.f32v[i] for i in range(v.columns)) + ']'
    except Exception:
        return '?'


def trace(ctl, vtx, idx):
    """{variable name: [values in order]} of the shader debugger run of one vertex, and its inputs"""
    t = ctl.DebugVertex(vtx, 0, idx, 0)
    if not t or not t.debugger:
        return None, None
    inputs = {v.name: var_str(v) for v in t.inputs}
    seq = []
    while True:
        states = ctl.ContinueDebug(t.debugger)
        if not states:
            break
        for s in states:
            for c in s.changes:
                seq.append((c.after.name, var_str(c.after)))
    ctl.FreeTrace(t)
    return inputs, seq


def main():
    cap = rd.OpenCaptureFile()
    if cap.OpenFile(RDC, '', None) != rd.ResultCode.Succeeded:
        p('cannot open', RDC)
        return
    res, ctl = cap.OpenCapture(rd.ReplayOptions(), None)
    a = find(ctl.GetRootActions(), EID)
    ctl.SetFrameEvent(EID, True)
    pipe = ctl.GetPipelineState()
    vins = pipe.GetVertexInputs()
    vb = pipe.GetVBuffers()[0]
    off = {v.name: v.byteOffset for v in vins}
    ib = pipe.GetIBuffer()
    w = ib.byteStride or 2
    raw = ctl.GetBufferData(ib.resourceId, ib.byteOffset + a.indexOffset * w, a.numIndices * w)
    idx = struct.unpack('<%d%s' % (a.numIndices, {1: 'B', 2: 'H', 4: 'I'}[w]), raw[:a.numIndices * w])
    mo = ctl.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    st = mo.vertexByteStride
    if mo.indexResourceId != rd.ResourceId.Null() and mo.indexByteStride:
        mw = mo.indexByteStride
        mraw = ctl.GetBufferData(mo.indexResourceId, mo.indexByteOffset, a.numIndices * mw)
        midx = [i + mo.baseVertex for i in struct.unpack('<%d%s' % (a.numIndices, {1: 'B', 2: 'H', 4: 'I'}[mw]), mraw)]
    else:
        midx = list(range(a.numIndices))
    pdata = ctl.GetBufferData(mo.vertexResourceId, mo.vertexByteOffset, (max(midx) + 1) * st)
    p('eid %d indices %d baseVertex %d vb off %d stride %d; inputs %s' % (
        EID, a.numIndices, a.baseVertex, vb.byteOffset, vb.byteStride, off))
    refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
    for i, cb in enumerate(refl.constantBlocks):  # declared size vs the range bound for this draw
        try:
            b = pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
            d = getattr(b, 'descriptor', b)
            res = getattr(d, 'resource', None) or getattr(d, 'resourceId', None)
            p('  cbuf %d %s: declared %d bytes, bound %s +%d size %d' % (
                i, cb.name, cb.byteSize, res, d.byteOffset, d.byteSize))
        except Exception:
            p('  cbuf %d %s: %s' % (i, cb.name, traceback.format_exc().splitlines()[-1]))
    seen = {}
    for i, (v, m) in enumerate(zip(idx, midx)):
        if v not in seen:
            seen[v] = struct.unpack_from('4f', pdata, m * st)
    top = max(seen) + 1
    vdata = ctl.GetBufferData(vb.resourceId, vb.byteOffset + a.baseVertex * vb.byteStride, top * vb.byteStride)
    ws = sorted(g[3] for g in seen.values())
    med = ws[len(ws) // 2]
    bad = [v for v in sorted(seen) if abs(seen[v][3] - med) > 0.5 * abs(med)]
    p('%d vertices, median w %.2f, %d bad (w far from the median)' % (len(seen), med, len(bad)))

    def row(v):
        r = vdata[v * vb.byteStride:(v + 1) * vb.byteStride]
        b11 = list(r[off['a11']:off['a11'] + 4]) if 'a11' in off else None
        b12 = list(r[off['a12']:off['a12'] + 4]) if 'a12' in off else None
        g = seen[v]
        return '  v%-5d a11 %-18s a12 %-20s -> (%.3f %.3f %.3f %.3f)' % (v, b11, b12, g[0], g[1], g[2], g[3])
    p('bad:')
    for v in bad[:N]:
        p(row(v))
    p('good with more than one bone:')
    multi = [v for v in sorted(seen) if v not in set(bad) and
             'a12' in off and vdata[v * vb.byteStride + off['a12'] + 1] > 0]
    for v in multi[:N]:
        p(row(v))
    # bundle for tools/skintest (make_harness.py): the draw's inputs exactly as bound on the GPU
    d = os.path.join(os.path.dirname(os.environ.get('OUT', 'build/rdc/x')), 'draw_%d' % EID)
    os.makedirs(d, exist_ok=True)
    open(os.path.join(d, 'verts.bin'), 'wb').write(vdata)
    gp = [seen.get(v, (0.0, 0.0, 0.0, 0.0)) for v in range(top)]
    open(os.path.join(d, 'gpu_pos.bin'), 'wb').write(b''.join(struct.pack('4f', *g) for g in gp))
    nvn = {'R32G32B32_FLOAT': 0x22, 'R32G32B32A32_FLOAT': 0x2e, 'R32G32_FLOAT': 0x16, 'R16G16B16A16_FLOAT': 0x29,
           'R16G16_FLOAT': 0x11, 'R8G8B8A8_UINT': 0x27, 'R8G8B8A8_UNORM': 0x25}
    with open(os.path.join(d, 'info.txt'), 'w') as f:
        f.write('vs 0 stride %d\n' % vb.byteStride)
        for v in vins:
            if v.vertexBuffer == 0 and v.name.startswith('a'):
                try:
                    fn = v.format.Name()
                except Exception:
                    fn = ''
                f.write('attr %s fmt %#x off %d\n' % (v.name[1:], nvn.get(fn, 0), v.byteOffset))
        for i, cb in enumerate(refl.constantBlocks):
            num = ''.join(ch for ch in cb.name if ch.isdigit())
            b = pipe.GetConstantBlock(rd.ShaderStage.Vertex, i, 0)
            dd = getattr(b, 'descriptor', b)
            res = getattr(dd, 'resource', None) or getattr(dd, 'resourceId', None)
            data = ctl.GetBufferData(res, dd.byteOffset, min(dd.byteSize, cb.byteSize or dd.byteSize))
            open(os.path.join(d, 'cb%s.bin' % num), 'wb').write(data)
            f.write('cb %s size %d\n' % (num, len(data)))
    p('exported to', d)
    ctl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    p(traceback.format_exc())
out.close()
os._exit(0)
