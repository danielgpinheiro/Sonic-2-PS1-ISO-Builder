#!/usr/bin/env python3
"""The stages whose scripts use the Scene3D buffers (docs/30, before phase 9): Data/Game/PS1Scene3D.bin.

The PS1 allocates the Scene3D arrays (faces, vertices, draw list, ~85 KB: RSDKv4/Scene3D.cpp PS1Scene3DPrepare) only
while such a stage is loaded. A stage is listed when its loaded bytecode (its file + GlobalCode when its StageConfig
loads the global objects: bytecode_scan.full_code) reads or writes a vertex / face buffer or count variable, or calls
TransformVertices / Draw3DScene. Sonic 2: the special stage only.

File: u8 count, then per stage u8 length + the folder name (ASCII).
Usage: scene3d_stages.py BYTECODE_DIR DATA_DIR   (the disc tree's Bytecode/ and Data/)
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import bytecode_scan as bs  # noqa: E402
import patch_bytecode as pb  # noqa: E402

VARS = ('VERTEXBUFFER', 'FACEBUFFER', 'SCENE3DVERTEXCOUNT', 'SCENE3DFACECOUNT')
FUNCS = ('TransformVertices', 'Draw3DScene')


def uses_scene3d(path, names, vars_):
    code, _, subs, _, fns, _ = bs.full_code(path)
    starts = sorted({x for t in subs for x in t if 0 <= x < len(code)} | {c for c, _ in fns if 0 <= c < len(code)})
    for x in starts:
        try:
            ins = pb.instructions(code, x, names, vars_)
        except (IndexError, KeyError):
            return True  # unreadable: keep the buffers
        for line in pb.listing(ins):
            w = line.split()
            if w[0] in FUNCS or any(t.startswith(VARS) for t in w[1:]):
                return True
    return False


def main():
    bcdir, data = sys.argv[1], sys.argv[2]
    names, vars_ = bs.tables(3)
    stages = []
    for f in sorted(os.listdir(bcdir)):
        if f.endswith('.bin') and f != 'GlobalCode.bin' and uses_scene3d(os.path.join(bcdir, f), names, vars_):
            stages.append(f[:-4])
    out = bytearray([len(stages)])
    for s in stages:
        out += bytes([len(s)]) + s.encode('ascii')
    open(os.path.join(data, 'Game', 'PS1Scene3D.bin'), 'wb').write(out)
    print('scene3d: %s' % (' '.join(stages) or '-'))


if __name__ == '__main__':
    main()
