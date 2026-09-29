#!/usr/bin/env python3
"""Static scan of RSDKv4 (Sonic 1/2 2013) script bytecode (docs/30 phase 1), ported from the Sonic CD tool.

Bytecode files (Bytecode/<StageFolder>.bin, Bytecode/GlobalCode.bin), as RSDKv4/Script.cpp LoadBytecode
reads them: a block-encoded int array of script code (u32 count, then blocks: header h, n = h & 0x7F
values of 4 bytes if h >= 0x80 else 1 byte), the same for the jump table, u16 object count, per object
3 code pointers (update, draw, startup), then per object 3 jump-table pointers, u16 function count, the
functions' code pointers, then their jump-table pointers. In a stage that loads the global scripts the
pointers are absolute (its code follows GlobalCode's in scriptCode, and it lists the global functions too);
in one that doesn't (Credits...) they start at 0. 0x3FFFF = no sub.
Instructions, as ProcessScript reads them: opcode, then per operand a type (1 = variable: array type
[+ array flag + index] + variable id; 2 = int constant; 3 = string: length + len/4 + 1 ints).

The function and variable tables are parsed from RSDKv4/Script.cpp (FunctionInfo list, enum ScrVar), with
its `#if RETRO_REV0x` blocks evaluated for a revision (0..3; the opcode and variable numbering depends on
it). `--revision` checks which revision parses every sub of every file cleanly.

Usage: bytecode_scan.py BYTECODE_DIR [--rev N] [--find NAME ...] [--sizes] [--revision]
"""
import glob, os, re, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT_CPP = os.path.join(HERE, '..', 'RSDKv4', 'Script.cpp')
SUBS = ('update', 'draw', 'startup')


def preprocess(lines, rev):
    """Keep the lines whose `#if` conditions hold for revision `rev` (the macros Script.cpp's tables use)."""
    macros = {'RETRO_REV00': rev == 0, 'RETRO_REV01': rev >= 1, 'RETRO_REV02': rev >= 2, 'RETRO_REV03': rev >= 3,
              'RETRO_USE_HAPTICS': False, 'RETRO_USE_COMPILER': True, 'RETRO_USE_ORIGINAL_CODE': False,
              'RETRO_USE_MOD_LOADER': False,
              'PS1_GAME': 1}  # Sonic 1's opcodes (docs/37) come after Sonic 2's: listing them keeps Sonic 2's numbers

    def ev(expr):
        e = expr.strip()
        e = re.sub(r'!\s*(\w+)', lambda m: 'not %s' % m.group(1), e).replace('&&', ' and ').replace('||', ' or ')
        return bool(eval(re.sub(r'[A-Z_][A-Z0-9_]+', lambda m: str(macros.get(m.group(0), False)), e)))
    out, stack = [], []  # stack of (active, taken)
    for l in lines:
        s = l.strip()
        if s.startswith('#if'):
            c = ev(s[3:]) if s.startswith('#if ') else True
            parent = all(a for a, _ in stack)
            stack.append((parent and c, c))
        elif s.startswith('#else'):
            a, t = stack.pop()
            parent = all(x for x, _ in stack)
            stack.append((parent and not t, True))
        elif s.startswith('#endif'):
            stack.pop()
        elif all(a for a, _ in stack):
            out.append(l)
    return out


def tables(rev=3):
    src = open(SCRIPT_CPP).read().split('\n')
    start = next(i for i, l in enumerate(src) if l.startswith('const FunctionInfo functions[]'))
    end = next(i for i in range(start, len(src)) if src[i].startswith('};'))
    funcs = [(n, int(c)) for n, c in re.findall(r'FunctionInfo\("(\w+)",\s*(\d+)\)', '\n'.join(preprocess(src[start:end], rev)))]
    vs = next(i for i, l in enumerate(src) if l.startswith('enum ScrVar {'))
    ve = next(i for i in range(vs, len(src)) if src[i].startswith('};'))
    vars_ = [m.group(1) for l in preprocess(src[vs:ve], rev) for m in [re.match(r'\s*(VAR_\w+)', l)] if m and m.group(1) != 'VAR_MAX_CNT']
    return funcs, vars_


def blocks(d, p):
    n, = struct.unpack_from('<I', d, p)
    p += 4
    out = []
    while len(out) < n:
        h = d[p]; p += 1
        for _ in range(h & 0x7F):
            if h >= 0x80:
                out.append(struct.unpack_from('<i', d, p)[0]); p += 4
            else:
                out.append(d[p]); p += 1
    return out, p


def load(path):
    """(code, jump table, per-object code pointers, per-object jump pointers, function code/jump pointers)."""
    d = open(path, 'rb').read()
    code, p = blocks(d, 0)
    jt, p = blocks(d, p)
    n, = struct.unpack_from('<H', d, p); p += 2
    subs = [struct.unpack_from('<3i', d, p + 12 * i) for i in range(n)]; p += 12 * n
    jsubs = [struct.unpack_from('<3i', d, p + 12 * i) for i in range(n)]; p += 12 * n
    nf, = struct.unpack_from('<H', d, p); p += 2
    fcode = struct.unpack_from('<%di' % nf, d, p); p += 4 * nf
    fjump = struct.unpack_from('<%di' % nf, d, p); p += 4 * nf
    return code, jt, subs, jsubs, list(zip(fcode, fjump)), p == len(d)


OPEN = ('IfEqual', 'IfGreater', 'IfGreaterOrEqual', 'IfLower', 'IfLowerOrEqual', 'IfNotEqual', 'WEqual', 'WGreater',
        'WGreaterOrEqual', 'WLower', 'WLowerOrEqual', 'WNotEqual', 'switch')
CLOSE = ('endif', 'loop', 'endswitch')


def walk(code, start, funcs, nvars):
    """Instructions from `start` to its End / EndFunction, or a `return` outside any if / while / switch
    (v4 functions can end that way; table data may follow): [(pos, name, operands)]. Raises on a bad
    opcode / operand / variable."""
    out, p, depth = [], start, 0
    while True:
        at, op = p, code[p]
        p += 1
        if not 0 <= op < len(funcs):
            raise ValueError('bad opcode %d at %d' % (op, at))
        name, size = funcs[op]
        ops = []
        for _ in range(size):
            t = code[p]; p += 1
            if t == 1:
                arr = code[p]; p += 1
                if arr in (1, 2, 3):
                    p += 2
                if not 0 <= code[p] < nvars:
                    raise ValueError('bad variable %d at %d' % (code[p], p))
                ops.append(('var', code[p])); p += 1
            elif t == 2:
                ops.append(('int', code[p])); p += 1
            elif t == 3:
                n = code[p]
                ops.append(('str', ''.join(chr((code[p + 1 + c // 4] >> (24 - 8 * (c % 4))) & 0xFF) for c in range(n))))
                p += n // 4 + 2
            else:
                raise ValueError('bad operand type %d at %d' % (t, p - 1))
        out.append((at, name, ops))
        depth += (name in OPEN) - (name in CLOSE)
        if name in ('End', 'EndFunction') or (name == 'return' and depth <= 0):
            return out


def full_code(path):
    """The code a file's pointers index: a stage file's code follows GlobalCode's in scriptCode (its sub and
    function pointers are absolute; GlobalCode.bin itself starts at 0)."""
    code, jt, subs, jsubs, fns, exact = load(path)
    ptrs = [x for t in subs for x in t if x != 0x3FFFF] + [c for c, _ in fns]
    # Stages that load the global scripts (StageConfig) follow them; the others (Credits...) start at 0.
    if os.path.basename(path).lower() != 'globalcode.bin' and ptrs and max(ptrs) >= len(code):
        code = load(os.path.join(os.path.dirname(path), 'GlobalCode.bin'))[0] + code
    return code, jt, subs, jsubs, fns, exact


def sub_starts(path):
    code, jt, subs, jsubs, fns, _ = full_code(path)
    starts = [s for t in subs for s in t if 0 <= s < len(code)] + [c for c, _ in fns if 0 <= c < len(code)]
    return code, starts


def main():
    a = sys.argv[1:]
    bc = a[0]
    rev = int(a[a.index('--rev') + 1]) if '--rev' in a else 3
    files = sorted(glob.glob(os.path.join(bc, '*.bin')))
    if '--revision' in a:
        for r in range(4):
            funcs, vars_ = tables(r)
            ok = bad = 0
            first = None
            for f in files:
                code, starts = sub_starts(f)
                for s in starts:
                    try:
                        walk(code, s, funcs, len(vars_)); ok += 1
                    except (ValueError, IndexError) as e:
                        bad += 1; first = first or '%s: %s' % (os.path.basename(f), e)
            print('revision %d: %d functions, %d variables | subs parsed %d, failed %d%s' % (
                r, len(funcs), len(vars_), ok, bad, '' if not first else ' (first: %s)' % first))
        return
    funcs, vars_ = tables(rev)
    find = set(a[a.index('--find') + 1:]) if '--find' in a else set()
    find = {x for x in find if not x.startswith('--')}
    for f in files:
        code, jt, subs, jsubs, fns, exact = full_code(f)
        own = len(load(f)[0])
        uses = {}
        for s in [x for t in subs for x in t if 0 <= x < len(code)] + [c for c, _ in fns if 0 <= c < len(code)]:
            for _, n, ops in walk(code, s, funcs, len(vars_)):
                if n in find:
                    uses[n] = uses.get(n, 0) + 1
                for k, v in ops:
                    if k == 'var' and vars_[v] in find:
                        uses[vars_[v]] = uses.get(vars_[v], 0) + 1
        line = '%-16s code %6d ints, jump table %5d, objects %3d, functions %3d%s' % (
            os.path.basename(f), own, len(jt), len(subs), len(fns), '' if exact else ' (TRAILING BYTES)')
        if uses:
            line += ' | ' + ' '.join('%s=%d' % kv for kv in sorted(uses.items()))
        print(line)


if __name__ == '__main__':
    main()
