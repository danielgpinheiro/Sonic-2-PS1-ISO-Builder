#!/usr/bin/python3
"""Stage tile sets for the RSDKv4 (Sonic 2) PS1 port: 16x16Tiles.gif -> 16x16Tiles.vram (build time).

Copied from RSDKv3-ps1 (Sonic CD); v4's LoadStageGIFFile (RSDKv4/Scene.cpp) does the same as v3's. The output goes
to a separate data tree (the disc tree has no GIFs), only for the stages it holds (ZoneM is not on the disc).
What upstream LoadStageGIFFile does at runtime, done here once:
  - decode the 16x16384 GIF (1024 tiles of 16x16, 8-bit master-palette indices);
  - pixels equal to the first pixel's value become 0 (transparent), as its loop over TILESET_SIZE;
  - the GIF's colour table: entries 0x80-0xFF are copied into the active palette
    (SetPaletteEntry(-1, c, r, g, b)); the PS1 load path applies them from the .vram header.
.vram layout (the RSDKv2 port's format, PS1UploadTileSet): u16 BE width 256, u16 BE height 1024,
768-byte palette (the GIF's, 256 x RGB), then 4 texture pages of 256x256 8-bit texels (65,536 B
each, row-major); page p holds tiles 256p..256p+255 in a 16x16 grid, tile n at texel
(16*(n%16), 16*(n/16)): one DMA per page (128 halfwords x 256 lines), no CPU re-pack at runtime.
The height field is the number of lines stored (docs/30 phase 9.3): the pages end at the last 16-line tile row with a
non-zero texel (the unused tile slots are empty: Zone01's last 120 KB); the loader uploads zeros for the rest, so VRAM
holds exactly the full pages (animated tiles copy from those slots).
Self-check: every page is unpacked back to the tile strip and compared with the fixed-up GIF pixels.
A stage that never draws a tile layer (every Act*.bin's active layers are 9 and no script of the stage writes
stage.activeLayer with anything but the constant 9: the special stage) gets the header (height 0) + palette only:
its 256 KB of pages would be read at every load for nothing (docs/30 phase 7.3).

Usage: convert_tiles.py DATA_DIR [OUT_DATA_DIR]   (every DATA_DIR/Stages/*/16x16Tiles.gif; with OUT_DATA_DIR, the
       .vram goes to OUT_DATA_DIR/Stages/<stage>/ for the stages that directory has)
"""
import glob, os, struct, sys
import numpy as np
from PIL import Image

PAGES, TILES_PER_PAGE = 4, 256


def load(path):
    im = Image.open(path)
    if im.mode != 'P' or im.size != (16, 16 * PAGES * TILES_PER_PAGE):
        raise ValueError('%s: expected a 16x16384 paletted GIF, got %s %s' % (path, im.mode, im.size))
    px = np.frombuffer(im.tobytes(), np.uint8).copy()
    px[px == px[0]] = 0  # upstream: transparent = tilesetGFXData[0]
    pal = bytes((im.getpalette() or [])[:768]).ljust(768, b'\0')
    return px, pal


def pack(px):
    # strip: tile t = rows 16t..16t+15; page p, tile n = t - 256p at (n%16, n//16) in the page
    strip = px.reshape(PAGES * TILES_PER_PAGE, 16, 16)
    pages = np.zeros((PAGES, 256, 256), np.uint8)
    for t in range(PAGES * TILES_PER_PAGE):
        p, n = divmod(t, TILES_PER_PAGE)
        tx, ty = (n % 16) * 16, (n // 16) * 16
        pages[p, ty:ty + 16, tx:tx + 16] = strip[t]
    return pages


def unpack(pages):
    out = np.zeros((PAGES * TILES_PER_PAGE, 16, 16), np.uint8)
    for t in range(PAGES * TILES_PER_PAGE):
        p, n = divmod(t, TILES_PER_PAGE)
        tx, ty = (n % 16) * 16, (n // 16) * 16
        out[t] = pages[p, ty:ty + 16, tx:tx + 16]
    return out.reshape(-1)


def uses_tile_layers(src_stage_dir, out_data, stage):
    """False only when the stage provably never activates a tile layer (see the module doc)."""
    for f in sorted(os.listdir(src_stage_dir)):
        if f.startswith('Act') and f.endswith('.bin'):
            a = open(os.path.join(src_stage_dir, f), 'rb').read()
            if tuple(a[1 + a[0]:5 + a[0]]) != (9, 9, 9, 9):
                return True
    bc = os.path.join(os.path.dirname(os.path.normpath(out_data)), 'Bytecode', stage + '.bin')
    if not os.path.exists(bc):
        return True
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, os.path.join(here, '..'))
    sys.path.insert(0, os.path.join(here, '..', 'scripts'))
    import bytecode_scan as bs
    import patch_bytecode as pb
    names, vars_ = bs.tables(3)
    code, _, subs, _, fns, _ = bs.full_code(bc)
    for p in [p for sub in subs for p in sub if p not in (0x3FFFF, 0x3FFF) and p >= 0] + [c for c, _ in fns]:
        try:
            lines = pb.listing(pb.instructions(code, p, names, vars_))
        except Exception:
            return True  # unreadable: assume it draws tiles
        for l in lines:
            w = l.split()
            if len(w) > 1 and w[1].startswith('STAGEACTIVELAYER') and not (w[0] == 'Equal' and w[2:] == ['9']):
                return True
    return False


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    out = sys.argv[2] if len(sys.argv) == 3 else None
    n = 0
    for gif in sorted(glob.glob(os.path.join(sys.argv[1], 'Stages', '*', '16x16Tiles.gif'))):
        dest = os.path.dirname(gif) if out is None else os.path.join(out, 'Stages', os.path.basename(os.path.dirname(gif)))
        if not os.path.isdir(dest):
            continue
        px, pal = load(gif)
        if out is not None and not uses_tile_layers(os.path.dirname(gif), out, os.path.basename(dest)):
            open(os.path.join(dest, '16x16Tiles.vram'), 'wb').write(struct.pack('>HH', 256, 0) + pal)
            print('tiles: %s draws no tile layer: palette only' % os.path.basename(dest))
            continue
        pages = pack(px)
        if not np.array_equal(unpack(pages), px):
            sys.exit('self-check failed: %s' % gif)
        data = pages.tobytes()
        rows = [r for r in range(PAGES * 16) if any(data[r * 4096:(r + 1) * 4096])]
        height = 16 * (rows[-1] + 1 if rows else 1)
        blob = struct.pack('>HH', 256, height) + pal + data[:height * 256]
        full = blob[4 + 768:] + bytes(PAGES * 65536 - height * 256)
        back = np.frombuffer(full, np.uint8).reshape(PAGES, 256, 256)
        if blob[4:4 + 768] != pal or not np.array_equal(unpack(back), px) or full != data:
            sys.exit('self-check (file) failed: %s' % gif)
        open(os.path.join(dest, '16x16Tiles.vram'), 'wb').write(blob)
        n += 1
    print('tiles: %d stages -> 16x16Tiles.vram (up to %d bytes: pages end at the last non-empty tile row), self-check exact'
          % (n, 4 + 768 + PAGES * 65536))


if __name__ == '__main__':
    main()
