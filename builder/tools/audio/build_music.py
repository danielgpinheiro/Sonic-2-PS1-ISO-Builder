#!/usr/bin/env python3
"""Build-time CD-XA music for the Sonic 2 PS1 port (docs/30 phase 5.2; the Sonic CD port's tool, RSDKv3-ps1):
Data/Music/*.ogg (44 tracks) -> Data/Music/Music.xa + Music.bin.

Format (as the RSDKv2 port): XA-ADPCM 37.8 kHz stereo 4-bit, 18.75 sectors/s per channel; at 2x CD speed
one channel takes 1 sector in 8, so the file interleaves 8 channels: physical sector r*8 + k carries
row r of channel k. The drive plays only the channel selected with Setfilter (file 1, channel k).

Many tracks per channel (as Sonic CD). Each channel holds its tracks back to back (longest first
into the least filled channel). Each track in its channel:
  - its audio rows. The PCM is resampled to 37,800 Hz by ffmpeg (swr, 64-tap filter) and **prefixed with silence** so
    the script's loop point lands on a row boundary; a row holds 2,016 stereo samples. The loop point
    comes from SetMusicTrack / SwapMusicTrack in the v4 bytecode (operand 2: <= 1 is the loop flag, from the start;
    more is the loop sample at 44.1 kHz) or from the native menus (NATIVE_LOOPS), else from the start. Pad
    = (-L') mod 2016 samples, L' = round(L * 37800 / 44100), at most 53 ms. A loop then restarts
    exactly there.
  - one Form-1 data row, the end marker: file 1, channel k, payload b'RSDKXAEND' + channel + u16 track
    id. Data sectors reach the CPU in XA mode (ps1/xa_music): it loops (Setloc to the loop row) or ends.
  - GUARD silent audio rows: they play while the loop Setloc / Pause takes effect, so the next track in
    the channel is never heard.
Rows past a channel's end are filler (silent XA audio on channel 31, never selected). psxavenc's EOF
submode bits are cleared.

Output: Data/Music/Music.xa (2336-byte sectors, mkpsxiso type="mixed") and Data/Music/Music.bin:
  'XAM2', u16 count, u16 interleave (8), u32 file sectors;
  count x { char[32] path relative to Data/Music (as in SetMusicTrack), u8 channel, u8 0, u16 track id,
            u32 start row, u32 loop row, u32 audio rows }                                   (48 B)
Self-check: every track's rows are read back from the written file (channel, submode, marker, guard) and
its first audio row is decoded (xa_adpcm) to confirm the leading pad is silent.

Usage: build_music.py SRC_DATA_DIR OUT_DATA_DIR BYTECODE_DIR   (e.g. ../Sonic2_Extracted/Data build/iso/Data
       ../Sonic2_Extracted/Bytecode; needs psxavenc, ffmpeg)
"""
import glob, os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import xa_adpcm  # noqa: E402
import bytecode_scan as bs  # noqa: E402

PSXAVENC = os.environ.get('PSXAVENC') or os.path.join(HERE, '../../../psxavenc/build/psxavenc')
FFMPEG = os.environ.get('FFMPEG') or 'ffmpeg'
SECTOR = 2336
INTERLEAVE = 8
FILE_NO = 1
FILLER_CHANNEL = 31
RATE = 37800
ROW_SAMPLES = 2016
GUARD = 4


def subheader(channel, submode, coding):
    return bytes([FILE_NO, channel, submode, coding] * 2)


def end_sector(channel, track_id):
    payload = (b'RSDKXAEND' + bytes([channel]) + struct.pack('<H', track_id)).ljust(2048, b'\0')
    return (subheader(channel, 0x08, 0) + payload).ljust(SECTOR, b'\0')  # Form 1 data; EDC/ECC by mkpsxiso


def silent_sector(channel, coding):
    return (subheader(channel, 0x64, coding)).ljust(SECTOR, b'\0')  # XA audio, all-zero sound groups


# The 2013 menus (RSDKv4/NativeObjects TitleScreen / MenuControl), which the PS1 menus replay (ps1/menu.cpp).
NATIVE_LOOPS = {'mainmenu.ogg': 106596, 'menuintro.ogg': 0}


def loop_points(bcdir):
    """{path under Data/Music (lower case): loop point (44.1 kHz samples, 0 = from the start)} from every
    SetMusicTrack / SwapMusicTrack with constant operands in the v4 bytecode, and the native menus'."""
    funcs, vars_ = bs.tables(3)
    names = [f[0] for f in funcs]
    ops_music = {names.index('SetMusicTrack'), names.index('SwapMusicTrack')}
    out = dict(NATIVE_LOOPS)
    for path in sorted(glob.glob(os.path.join(bcdir, '*.bin'))):
        code, _, subs, _, fns, _ = bs.load(path)
        starts = sorted({x for t in subs for x in t if 0 <= x < len(code)} | {c for c, _ in fns if 0 <= c < len(code)})
        for x in starts:
            try:
                ins = bs.walk(code, x, funcs, len(vars_))
            except (IndexError, ValueError):
                continue
            for _, n, ops in ins:
                if names.index(n) in ops_music and ops[0][0] == 'str' and ops[2][0] == 'int':
                    loop = ops[2][1] if ops[2][1] > 1 else 0
                    prev = out.setdefault(ops[0][1].lower(), loop)
                    if prev != loop:
                        sys.exit('ERROR: %s has two loop points (%d, %d)' % (ops[0][1], prev, loop))
    return out


def encode(src, loop44, tmp, channel):
    """-> (audio sectors, loop row) for one track."""
    d = open(src, 'rb').read()
    if d[:4] != b'OggS':
        d = bytes(b ^ 0xFF for b in d)
    ogg, wav, xa = os.path.join(tmp, 'in.ogg'), os.path.join(tmp, 'in.wav'), os.path.join(tmp, 'out.xa')
    open(ogg, 'wb').write(d)
    loop = round(loop44 * RATE / 44100) if loop44 > 1 else 0
    pad = (-loop) % ROW_SAMPLES
    subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', ogg, '-af', 'aresample=%d:resampler=swr:filter_size=64,adelay=%dS:all=1' % (RATE, pad),
                    '-ac', '2', '-ar', str(RATE), '-c:a', 'pcm_s16le', wav], check=True)
    subprocess.run([PSXAVENC, '-q', '-t', 'xa', '-f', str(RATE), '-c', '2', '-b', '4', '-F', str(FILE_NO), '-C', str(channel), wav, xa],
                   check=True)
    x = open(xa, 'rb').read()
    assert len(x) % SECTOR == 0, src
    secs = [bytearray(x[i:i + SECTOR]) for i in range(0, len(x), SECTOR)]
    for s in secs:
        assert s[0] == FILE_NO and s[1] == channel and s[2] & 0x04, (src, s[:8].hex())
        s[2] &= 0x7F  # clear EOF
        s[6] &= 0x7F
    return [bytes(s) for s in secs], (loop + pad) // ROW_SAMPLES, pad


def main():
    src_data, out_data, bcdir = sys.argv[1], sys.argv[2], sys.argv[3]
    root = os.path.join(src_data, 'Music')
    out_root = os.path.join(out_data, 'Music')
    os.makedirs(out_root, exist_ok=True)
    loops = loop_points(bcdir)
    tracks = sorted(os.path.relpath(p, root).replace(os.sep, '/') for p in glob.glob(os.path.join(root, '**', '*.ogg'), recursive=True))
    missing = [t for t in tracks if t.lower() not in loops]
    enc = {}
    with tempfile.TemporaryDirectory() as tmp:
        for i, rel in enumerate(tracks):
            enc[rel] = encode(os.path.join(root, rel), loops.get(rel.lower(), 0), tmp, 0)
    coding = enc[tracks[0]][0][0][3]
    # Channels: longest track first into the least filled channel.
    chans = [[] for _ in range(INTERLEAVE)]
    fill = [0] * INTERLEAVE
    for rel in sorted(tracks, key=lambda t: (-len(enc[t][0]), t)):
        k = fill.index(min(fill))
        chans[k].append(rel)
        fill[k] += len(enc[rel][0]) + 1 + GUARD
    rows = max(fill)
    table, placed = [], {}
    columns = []
    for k in range(INTERLEAVE):
        col = []
        for rel in chans[k]:
            tid = tracks.index(rel)
            secs, loop_row, pad = enc[rel]
            start = len(col)
            col += [s[:1] + bytes([k]) + s[2:5] + bytes([k]) + s[6:] for s in secs]
            col.append(end_sector(k, tid))
            col += [silent_sector(k, coding)] * GUARD
            placed[rel] = (k, tid, start, start + loop_row, len(secs), pad)
        col += [silent_sector(FILLER_CHANNEL, coding)] * (rows - len(col))
        columns.append(col)
    out = bytearray()
    for r in range(rows):
        for k in range(INTERLEAVE):
            out += columns[k][r]
    out = xa_adpcm.clear_unused(out)  # reproducible build
    open(os.path.join(out_root, 'Music.xa'), 'wb').write(out)
    table = struct.pack('<4sHHI', b'XAM2', len(tracks), INTERLEAVE, len(out) // SECTOR)
    for rel in tracks:
        k, tid, start, loop_row, n, _ = placed[rel]
        assert len(rel) < 32, rel
        table += struct.pack('<32sBBHIII', rel.encode(), k, 0, tid, start, loop_row, n)
    open(os.path.join(out_root, 'Music.bin'), 'wb').write(table)
    # Self-check from the written file.
    for rel in tracks:
        k, tid, start, loop_row, n, pad = placed[rel]
        sec = lambda r: out[(r * INTERLEAVE + k) * SECTOR:(r * INTERLEAVE + k + 1) * SECTOR]
        for r in (start, start + n - 1):
            assert sec(r)[1] == k and sec(r)[2] & 0x04 and not sec(r)[2] & 0x80, (rel, r)
        m = sec(start + n)
        assert m[1] == k and m[2] & 0x08 and m[8:17] == b'RSDKXAEND' and m[17] == k and struct.unpack_from('<H', m, 18)[0] == tid, rel
        for g in range(GUARD):
            assert sec(start + n + 1 + g)[1] == k and sec(start + n + 1 + g)[2] & 0x04, rel
        if pad >= 64:
            left, right = xa_adpcm.decode_sectors([sec(start)])
            assert abs(left[:pad - 32]).max() < 64 and abs(right[:pad - 32]).max() < 64, (rel, 'lead pad not silent')
    for k in range(INTERLEAVE):
        print('ch%d %2d tracks %6d rows %5.1f min' % (k, len(chans[k]), fill[k], fill[k] / 18.75 / 60))
    print('Music.xa: %d sectors (%.1f MB on disc), %d tracks, interleave %d, 37.8 kHz stereo 4-bit | loop points from scripts %d, none %d%s' % (
        len(out) // SECTOR, len(out) / SECTOR * 2352 / 1048576, len(tracks), INTERLEAVE, len(tracks) - len(missing), len(missing),
        (' (%s)' % ', '.join(missing)) if missing else ''))


if __name__ == '__main__':
    main()
