#!/usr/bin/env python3
"""Sprite manifests from the bytecode alone (docs/30 phase 1.3 feasibility), in the format
build_atlas.py reads (`sheet <id> <path>`, `frame <id> x y w h ani <name>`), one per stage folder and
player variant, until the engine can list its frames itself (as RSDKv3-ps1's tools/rsdkmanifest does).

Per folder: every object's constant SpriteFrame rects on the sheet loaded before them
(tools/atlas/object_sheets.py), the frames of every .ani an object loads, and the player variant's
animations. The player objects load every character's .ani; the variant decides which are live:
  sonic = Sonic.ani + SuperSonic.ani, tails = Tails.ani (+ the Tails Object's tail frames),
  knux = Knuckles.ani, sonictails = sonic + tails.
Missed: frames set at runtime (SpriteFrame / EditFrame with variable operands: counted per stage).

Usage: static_manifest.py DATA_DIR OUT_DIR   (writes OUT_DIR/<variant>/<folder>.txt)
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import object_sheets as osh  # noqa: E402

PLAYER_ANIS = {'sonic.ani', 'supersonic.ani', 'tails.ani', 'knuckles.ani'}
VARIANTS = {
    'sonic': ['Sonic.ani', 'SuperSonic.ani'],
    'tails': ['Tails.ani'],
    'knux': ['Knuckles.ani'],
    'sonictails': ['Sonic.ani', 'SuperSonic.ani', 'Tails.ani'],
}
TAILS_ONLY_OBJECTS = {'tails object'}  # the tail sprite that follows Tails


def folders(data):
    out = []
    for lst in osh.game_config(data)[3]:
        for f, _, _ in lst:
            if f not in out:
                out.append(f)
    return out


def manifest(data, folder, variant):
    """(manifest text, runtime frame count, [objects with runtime frames])."""
    objs = osh.object_sheets(data, folder)
    frames = {}
    runtime, rt_objs = 0, []
    anis = set()
    for name, info in objs.items():
        if name.lower() in TAILS_ONLY_OBJECTS and 'Tails.ani' not in VARIANTS[variant]:
            continue
        for sh, rects in info['frames'].items():
            frames.setdefault(sh, set()).update(rects)
        anis |= {a for a in info['anis'] if a.lower() not in PLAYER_ANIS}
        if info['runtime']:
            runtime += info['runtime']
            rt_objs.append(name)
    for a in sorted(anis) + VARIANTS[variant]:
        for sh, rects in osh.ani_frames(data, a).items():
            frames.setdefault(sh, set()).update(rects)
    lines = []
    for i, sh in enumerate(sorted(frames)):
        lines.append('sheet %d %s' % (i, sh))
        for r in sorted(frames[sh]):
            lines.append('frame %d %d %d %d %d ani static' % ((i,) + r))
    return '\n'.join(lines) + '\n', runtime, rt_objs


def main():
    data, out = sys.argv[1], sys.argv[2]
    for v in VARIANTS:
        os.makedirs(os.path.join(out, v), exist_ok=True)
    for f in folders(data):
        if not os.path.exists(os.path.join(data, 'Stages', f, 'StageConfig.bin')):
            continue
        for v in VARIANTS:
            text, rt, rt_objs = manifest(data, f, v)
            open(os.path.join(out, v, f + '.txt'), 'w').write(text)
        print('%-12s runtime frames %3d %s' % (f, rt, ', '.join(rt_objs[:6]) + (' ...' if len(rt_objs) > 6 else '')))


if __name__ == '__main__':
    main()
