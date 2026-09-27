#!/usr/bin/env python3
"""Which SFX each Sonic 2 stage can play (docs/30 phase 5.1): the PS1 loads only those.

The 44 global SFX alone are 1.13 MB at 44.1 kHz (phase 1), twice the SPU RAM, so the Sonic CD model (globals
resident, stage SFX after them) does not fit. Per stage folder:
  - object types = the types placed in its acts (Act*.bin) + every type its scripts create or assign with a constant
    (CreateTempObject, ResetObjectEntity, `Equal OBJECTTYPE... <const>`), transitively;
  - their subs (update, draw, startup) and every script function they call (CallFunction, transitively) are walked
    (both branches of every If / switch: conservative). A call through a variable (the players' state machine:
    `CallFunction OBJECTSTATE`; callbacks in values, GLOBAL[99] / [100], a local) may reach every constant any script
    of the stage stores into that variable (`Equal <var> <const>`; object variables of any entity); through a temp
    loaded from a table (Debug Mode), every entry of that table;
  - SFX ids = the constants of PlaySfx / SetSfxAttributes there (SetSfxAttributes starts the sound too); Sonic 2 has
    no variable SFX id (checked: an error if one appears).
Global SFX ids 0..G-1 (GameConfig, the engine loads them with the game config), a stage's own from G (StageConfig),
whether or not the stage loads the global objects. The pause menu's PlaySfxByName sounds (ps1/menu.cpp) are
RESIDENT (loaded once at boot); the menus' others (MENU_NAMES) load with the menu screens.

The scripts are read from the original bytecode (Sonic2_Extracted/Bytecode): the PS1 patches replace some script
loops with native opcodes, which play the same ids.

Usage: sfx_sets.py DATA_DIR BYTECODE_DIR   (prints the sets; build_sfx.py imports stage_sets())
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', 'atlas'))
sys.path.insert(0, os.path.join(HERE, '..', 'scripts'))
import bytecode_scan as bs  # noqa: E402
import object_sheets as osh  # noqa: E402
import patch_bytecode as pb  # noqa: E402

# ps1/menu.cpp PlaySfxByName names (GameConfig SFX names): the pause menu's are resident (every stage), the menus'
# other ones load with the menu screens (no stage runs then).
RESIDENT_NAMES = ['Menu Move', 'Menu Select', 'Resume']
MENU_NAMES = ['Menu Back', 'Event']
ACT_ATTR_SIZES = [(0x1, 4), (0x2, 1), (0x4, 4), (0x8, 4), (0x10, 1), (0x20, 1), (0x40, 1), (0x80, 1), (0x100, 4),
                  (0x200, 1), (0x400, 1), (0x800, 4), (0x1000, 4), (0x2000, 4), (0x4000, 4)]


def global_sfx(data):
    """(names, paths) of the global SFX (GameConfig.bin)."""
    r = osh._R(os.path.join(data, 'Game', 'GameConfig.bin'))
    r.s(), r.s()
    r.p += 0x60 * 3
    n = r.u8()
    [r.s() for _ in range(2 * n)]
    for _ in range(r.u8()):
        r.s()
        r.p += 4
    n = r.u8()
    names = [r.s() for _ in range(n)]
    return names, [r.s() for _ in range(n)]


def placed_types(stage_dir):
    """Object types placed in every Act*.bin of a stage (RSDKv4/Scene.cpp LoadActLayout)."""
    out = set()
    for f in sorted(os.listdir(stage_dir)):
        if not (f.startswith('Act') and f.endswith('.bin')):
            continue
        d = open(os.path.join(stage_dir, f), 'rb').read()
        p = 1 + d[0] + 4 + 1
        w, h = d[p], d[p + 2]
        p += 4 + w * h * 2
        n = struct.unpack_from('<H', d, p)[0]
        p += 2
        for _ in range(n):
            attribs = struct.unpack_from('<H', d, p)[0]
            out.add(d[p + 2])
            p += 2 + 2 + 8
            for bit, size in ACT_ATTR_SIZES:
                if attribs & bit:
                    p += size
    return out


def walk_all(code, start, names, vars_):
    """Every instruction from `start` to the sub's End (or the function's closing return), branches included:
    (position, name, operands) as patch_bytecode.instructions."""
    try:
        return pb.instructions(code, start, names, vars_)
    except (IndexError, KeyError, ValueError):
        return []


def var_key(op):
    """The variable an operand names, for matching stores and calls: object variables by name (any entity),
    GLOBAL / LOCAL by their constant index."""
    name, idx = op[1]
    if name.startswith('OBJECT') or idx is None or idx[1] == 1:
        return name
    return '%s[%d]' % (name, idx[2])


def stage_sets(data, bcdir):
    """{stage folder: sorted SFX ids}, the global SFX (names, paths), the resident ids, and notes."""
    names, vars_ = bs.tables(3)
    gnames, gpaths = global_sfx(data)
    gobjs = osh.game_config(data)[0]
    resident = sorted(gnames.index(n) for n in RESIDENT_NAMES)
    gload = bs.load(os.path.join(bcdir, 'GlobalCode.bin'))
    out, notes = {}, []
    for folder in sorted(os.listdir(os.path.join(data, 'Stages'))):
        cfg = os.path.join(data, 'Stages', folder, 'StageConfig.bin')
        path = os.path.join(bcdir, folder + '.bin')
        if not os.path.exists(cfg) or not os.path.exists(path):
            continue
        load_globals, _, sobjs = osh.stage_config(data, folder)
        code, _, ssubs, _, fns, _ = bs.full_code(path)
        # type t (1-based): the global objects first when the stage loads them
        subs = (list(gload[2]) if load_globals else []) + list(ssubs)
        onames = (list(gobjs) if load_globals else []) + list(sobjs)
        starts_all = sorted({x for t in subs for x in t if 0 <= x < len(code)} | {c for c, _ in fns if 0 <= c < len(code)})
        stored = {}  # variable -> constants stored into it anywhere in the stage's code
        for x in starts_all:
            for _, n, ops in walk_all(code, x, names, vars_):
                if n == 'Equal' and ops[0][0] == 'var' and ops[1][0] == 'int':
                    stored.setdefault(var_key(ops[0]), set()).add(ops[1][1])
        types = {t for t in placed_types(os.path.join(data, 'Stages', folder)) if 0 < t <= len(subs)}
        types.add(1)  # the player object (slot 0, set by the stage setup)
        sfx, seen_t, seen_f, todo = set(), set(), set(), list(types)
        while todo:
            t = todo.pop()
            if t in seen_t or not 0 < t <= len(subs):
                continue
            seen_t.add(t)
            starts = [x for x in subs[t - 1] if 0 <= x < len(code)]
            fq = []
            while starts or fq:
                if starts:
                    ins = walk_all(code, starts.pop(), names, vars_)
                else:
                    f = fq.pop()
                    if f in seen_f or not 0 <= f < len(fns) or not 0 <= fns[f][0] < len(code):
                        continue
                    seen_f.add(f)
                    ins = walk_all(code, fns[f][0], names, vars_)
                for k, (_, n, ops) in enumerate(ins):
                    if n in ('PlaySfx', 'SetSfxAttributes'):
                        if ops[0][0] != 'int':
                            sys.exit('ERROR sfx_sets: %s: %s with a variable id (%s)' % (folder, n, onames[t - 1]))
                        sfx.add(ops[0][1])
                    elif n == 'CallFunction':
                        if ops[0][0] == 'int':
                            fq.append(ops[0][1])
                        elif ops[0][1][0].startswith('TEMP'):
                            tabled = False
                            for _, n2, ops2 in reversed(ins[:k]):  # the temp's table load before the call
                                if n2 == 'GetTableValue' and ops2[0][0] == 'var' and var_key(ops2[0]) == var_key(ops[0]) \
                                        and ops2[2][0] == 'int':
                                    tab = ops2[2][1]
                                    fq.extend(code[tab + 1:tab + 1 + code[tab]])
                                    tabled = True
                                    break
                            if not tabled:
                                sys.exit('ERROR sfx_sets: %s: CallFunction %s not from a table (%s)' % (folder, ops[0][1][0],
                                                                                                     onames[t - 1]))
                        else:
                            fq.extend(stored.get(var_key(ops[0]), ()))
                    elif n == 'CreateTempObject' and ops[0][0] == 'int':
                        todo.append(ops[0][1])
                    elif n == 'ResetObjectEntity' and ops[1][0] == 'int':
                        todo.append(ops[1][1])
                    elif n == 'Equal' and ops[0][0] == 'var' and ops[0][1][0] == 'OBJECTTYPE' and ops[1][0] == 'int':
                        todo.append(ops[1][1])
        out[folder] = sorted(sfx)
    return out, (gnames, gpaths), resident, sorted(set(notes))


def main():
    data, bcdir = sys.argv[1], sys.argv[2]
    sets, (gnames, gpaths), resident, notes = stage_sets(data, bcdir)
    print('resident (menus, pause): %s' % ', '.join(gnames[i] for i in resident))
    for folder, ids in sets.items():
        g = [i for i in ids if i < len(gnames)]
        print('%-9s %2d global + %2d stage: %s' % (folder, len(g), len(ids) - len(g), ' '.join(str(i) for i in ids)))
    for n in notes:
        print('note:', n)


if __name__ == '__main__':
    main()
