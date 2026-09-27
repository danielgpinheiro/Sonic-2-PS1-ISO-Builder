#!/usr/bin/env python3
"""Phase 4.2 check: every VRAM block of the menu packs (tools/menu/build_menu.py) equals the VRAM dump.
Usage: check_menu_vram.py VRAM.bin PACK.pmn [PACK.pmn ...]"""
import struct, sys
import numpy as np

vram = np.frombuffer(open(sys.argv[1], 'rb').read(), '<u2').reshape(512, 1024)
ok = True
for path in sys.argv[2:]:
    d = open(path, 'rb').read()
    magic, ni, nf, ng, nb, ns, _, off = struct.unpack_from('<4sHHHHHHI', d, 0)
    p = 20 + ni * 12 + nf * 8 + ng * 8
    bad = 0
    for b in range(nb):
        x, y, w, h, o = struct.unpack_from('<HHHHI', d, p + 12 * b)
        blk = np.frombuffer(d, '<u2', w * h, off + o).reshape(h, w)
        bad += not np.array_equal(vram[y:y + h, x:x + w], blk)
    ok &= bad == 0
    print('%s: %d blocks, %d different' % (path.split('/')[-1], nb, bad))
print('OK' if ok else 'FAIL')
sys.exit(0 if ok else 1)
