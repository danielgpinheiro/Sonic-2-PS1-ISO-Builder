#!/usr/bin/env python3
"""Extract a Retro Engine v4 datapack ("RSDKvB": Sonic 1/2 2013) into a loose Data/ + Bytecode/ tree (build time).

Format, from RSDKv4/Reader.cpp (CheckRSDKFile, LoadFile, GenerateELoadKeys, FileRead):
  "RSDKvB", u16 fileCount, fileCount x { MD5 of the lower-case path as 4 big-endian words (the digest with
  each 4-byte group reversed), u32 offset, u32 size (bit 31 = encrypted) }.
Names are only hashes: the paths come from a file list, tools/rsdkv4_filelist.txt (Sonic 2's 470 paths; any
entry whose hash no listed path produces is reported, so a list for another pack can be completed).
Encrypted files are XORed with a stream keyed by the file size:
  A = MD5(str(size)), B = MD5(str(size / 2 + 1)), each as 4 big-endian words, no = (size & 0x1FC) >> 2,
  posA = 0, posB = 8, swap = 0; per byte: b ^= B[posB] ^ no; if swap: b = nybble-swapped b; b ^= A[posA];
  then posA++, posB++ and: if posA <= 15: (posB > 12: posB = 0, swap ^= 1); elif posB <= 8: (posA = 0,
  swap ^= 1); else no = (no + 2) & 0x7F and both positions restart from no (the two branches of FileRead).
Every extracted file can be checked against a reference tree (--check DIR).

Usage: extract_rsdk.py DATA.rsdk OUT_DIR [--list FILELIST] [--check REF_DIR]
"""
import hashlib, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))


def be_words_md5(text):
    """MD5 with each 4-byte group reversed: the engine's GenerateMD5FromString words written big-endian
    (how the pack stores name hashes and how GenerateELoadKeys lays out the keys)."""
    d = hashlib.md5(text.encode('latin1')).digest()
    return b''.join(d[i:i + 4][::-1] for i in range(0, 16, 4))


def name_hash(path):
    """The pack's 16 hash bytes for a path (MD5 of the lower-case path)."""
    return be_words_md5(path.lower())


def entries(d):
    assert d[:6] == b'RSDKvB', 'not an RSDKvB datapack: %r' % d[:6]
    n, = struct.unpack_from('<H', d, 6)
    out, p = [], 8
    for _ in range(n):
        h = d[p:p + 16]
        off, size = struct.unpack_from('<II', d, p + 16)
        out.append((h, off, size & 0x7FFFFFFF, bool(size & 0x80000000)))
        p += 24
    return out


def mul_high(a, b):
    return ((a & 0xFFFFFFFF) * (b & 0xFFFFFFFF)) >> 32


def keystream(size):
    """(xorB, swap, xorA) per byte for a file of `size` bytes (FileRead's state machine)."""
    ka, kb = be_words_md5(str(size)), be_words_md5(str((size >> 1) + 1))
    no = (size & 0x1FC) >> 2
    pa, pb, swap = 0, 8, 0
    xb = np.empty(size, np.uint8); sw = np.empty(size, np.bool_); xa = np.empty(size, np.uint8)
    for i in range(size):
        xb[i] = kb[pb] ^ no
        sw[i] = swap
        xa[i] = ka[pa]
        pa += 1
        pb += 1
        if pa <= 0x0F:
            if pb > 0x0C:
                pb = 0
                swap ^= 1
        elif pb <= 0x08:
            pa = 0
            swap ^= 1
        else:
            no = (no + 2) & 0x7F
            k1, k2 = mul_high(0xAAAAAAAB, no), mul_high(0x24924925, no)
            t1, t2 = k2 + (no - k2) // 2, k1 // 8 * 3
            if swap:
                swap = 0
                pa, pb = no - t1 // 4 * 7, no - t2 * 4 + 2
            else:
                swap = 1
                pb, pa = no - t1 // 4 * 7, no - t2 * 4 + 3
    return xb, sw, xa


def decrypt(data):
    xb, sw, xa = keystream(len(data))
    b = np.frombuffer(data, np.uint8) ^ xb
    b = np.where(sw, ((b << 4) | (b >> 4)) & 0xFF, b).astype(np.uint8)
    return (b ^ xa).tobytes()


def main():
    a = sys.argv[1:]
    src, out = a[0], a[1]
    listf = a[a.index('--list') + 1] if '--list' in a else os.path.join(HERE, 'rsdkv4_filelist.txt')
    ref = a[a.index('--check') + 1] if '--check' in a else None
    d = open(src, 'rb').read()
    ents = entries(d)
    known = {}
    for line in open(listf):
        line = line.strip()
        if line and not line.startswith('#'):
            known[name_hash(line)] = line
    done, missing, bad = 0, [], []
    for h, off, size, enc in ents:
        path = known.get(h)
        if path is None:
            missing.append(h.hex())
            continue
        data = d[off:off + size]
        if enc:
            data = decrypt(data)
        dst = os.path.join(out, path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        open(dst, 'wb').write(data)
        done += 1
        if ref:
            r = os.path.join(ref, path)
            if not os.path.exists(r) or open(r, 'rb').read() != data:
                bad.append(path)
    print('%d files in the pack: %d extracted, %d names unknown%s' % (
        len(ents), done, len(missing), '' if not ref else ', %d differ from %s' % (len(bad), ref)))
    for m in missing[:20]:
        print('  unknown', m)
    for b in bad[:20]:
        print('  DIFFERS', b)
    return 1 if missing or bad else 0


if __name__ == '__main__':
    sys.exit(main())
