#!/usr/bin/env python3
"""Stage-local copies of the shared files a stage load opens (docs/30 phase 9.2).

A stage load reads a few files that live elsewhere on the disc: Data/Game/GameConfig.bin (when its StageConfig loads the
global scripts: first byte 1; GlobalCode.bin was one too until the script images, phase 9.3), the players' animation files (Data/Animations/*.ani, opened by
the player objects' startups; which ones per player variant from the sprite manifests, build/manifest/p0..p3) and the
palette files (Data/Palettes: 5 files, 4.3 KB, loaded by startups such as the special stage's; all of them, as which stage
loads which isn't in the bytecode as plain text). Each such
read was a seek away from the stage's own files. Each stage gets a copy of them in Data/Stages/<Stage>/PS1Local/<name>
(exact bytes); ps1/cdrom_fs.cpp looks there first while that stage loads, and tools/disc/gen_load_order.py lays the copies
out between the stage's files in the order the load reads them. Basenames must be unique (checked).

Writes Data/Stages/<Stage>/PS1Local/ and prints one line per stage.
Usage: stage_local.py ISO_ROOT MANIFEST_DIR
"""
import os, re, shutil, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'atlas'))
import object_sheets  # noqa: E402

GLOBALS = ('Data/Game/GameConfig.bin',)  # GlobalCode.bin: in the stage's script image since phase 9.3
VARIANTS = ('p0', 'p1', 'p2', 'p3')  # Sonic, Tails, Knuckles, Sonic & Tails (atlas variants '', _t, _k, _st)


def shared_files(iso, manifests, folder):
    """The shared files `folder`'s load opens, in the order it reads them: globals first, then the animations."""
    out = []
    cfg = os.path.join(iso, 'Data', 'Stages', folder, 'StageConfig.bin')
    if open(cfg, 'rb').read(1) == b'\x01':
        out += list(GLOBALS)
    for v in VARIANTS:
        m = os.path.join(manifests, v, folder + '.txt')
        if not os.path.exists(m):
            continue
        for name in re.findall(r'\bani (\S+\.ani)', open(m).read()):
            p = 'Data/Animations/' + name
            if p not in out:
                out.append(p)
    pal = os.path.join(iso, 'Data', 'Palettes')
    if os.path.isdir(pal):
        out += ['Data/Palettes/' + n for n in sorted(os.listdir(pal)) if os.path.getsize(os.path.join(pal, n)) <= 4096]
    return [p for p in out if os.path.exists(os.path.join(iso, p))]


def main():
    iso, manifests = sys.argv[1], sys.argv[2]
    data = os.path.join(iso, 'Data')
    folders = []
    for lst in object_sheets.game_config(data)[3]:
        for folder, _, _ in lst:
            if folder not in folders and os.path.exists(os.path.join(data, 'Stages', folder, 'StageConfig.bin')):
                folders.append(folder)
    total = 0
    for folder in folders:
        dst = os.path.join(data, 'Stages', folder, 'PS1Local')
        shutil.rmtree(dst, ignore_errors=True)
        files = shared_files(iso, manifests, folder)
        if not files:
            continue
        names = [os.path.basename(p) for p in files]
        if len(set(n.lower() for n in names)) != len(names):
            sys.exit('ERROR stage_local: %s: two shared files share a name: %s' % (folder, names))
        os.makedirs(dst)
        size = 0
        for p in files:
            shutil.copyfile(os.path.join(iso, p), os.path.join(dst, os.path.basename(p)))
            size += os.path.getsize(os.path.join(iso, p))
        total += size
        print('stage local: %-9s %s (%d B)' % (folder, ' '.join(names), size))
    print('stage local: %d stages, %d B of copies' % (len(folders), total))


if __name__ == '__main__':
    main()
