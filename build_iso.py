#!/usr/bin/env python3
"""Sonic 2 PS1 ISO Builder: your Sonic the Hedgehog 2 (2013) Data.rsdk -> a PlayStation disc image.

    python3 build_iso.py --data /path/to/Data.rsdk [--license licensea.dat] [--out output]

1. checks the tools (Python packages, psxavenc, ffmpeg, mkpsxiso, a C++17 compiler, make);
2. checks that Data.rsdk is the known Android / iOS release (SHA-256), unless --force;
3. converts the game's assets for the PlayStation (builder/tools/build_assets.py);
4. lays the files out in the order the game loads them and builds the disc around the prebuilt
   executable (bin/Sonic2-PS1.exe) with mkpsxiso;
5. verifies the image byte by byte (EDC/ECC, license sectors, file index, every file);
6. writes output/Sonic2-PS1.bin + .cue + SHA256SUMS.

No game data is included in this repository: use your own copy of the game (see README.md).
"""
import argparse, hashlib, os, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.abspath(__file__))
BUILDER = os.path.join(ROOT, 'builder')
EXE = os.path.join(ROOT, 'bin', 'Sonic2-PS1.exe')
# Sonic the Hedgehog 2 (2013 remaster), the Android / iOS datapack ("RSDKvB").
DATA_SHA256 = '3b2346d4bda49ee09880dfdfc42cc40d70c3a932a4ade06087b8072756630b14'

sys.path.insert(0, os.path.join(BUILDER, 'tools'))
import build_assets  # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description='Build a Sonic 2 PlayStation disc image from your own Data.rsdk.')
    ap.add_argument('--data', required=True, help='Data.rsdk from your copy of Sonic the Hedgehog 2 (2013, Android / iOS)')
    ap.add_argument('--license', help='optional Sony license file (e.g. licensea.dat, NTSC-U) for retail consoles')
    ap.add_argument('--out', default=os.path.join(ROOT, 'output'), help='output folder (default: output/)')
    ap.add_argument('--force', action='store_true', help='accept a Data.rsdk that is not the known release')
    ap.add_argument('--keep-work', action='store_true', help='keep the intermediate files (output/work)')
    a = ap.parse_args()

    if sys.version_info < (3, 8):
        sys.exit('Python 3.8 or newer is required.')
    if not os.path.isfile(a.data):
        sys.exit('Data.rsdk not found: %s' % a.data)
    if a.license and not os.path.isfile(a.license):
        sys.exit('License file not found: %s' % a.license)
    tools = build_assets.check_tools(need_disc=True)
    got = sha256(a.data)
    if got != DATA_SHA256:
        msg = 'Data.rsdk is not the known Android / iOS release (SHA-256 %s).' % got
        if not a.force:
            sys.exit(msg + '\nUse the unmodified file from your copy of the game, or pass --force to try anyway.')
        print('WARNING: ' + msg + ' Continuing because of --force.')

    name = 'Sonic2-PS1'
    out = os.path.abspath(a.out)
    work = os.path.join(out, 'work')
    if os.path.exists(work):
        shutil.rmtree(work)
    os.makedirs(work)
    order = os.path.join(work, 'load_order.txt')
    env = dict(os.environ, PS1_LOAD_ORDER=order, **tools)
    py = sys.executable
    t0 = time.time()
    subprocess.run([py, os.path.join(BUILDER, 'tools', 'build_assets.py'), '--rsdk', os.path.abspath(a.data), '--out', work,
                    '--order', order, '--disc-tools'], env=env, check=True)
    iso = os.path.join(work, 'build', 'iso')
    prefix = os.path.join(out, name)
    lic = os.path.abspath(a.license) if a.license else 'none'
    print('[disc] mkpsxiso', flush=True)
    subprocess.run([py, os.path.join(BUILDER, 'tools', 'disc', 'build_disc.py'), iso, EXE, lic, prefix], env=env, check=True)
    print('[verify]', flush=True)
    subprocess.run([py, os.path.join(BUILDER, 'tools', 'disc', 'verify_disc.py'), prefix + '.bin', iso, EXE, lic],
                   env=env, check=True)
    with open(os.path.join(out, 'SHA256SUMS'), 'w') as f:
        for ext in ('.bin', '.cue'):
            f.write('%s  %s\n' % (sha256(prefix + ext), name + ext))
    if not a.keep_work:
        shutil.rmtree(work)
        shutil.rmtree(os.path.join(out, 'disc'), ignore_errors=True)
    print('\nDone in %d min: %s.cue / .bin%s' % ((time.time() - t0 + 59) // 60, prefix, '' if a.license else
                                                   '\n(no license file: plays in emulators, on ODEs and modded consoles)'))


if __name__ == '__main__':
    main()
