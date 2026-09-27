#!/usr/bin/python3
"""Disc load order for Sonic 2 (docs/30 phase 7.3 / 9): tools/disc/load_order.txt, which build_disc.py lays out first.

Every psyqo CD request costs a seek (~0.2 s in PCSX-Redux, more on a console) on top of 6.7 ms per sector at 2x, so a
load's time is mostly its request count; the read-ahead windows (ps1/cdrom_fs, fed by the load stream) serve the next
file when it follows on the disc. Order:
  1. the boot's files, in the order the natural build opens them (BOOT_TRACE: `OPEN <path>` lines from a GDB trace of
     fopen, tools/disc/boot_trace.txt), up to the first stage load;
  2. per stage (GameConfig's lists: presentation, regular, special, bonus; a folder once), what LoadStageFiles opens,
     in its order (traced on Special and Zone01, docs/30): StageConfig.bin, the stage-local copy of GameConfig.bin
     (tools/disc/stage_local.py, phase 9.2), the script image Scripts.ps1 (phase 9.3; after the SFX bank when the stage
     doesn't load the global scripts), PS1Sfx.bin, 16x16Tiles.vram, the Sonic
     atlas + pictures, BGStrips.bin, CollisionMasks.ps1, Backgrounds.bin, 128x128Tiles.ps1, the acts, the local copies
     of the players' animations and the palettes (the startups open them); then the other characters' atlases / strips (a Tails load
     skips forward over Sonic's);
  3. the rest of the small data by path (build_disc.py adds every remaining file after these).
Usage: gen_load_order.py ISO_ROOT [BOOT_TRACE] [OUT]
"""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'atlas'))
import object_sheets  # noqa: E402
import stage_local  # noqa: E402

VARIANTS = ('', '_t', '_k', '_st')


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'build', 'iso')
    trace = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, 'boot_trace.txt')
    out_path = sys.argv[3] if len(sys.argv) > 3 else os.path.join(HERE, 'load_order.txt')
    manifests = os.path.join(os.path.dirname(os.path.abspath(iso)), 'manifest')  # <build>/iso -> <build>/manifest (port or builder)
    data = os.path.join(iso, 'Data')
    have = {}
    for d, _, names in os.walk(iso):
        for n in names:
            p = os.path.relpath(os.path.join(d, n), iso)
            have[p.lower()] = p
    order, seen = [], set()

    def add(p):
        q = have.get(p.lower())
        if q and q not in seen:
            seen.add(q)
            order.append(q)

    if os.path.exists(trace):  # 1. the boot, up to the first stage file
        for line in open(trace):
            w = line.split()
            if len(w) >= 2 and w[0] == 'OPEN':
                if w[1].startswith('Data/Stages/'):
                    break
                add(w[1])
    lists = object_sheets.game_config(data)[3]
    folders = []
    for lst in lists:
        for folder, _, _ in lst:
            if folder not in folders:
                folders.append(folder)
    for folder in folders:  # 2. each stage as LoadStageFiles opens it
        s = 'Data/Stages/%s/' % folder
        if s.lower() + 'stageconfig.bin' not in have:
            continue
        local = [s + 'PS1Local/' + os.path.basename(p) for p in stage_local.shared_files(iso, manifests, folder)]
        add(s + 'StageConfig.bin')
        early = ('GameConfig.bin',) # read before the SFX bank; the animations / palettes by the startups
        for p in local:
            if os.path.basename(p) in early:
                add(p)
        # the script image (phase 9.3) is read at the first LoadBytecode: the global one, before the SFX bank, when the
        # stage loads the global scripts; else the stage's own, after it
        globals_ = open(os.path.join(data, 'Stages', folder, 'StageConfig.bin'), 'rb').read(1) == b'\x01'
        if globals_:
            add(s + 'Scripts.ps1')
        add(s + 'PS1Sfx.bin')
        if not globals_:
            add(s + 'Scripts.ps1')
        # Bytecode/<folder>.bin: not read with an image (phase 9.3); left to the rest of the disc
        add(s + '16x16Tiles.vram')
        add('Data/Sprites/Atlas/%s.atl' % folder)
        add('Data/Sprites/Atlas/%s.pic' % folder)
        for f in ('BGStrips.bin', 'CollisionMasks.ps1', 'Backgrounds.bin', '128x128Tiles.ps1'):
            add(s + f)
        for n in sorted(os.listdir(os.path.join(data, 'Stages', folder))):
            if n.startswith('Act') and n.endswith('.bin'):
                add(s + n)
        for p in local:
            if os.path.basename(p) not in early:
                add(p)
        for v in VARIANTS[1:]:
            add('Data/Sprites/Atlas/%s%s.atl' % (folder, v))
            add('Data/Sprites/Atlas/%s%s.pic' % (folder, v))
            add(s + 'BGStrips%s.bin' % v)
    with open(out_path, 'w') as f:
        f.write('# Disc files in the order the game loads them (tools/disc/gen_load_order.py, docs/30 phase 7.3 / 9).\n'
                '# tools/disc/build_disc.py lays these out first, in this order; then the other data files; CD-XA\n'
                '# streams last. Regenerated by tools/data_disc.sh.\n')
        f.write('\n'.join(order) + '\n')
    print('load order: %d files (%d stages) -> %s' % (len(order), len(folders), os.path.relpath(out_path)))


if __name__ == '__main__':
    main()
