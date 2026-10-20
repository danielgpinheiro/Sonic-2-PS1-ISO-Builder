#!/usr/bin/env python3
"""Build-time edits of stage layouts (Data/Stages/<X>/Act*.bin) for the PS1's 320-pixel screen.

The act file's entity records (RSDKv4/Scene.cpp LoadActLayout): u16 attribute flags, u8 type, u8 property value, s32 x,
s32 y, then one field per set flag (0x1 state 4, 0x2 direction 1, 0x4 scale 4, 0x8 rotation 4, 0x10 drawOrder 1,
0x20 priority 1, 0x40 alpha 1, 0x80 animation 1, 0x100 animationSpeed 4, 0x200 frame 1, 0x400 inkEffect 1,
0x800..0x4000 values[0..3] 4 each). A layout can give an entity its own priority (flag 0x20); the engine reads it as
upstream does.

Sonic 2, Death Egg Zone (Zone12, V4-105): after Mecha Sonic the One Way Door at x 1016 and Eggman at x 1088 (who holds
that door shut, `object[-1].state = 0`, until the player reaches him) are processed only within the screen's object
range (xScrollOffset + SCREEN_XSIZE + 128). A spindash locks the camera for 15 frames (GlobalCode function 18): on the
PS1's 320-pixel screen the arena leaves the camera at 544-630, the range ends before Eggman and he does not run while a
full-charge spindash rolls past the door, which then opens for the player's speed with nobody holding it (on a 424-pixel
PC screen both are in range). Eggman gets PRIORITY_ACTIVE (always processed), so the door stays shut until his scene
opens the way, as in a slow approach. The door keeps its default: it is in range whenever the player can reach it, and
its update (three player loops of box tests) cost 2.6 CPU hblanks a frame on DuckStation when always processed.

Each edit is checked against the record it expects (type by name in the stage's object list, position, flags); a
layout already edited is left as it is. Usage: patch_layouts.py DATA_DIR [--game 1|2]
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'atlas'))
import object_sheets  # noqa: E402

FIELD = ((0x1, 4), (0x2, 1), (0x4, 4), (0x8, 4), (0x10, 1), (0x20, 1), (0x40, 1), (0x80, 1), (0x100, 4), (0x200, 1),
         (0x400, 1), (0x800, 4), (0x1000, 4), (0x2000, 4), (0x4000, 4))
PRIORITY_FLAG, PRIORITY_ACTIVE = 0x20, 1  # RSDKv4/Object.hpp ObjectPriority

# (folder, act file, object name, x, y in pixels, attribute flags expected before the edit, priority)
EDITS_S2 = (
    ('Zone12', 'Act1.bin', 'Eggman', 1088, None, 0, PRIORITY_ACTIVE),
)


def entities(raw):
    """(offset of the entity count, [(record offset, attribs, type, x, y, field offsets {flag: offset})])."""
    p = 1 + raw[0] + 4 + 1  # title length + text, activeTileLayers, tLayerMidPoint
    xs, ys = raw[p], raw[p + 2]
    p += 4 + xs * ys * 2
    count = struct.unpack_from('<H', raw, p)[0]
    cpos, p = p, p + 2
    out = []
    for _ in range(count):
        start = p
        attribs, typ, _prop, x, y = struct.unpack_from('<HBBii', raw, p)
        p += 12
        fields = {}
        for flag, size in FIELD:
            if attribs & flag:
                fields[flag] = p
                p += size
        out.append((start, attribs, typ, x >> 16, y >> 16, fields))
    assert p == len(raw), 'layout parse ended at %d of %d bytes' % (p, len(raw))
    return cpos, out


def type_ids(data, folder):
    """Object name -> entity type (1-based: the global objects first when the stage loads them)."""
    load_globals, globals_, objs = object_sheets.stage_config(data, folder)
    names = (list(object_sheets.game_config(data)[0]) if load_globals else []) + list(objs)
    return {n: i + 1 for i, n in enumerate(names)}


def patch(data, edits):
    done, already = [], []
    for folder, act, name, x, y, before, prio in edits:
        path = os.path.join(data, 'Stages', folder, act)
        raw = bytearray(open(path, 'rb').read())
        typ = type_ids(data, folder)[name]
        _, ents = entities(raw)
        hits = [e for e in ents if e[2] == typ and e[3] == x and (y is None or e[4] == y)]
        if len(hits) != 1:
            sys.exit('ERROR patch_layouts: %d %s entities at x %d in %s/%s' % (len(hits), name, x, folder, act))
        start, attribs, _, _, _, fields = hits[0]
        if attribs & PRIORITY_FLAG:
            if raw[fields[PRIORITY_FLAG]] != prio:
                sys.exit('ERROR patch_layouts: %s at x %d already has priority %d' % (name, x, raw[fields[PRIORITY_FLAG]]))
            already.append('%s %s x %d' % (folder, name, x))
            continue
        if attribs != before:
            sys.exit('ERROR patch_layouts: %s at x %d has flags %#x, expected %#x' % (name, x, attribs, before))
        # the priority byte goes after the fields of the lower flags
        at = start + 12 + sum(size for flag, size in FIELD if flag < PRIORITY_FLAG and attribs & flag)
        raw[at:at] = bytes([prio])
        struct.pack_into('<H', raw, start, attribs | PRIORITY_FLAG)
        entities(raw)  # still parses to the end
        open(path, 'wb').write(raw)
        done.append('%s %s x %d -> priority %d' % (folder, name, x, prio))
    return done, already


def main():
    args = sys.argv[1:]
    game = '2'
    if '--game' in args:
        i = args.index('--game'); game = args[i + 1]; del args[i:i + 2]
    edits = EDITS_S2 if game == '2' else ()
    done, already = patch(args[0], edits)
    print('layouts: %s%s' % (', '.join(done) or 'nothing to edit', ' (already: %s)' % ', '.join(already) if already else ''))


if __name__ == '__main__':
    main()
