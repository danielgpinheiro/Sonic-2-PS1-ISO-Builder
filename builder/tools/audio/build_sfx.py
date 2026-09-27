#!/usr/bin/env python3
"""Build-time SFX for the Sonic 2 PS1 port (docs/30 phase 5.1): SPU-ADPCM banks, one per stage.

Sonic 2's 44 global SFX alone need 1.13 MB at 44.1 kHz, over twice the SPU sample area, so the Sonic CD model (globals
resident) does not fit. Instead (tools/audio/sfx_sets.py):
  Data/Game/PS1SfxResident.bin   the SFX the PS1 pause menu plays by name (resident, uploaded at boot);
  Data/Game/PS1SfxMenu.bin       the menus' other ones, uploaded into the stage area when the menu screens open;
  Data/Stages/<Stage>/PS1Sfx.bin every other SFX the stage's scripts can play (global or its own), uploaded at the
                                 stage load after the resident ones in one read.
Rates: one per sample per bank (the Sonic CD solver, tools/audio in RSDKv3-ps1): every sample starts at its source rate
(<= 44.1 kHz) and, while resident + the stage's bank is over the SPU sample area, the largest sample steps down one
rate (RATES), none below FLOOR while another is above it; short, bright sounds keep theirs. Each stage is solved on
its own (its bank holds its own copies).
Stage banks are capped at STAGE_CAP = 400 KB (docs/30, user decision 2026-09-27: shorter loads; the zones' ~481 KB banks
were ~1.6 s of each zone load at 2x; 300 KB was measured and rejected: most zones ~11 kHz, some 8-9 kHz): the solver's budget is min(SPU area left, STAGE_CAP), the same largest-first steps. The log gives each
bank's size without / with the cap and every sound whose rate the cap lowered (the listening check); the full list
per stage goes to OUT_DATA_DIR/../../sfx_rates.txt (build/, not on the disc).

Sources are the extracted data's Data/SoundFX (Global/*.ogg, Stage/*.wav): ffmpeg decodes them to mono 16-bit PCM,
psxavenc encodes `-t vag` (48-byte header, rate at 0x10, data padded to 64 bytes with a trailing loop-trap block).

Bank (little endian): 'PSFX', u16 count, u16 pad; count x (u16 sfx id, u16 pad, u32 offset, u32 size, u32 rate)
(16 B each; ids sharing a file share its data); the sample data from offset 0 after the table, each a multiple of 64 bytes (SPU DMA). Self-check: each
bank is read back and its samples compared with their .vag data; sizes re-measured against the budget.

Usage: build_sfx.py SRC_DATA_DIR OUT_DATA_DIR BYTECODE_DIR   (e.g. ../Sonic2_Extracted/Data build/iso/Data
       ../Sonic2_Extracted/Bytecode; needs ffmpeg and psxavenc; PS1_SFX_STAGE_CAP=bytes overrides the cap)
"""
import os, struct, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sfx_sets  # noqa: E402

PSXAVENC = os.environ.get('PSXAVENC') or os.path.join(HERE, '../../../psxavenc/build/psxavenc')
FFMPEG = os.environ.get('FFMPEG') or 'ffmpeg'
SPU_SAMPLE_START = 0x1000
SPU_SAMPLE_END = 0x80000 - 32
RATES = (44100, 37800, 32000, 27000, 22050, 18900, 16000, 13000, 11025, 9450, 8000)
FLOOR = 11025  # below it only once every sample is there (reported)
STAGE_CAP = int(os.environ.get("PS1_SFX_STAGE_CAP") or 400 * 1024)  # stage bank bytes (see the module doc)


def vag_size(frames, src_rate, rate):
    samples = -(-frames * rate // src_rate)
    blocks = -(-samples // 28) + 2  # + leading dummy block + trailing loop-trap block
    return -(-blocks * 16 // 64) * 64


def choose_rates(names, info, budget):
    """{name: rate index}: see the module doc. The largest sample steps down first, but no sample goes below FLOOR
    while another one is still above it (long sounds -- the 1-up jingle, Continue -- would otherwise end at 8 kHz
    while short ones keep 44.1 kHz)."""
    rate = {n: next(k for k, r in enumerate(RATES) if r <= info[n][1]) for n in names}

    def size(n):
        return vag_size(info[n][0], info[n][1], RATES[rate[n]])

    while sum(size(n) for n in names) > budget:
        cand = [n for n in names if RATES[rate[n]] > FLOOR] or [n for n in names if rate[n] + 1 < len(RATES)]
        if not cand:
            return None
        n = max(cand, key=lambda n: (size(n), n))
        rate[n] += 1
    return rate


class Encoder:
    def __init__(self, src_sfx, tmp):
        self.src, self.tmp, self.pcm, self.vag = src_sfx, tmp, {}, {}

    def info(self, path):
        """(frames, rate) of the mono PCM decode."""
        if path not in self.pcm:
            out = os.path.join(self.tmp, 'pcm%d.wav' % len(self.pcm))
            subprocess.run([FFMPEG, '-v', 'error', '-y', '-i', os.path.join(self.src, path), '-ac', '1', '-c:a', 'pcm_s16le',
                            out], check=True)
            d = open(out, 'rb').read()
            i, rate, frames = 12, 44100, 0
            while i + 8 <= len(d):
                cid, cs = d[i:i + 4], struct.unpack_from('<I', d, i + 4)[0]
                if cid == b'fmt ':
                    rate = struct.unpack_from('<I', d, i + 12)[0]
                elif cid == b'data':
                    frames = cs // 2
                i += 8 + cs + (cs & 1)
            self.pcm[path] = (out, frames, rate)
        return self.pcm[path][1:]

    def encode(self, path, rate):
        """SPU-ADPCM data (the .vag body, padded to 64 bytes) at `rate`."""
        key = (path, rate)
        if key not in self.vag:
            out = os.path.join(self.tmp, 'v%d.vag' % len(self.vag))
            subprocess.run([PSXAVENC, '-q', '-t', 'vag', '-f', str(rate), self.pcm[path][0], out], check=True)
            v = open(out, 'rb').read()
            assert v[:4] == b'VAGp', path
            size, vrate = struct.unpack_from('>I', v, 12)[0], struct.unpack_from('>I', v, 16)[0]
            body = v[48:]
            assert vrate == rate and len(body) >= size and len(body) % 64 == 0, (path, rate)
            self.vag[key] = body
        return self.vag[key]


def write_bank(path, entries):
    """entries: [(sfx id, rate, data)] -> the bank file; read back and checked."""
    table, blob, at = bytearray(), bytearray(), {}
    for sid, rate, data in entries:  # ids sharing a file (Ring L / Ring R) share its data
        if data not in at:
            at[data] = len(blob)
            blob += data
        table += struct.pack('<HHIII', sid, 0, at[data], len(data), rate)
    out = b'PSFX' + struct.pack('<HH', len(entries), 0) + table + blob
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, 'wb').write(out)
    back = open(path, 'rb').read()
    n = struct.unpack_from('<H', back, 4)[0]
    base = 8 + 16 * n
    for k, (sid, rate, data) in enumerate(entries):
        s2, _, off, size, r2 = struct.unpack_from('<HHIII', back, 8 + 16 * k)
        assert (s2, r2) == (sid, rate) and back[base + off:base + off + size] == data and size % 64 == 0, path
    return len(blob)


def main():
    src_data, out_data, bcdir = sys.argv[1], sys.argv[2], sys.argv[3]
    sets, (gnames, gpaths), resident, _ = sfx_sets.stage_sets(src_data, bcdir)
    src_sfx = os.path.join(src_data, 'SoundFX')
    budget = SPU_SAMPLE_END - SPU_SAMPLE_START
    report, low, drops, detail = [], [], {}, []
    with tempfile.TemporaryDirectory() as tmp:
        enc = Encoder(src_sfx, tmp)
        # resident: source rates
        rpaths = {i: gpaths[i] for i in resident}
        info = {p: enc.info(p) for p in rpaths.values()}
        rentries = [(i, min(info[p][1], 44100), enc.encode(p, min(info[p][1], 44100))) for i, p in sorted(rpaths.items())]
        rsize = write_bank(os.path.join(out_data, 'Game', 'PS1SfxResident.bin'), rentries)
        stage_budget = budget - rsize
        # the menus' other sounds: a bank loaded into the stage area when the menu screens open (source rates)
        mids = sorted(gnames.index(n) for n in sfx_sets.MENU_NAMES)
        for i in mids:
            info[gpaths[i]] = enc.info(gpaths[i])
        mentries = [(i, min(info[gpaths[i]][1], 44100), enc.encode(gpaths[i], min(info[gpaths[i]][1], 44100))) for i in mids]
        msize = write_bank(os.path.join(out_data, 'Game', 'PS1SfxMenu.bin'), mentries)
        assert msize <= stage_budget
        for folder, ids in sets.items():
            spaths = sfx_sets.osh.stage_config(src_data, folder)[1]
            paths = {}
            for i in ids:
                if i in resident:
                    continue
                p = gpaths[i] if i < len(gpaths) else (spaths[i - len(gpaths)] if i - len(gpaths) < len(spaths) else None)
                if p is None:
                    sys.exit('ERROR build_sfx: %s: sfx id %d has no file' % (folder, i))
                paths[i] = p
            names = sorted(set(paths.values()))
            for p in names:
                info[p] = enc.info(p)
            free = choose_rates(names, info, stage_budget)  # without the cap: for the log
            rate = choose_rates(names, info, min(stage_budget, STAGE_CAP))
            if rate is None or free is None:
                sys.exit('ERROR build_sfx: %s does not fit %d B even at %d Hz' % (folder, min(stage_budget, STAGE_CAP),
                                                                                 RATES[-1]))
            free_size = sum(vag_size(info[p][0], info[p][1], RATES[free[p]]) for p in names)
            for p in names:
                if rate[p] != free[p]:
                    d = drops.setdefault(p, [RATES[free[p]], RATES[rate[p]], 0])
                    d[0], d[1], d[2] = max(d[0], RATES[free[p]]), min(d[1], RATES[rate[p]]), d[2] + 1
                detail.append('%-9s %-40s %5d -> %5d Hz' % (folder, p, RATES[free[p]], RATES[rate[p]]))
            entries = [(i, RATES[rate[p]], enc.encode(p, RATES[rate[p]])) for i, p in sorted(paths.items())]
            size = write_bank(os.path.join(out_data, 'Stages', folder, 'PS1Sfx.bin'), entries)
            assert size <= min(stage_budget, STAGE_CAP), folder
            lowest = min((RATES[rate[p]] for p in names), default=44100)
            if lowest < FLOOR:
                low.append(folder)
            hist = {}
            for p in names:
                hist[RATES[rate[p]]] = hist.get(RATES[rate[p]], 0) + 1
            report.append('%-9s %2d sfx %7d -> %7d B | %s' % (folder, len(entries), free_size, size,
                                                     ' '.join('%d:%d' % (r // 1000, hist[r]) for r in sorted(hist, reverse=True))))
    print('sfx resident %d B (%s) | menu bank %d B (%s) | stage budget %d B of %d, capped at %d' % (
        rsize, ', '.join(gnames[i] for i in resident), msize, ', '.join(sfx_sets.MENU_NAMES), stage_budget, budget,
        STAGE_CAP))
    for r in report:
        print('  ' + r)
    print('  rates the cap lowered (highest without -> lowest with, stages): ' + (', '.join(
        '%s %d->%d (%d)' % (os.path.splitext(p)[0], a // 1000, b // 1000, n) for p, (a, b, n) in sorted(drops.items()))
        or '-'))
    rep = os.path.normpath(os.path.join(out_data, '..', '..', 'sfx_rates.txt'))
    with open(rep, 'w') as f:
        f.write('# Stage SFX rates without -> with the %d-byte cap (tools/audio/build_sfx.py)\n' % STAGE_CAP)
        f.write('\n'.join(detail) + '\n')
    if low:
        print('  below %d Hz: %s' % (FLOOR, ' '.join(low)))


if __name__ == '__main__':
    main()
