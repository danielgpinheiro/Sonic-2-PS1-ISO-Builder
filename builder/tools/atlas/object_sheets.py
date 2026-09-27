#!/usr/bin/env python3
"""What each object of an RSDKv4 stage draws with (docs/30 phase 1.3), from the bytecode alone.

Ported from the Sonic CD tool. v4 differences: the bytecode is Bytecode/<folder>.bin (+ GlobalCode.bin
when the StageConfig loads the global objects), 3 subs per object, and most sprite frames are script
SpriteFrame(pivotX, pivotY, w, h, sprX, sprY) calls in the setup sub (only five .ani files ship, the
players' and one boss's). A frame belongs to its object's frame list and is drawn from the sheet the
object last loaded (scriptInfo->spriteSheetID), so each constant frame is attributed to the sheet loaded
last before it on the walk. Subs are walked linearly: branches on CheckCurrentStageFolder are resolved for
the folder, every other branch is taken both ways; CallFunction constants are followed into script
functions.

object_sheets(data, folder) -> {object name: dict(sheets, frames, anis, runtime, flags)}
  sheets : set of LoadSpriteSheet paths (under Data/Sprites/)
  frames : {sheet: set of constant (x, y, w, h) SpriteFrame rects} (the sheet loaded last before the frame)
  anis   : set of LoadAnimation paths (under Data/Animations/)
  runtime: count of SpriteFrame / EditFrame calls with variable operands (frames only known at runtime)
  flags  : '3d' (Draw3DScene), 'text' (DrawText), ('native', name) ...
"""
import os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
import bytecode_scan as bs  # noqa: E402

_TABLES = None


def _tables():
    global _TABLES
    if _TABLES is None:
        _TABLES = bs.tables(3)
    return _TABLES


class _R:
    def __init__(self, path):
        self.d, self.p = open(path, 'rb').read(), 0

    def u8(self):
        self.p += 1
        return self.d[self.p - 1]

    def s(self):
        n = self.u8()
        self.p += n
        return self.d[self.p - n:self.p].decode('latin1')


def game_config(data):
    """(global object names, global SFX paths, player names, stage lists [(folder, id, name)] x 4 in file order:
    presentation, regular, special, bonus)."""
    r = _R(os.path.join(data, 'Game', 'GameConfig.bin'))
    r.s(), r.s()
    r.p += 0x60 * 3
    n = r.u8()
    names = [r.s() for _ in range(n)]
    [r.s() for _ in range(n)]
    for _ in range(r.u8()):
        r.s(); r.p += 4
    n = r.u8()
    [r.s() for _ in range(n)]
    sfx = [r.s() for _ in range(n)]
    players = [r.s() for _ in range(r.u8())]
    lists = []
    for _ in range(4):
        lst = []
        for _ in range(r.u8()):
            lst.append((r.s(), r.s(), r.s())); r.u8()
        lists.append(lst)
    return names, sfx, players, lists


def stage_config(data, folder):
    """(loads the global objects, stage SFX paths, stage object names)."""
    r = _R(os.path.join(data, 'Stages', folder, 'StageConfig.bin'))
    globals_ = r.u8()
    r.p += 0x20 * 3
    n = r.u8()
    [r.s() for _ in range(n)]
    sfx = [r.s() for _ in range(n)]
    n = r.u8()
    return bool(globals_), sfx, [r.s() for _ in range(n)]


def _scan(code, start, fptrs, info, seen, folder):
    """Walk one sub / function. Branches on CheckCurrentStageFolder("X") (then `if checkResult == 1` / `!= 1`...)
    are resolved for `folder`; every other branch is taken both ways. A frame is attributed to the sheet
    loaded last on the walk (info['frames'] maps sheet -> rects; None = no sheet loaded yet in this sub)."""
    funcs, vars_ = _tables()
    try:
        ins = bs.walk(code, start, funcs, len(vars_))
    except (ValueError, IndexError):
        info['flags'].add('parse-error')
        return
    check = None     # result of the last CheckCurrentStageFolder, while no other instruction intervenes
    stack = []       # per open block: None (not resolved) or the truth of a resolved folder `if`
    for _, name, ops in ins:
        vals = [v if k in ('int', 'str') else None for k, v in ops]
        live = all(t is not False for t in stack)
        if name in bs.CLOSE:
            if stack:
                stack.pop()
            continue
        if name == 'else':
            if stack and stack[-1] is not None:
                stack[-1] = not stack[-1]
            continue
        if name in bs.OPEN:
            truth = None
            if check is not None and name in ('IfEqual', 'IfNotEqual') and live:
                names = [vars_[v] if k == 'var' else None for k, v in ops[1:]]
                consts = [v for k, v in ops[1:] if k == 'int']
                if 'VAR_CHECKRESULT' in names and len(consts) == 1:
                    truth = (check == bool(consts[0])) == (name == 'IfEqual')
            stack.append(truth)
            check = None
            continue
        check = None
        if not live:
            continue
        if name == 'CheckCurrentStageFolder' and isinstance(vals[0], str):
            check = vals[0].lower() == folder.lower()
        elif name == 'LoadSpriteSheet' and isinstance(vals[0], str):
            info['sheets'].add(vals[0])
            info['cur'] = vals[0]
        elif name == 'SpriteFrame':
            if all(isinstance(v, int) for v in vals):
                px, py, w, h, x, y = vals
                info['frames'].setdefault(info['cur'], set()).add((x, y, w, h))
            else:
                info['runtime'] += 1
        elif name == 'EditFrame':
            info['runtime'] += 1
        elif name == 'LoadAnimation' and isinstance(vals[0], str):
            info['anis'].add(vals[0])
        elif name == 'Draw3DScene':
            info['flags'].add('3d')
        elif name == 'DrawText':
            info['flags'].add('text')
        elif name == 'CallFunction' and isinstance(vals[0], int) and 0 <= vals[0] < len(fptrs) and vals[0] not in seen:
            seen.add(vals[0])
            _scan(code, fptrs[vals[0]], fptrs, info, seen, folder)


def object_sheets(data, folder):
    bcdir = os.path.join(os.path.dirname(os.path.normpath(data)), 'Bytecode')
    path = os.path.join(bcdir, folder + '.bin')
    if not os.path.exists(path):
        return {}
    load_globals, _, stage_names = stage_config(data, folder)
    code, _, subs, _, fns, _ = bs.full_code(path)
    fptrs = [c for c, _ in fns]
    objs = []
    if load_globals:
        gpath = os.path.join(bcdir, 'GlobalCode.bin')
        _, _, gsubs, _, _, _ = bs.load(gpath)
        objs += list(zip(game_config(data)[0], gsubs))
    objs += list(zip(stage_names, subs))
    out = {}
    for name, sub in objs:
        info = dict(sheets=set(), frames={}, anis=set(), runtime=0, flags=set(), cur=None)
        for off in sub:
            if off != 0x3FFFF and 0 <= off < len(code):
                info['cur'] = None
                _scan(code, off, fptrs, info, set(), folder)
        # Frames defined before any LoadSpriteSheet of their sub are drawn with whatever sheet the object holds:
        # any of its sheets.
        loose = info['frames'].pop(None, set())
        for sh in info['sheets']:
            info['frames'].setdefault(sh, set()).update(loose)
        if loose and not info['sheets']:
            info['flags'].add('frames-without-sheet')
        del info['cur']
        out[name] = info
    return out


def ani_frames(data, path):
    """{sheet path: set of (x, y, w, h)} of an .ani file (RSDKv4/Animation.cpp LoadAnimationFile)."""
    r = _R(os.path.join(data, 'Animations', path))
    sheets = [r.s() for _ in range(r.u8())]
    out = {}
    for _ in range(r.u8()):
        r.s()
        n = r.u8()
        r.p += 3
        for _ in range(n):
            sid = r.u8(); r.u8()
            x, y, w, h = r.u8(), r.u8(), r.u8(), r.u8()
            r.p += 2
            out.setdefault(sheets[sid], set()).add((x, y, w, h))
    return out


if __name__ == '__main__':
    d, folder = sys.argv[1], sys.argv[2]
    for k, v in object_sheets(d, folder).items():
        if v['sheets'] or v['frames'] or v['anis'] or v['runtime'] or v['flags']:
            print('%-24s sheets %s frames %d anis %s runtime %d %s' % (
                k, sorted(v['sheets']), sum(map(len, v['frames'].values())), sorted(v['anis']), v['runtime'], sorted(map(str, v['flags']))))
