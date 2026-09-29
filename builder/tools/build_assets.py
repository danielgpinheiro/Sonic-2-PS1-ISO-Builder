#!/usr/bin/env python3
"""Every build-time conversion of the Sonic 2 PS1 disc (docs/30 phases 1-9, 11): one pipeline for the port
(tools/data_disc.sh) and the public ISO builder (build_iso.py), so both make the same image.

From the game's datapack (--rsdk: the Android / iOS Data.rsdk, "RSDKvB") or an already extracted tree (--src: Data/ +
Bytecode/), into OUT/build/iso (the disc tree) and OUT/build/manifest (the sprite manifests):
  1 data      extract (tools/extract_rsdk.py with tools/rsdkv4_filelist.txt), copy Data/ + Bytecode/ without the PC
              source assets (.gif .wav .ogg), the native menus' art and meshes (Data/Game/Menu, Data/Game/Models) and
              Egg Gauntlet (ZoneM, user decision 2026-09-25)
  1 scripts   the bytecode's PS1 edits (scripts/patch_bytecode.py) and the stages that get the Scene3D arrays
  2 manifest  the host build of the engine (rsdkmanifest: every stage's startups, per player variant) -> sprite
              manifests, the packed chunk tiles / collision masks and the script images (Scripts.ps1)
  3 atlas     one sprite atlas per stage per variant ('' Sonic, _t Tails, _k Knuckles, _st Sonic & Tails)
  4 tiles     tile pages (16x16Tiles.vram); 5 strips: BG line strips; 6 menus: the PS1 menus' art, fonts, strings
  7 sfx       SPU-ADPCM banks (400 KB stage cap); 8 music: CD-XA (44 tracks, loop rows from the scripts)
  9 layout    drops the originals the console no longer reads, stage-local copies, the load order
The disc itself: tools/disc/build_disc.py (make disc / make dist, or the builder's build_iso.py).

Usage: build_assets.py (--rsdk DATA.rsdk | --src EXTRACTED_DIR) [--out DIR] [--order LOAD_ORDER.txt] [--disc-tools]
"""
import argparse, fnmatch, importlib.util, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..'))
PY = sys.executable
VARIANTS = (('', 0), ('_t', 1), ('_k', 2), ('_st', 3))
WORKSPACE_TOOLS = {  # the development workspace's builds, used when nothing else is configured
    'PSXAVENC': os.path.join(REPO, '..', 'psxavenc', 'build', 'psxavenc'),
    'MKPSXISO': os.path.join(REPO, '..', 'mkpsxiso-2.30-Darwin', 'bin', 'mkpsxiso'),
}


def find_tool(env, name):
    path = os.environ.get(env) or shutil.which(name)
    if not path and os.path.exists(WORKSPACE_TOOLS.get(env, '')):
        path = WORKSPACE_TOOLS[env]
    return path


def check_tools(need_disc=False):
    """The external tools (as environment variables for the converters); exits with a list if any is missing."""
    tools, missing = {}, []
    wanted = [('PSXAVENC', 'psxavenc'), ('FFMPEG', 'ffmpeg')]
    if need_disc:
        wanted.append(('MKPSXISO', 'mkpsxiso'))
    for env, name in wanted:
        p = find_tool(env, name)
        (tools.__setitem__(env, os.path.abspath(p)) if p else missing.append('%s (or set %s)' % (name, env)))
    cxx = os.environ.get('CXX') or shutil.which('clang++') or shutil.which('g++') or shutil.which('c++')
    (tools.__setitem__('CXX', cxx) if cxx else missing.append('a C++17 compiler: clang++ or g++ (or set CXX)'))
    if not shutil.which('make'):
        missing.append('make')
    for mod, pkg in (('PIL', 'Pillow'), ('numpy', 'numpy')):
        if importlib.util.find_spec(mod) is None:
            missing.append('Python package %s (pip install %s)' % (pkg, pkg))
    if missing:
        sys.exit('Missing tools:\n  ' + '\n  '.join(missing))
    return tools


def run(step, cmd, log=None, **kw):
    shown = ['python3' if c == PY else os.path.relpath(c, REPO) if os.path.isabs(c) else c for c in cmd]
    print('[%s] %s' % (step, ' '.join(shown)), flush=True)
    if log:
        with open(log, 'w') as f:
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, **kw)
        if r.returncode:
            sys.stdout.write(open(log).read())
            sys.exit('[%s] failed (%s)' % (step, log))
        return r
    return subprocess.run(cmd, check=True, **kw)


def copy_tree(src, dst, excludes):
    """rsync -a --exclude ...: copy src into dst, skipping names / paths matching the patterns (a trailing / = a dir)."""
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        rel = '' if rel == '.' else rel.replace(os.sep, '/') + '/'
        dirs[:] = sorted(d for d in dirs if not any(p.endswith('/') and fnmatch.fnmatch(rel + d + '/', '*' + p) for p in excludes))
        os.makedirs(os.path.join(dst, rel), exist_ok=True)
        for f in sorted(files):
            if any(not p.endswith('/') and fnmatch.fnmatch(f, p) for p in excludes):
                continue
            shutil.copy2(os.path.join(root, f), os.path.join(dst, rel, f))


def main():
    ap = argparse.ArgumentParser(description='Convert the Sonic 2 (docs/30) or Sonic 1 (docs/37) data for the PS1 disc.')
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--rsdk', help='the Android / iOS Data.rsdk')
    g.add_argument('--src', help='an extracted tree (Data/ + Bytecode/)')
    ap.add_argument('--out', default=REPO, help='where build/iso and build/manifest go (default: the repo)')
    ap.add_argument('--order', help='the load order file to write (default: tools/disc/load_order.txt)')
    ap.add_argument('--disc-tools', action='store_true', help='also require mkpsxiso (the disc step that follows)')
    ap.add_argument('--game', choices=('1', '2'), default='2', help='2 = Sonic 2 (default), 1 = Sonic 1 (docs/37)')
    a = ap.parse_args()
    s1 = a.game == '1'
    tools = check_tools(a.disc_tools)
    env = dict(os.environ, **tools)
    out = os.path.abspath(a.out)
    iso, mdir = os.path.join(out, 'build', 'iso'), os.path.join(out, 'build', 'manifest')
    data = os.path.join(iso, 'Data')

    src = os.path.abspath(a.src) if a.src else os.path.join(out, 'build', 'extracted')
    if a.rsdk:
        shutil.rmtree(src, ignore_errors=True)
        lst = os.path.join(HERE, 'rsdkv4_filelist_s1.txt' if s1 else 'rsdkv4_filelist.txt')
        run('1 data', [PY, os.path.join(HERE, 'extract_rsdk.py'), os.path.abspath(a.rsdk), src, '--list', lst], env=env)
    sdata, sbc = os.path.join(src, 'Data'), os.path.join(src, 'Bytecode')
    shutil.rmtree(iso, ignore_errors=True)
    print('[1 data] copy the data the console reads -> %s' % os.path.relpath(iso, REPO), flush=True)
    copy_tree(sdata, data, ['*.gif', '*.wav', '*.ogg', '.DS_Store', 'Game/Menu/', 'Game/Models/', 'Stages/ZoneM/'])
    copy_tree(sbc, os.path.join(iso, 'Bytecode'), ['ZoneM.bin', '.DS_Store'])
    bc = os.path.join(iso, 'Bytecode')
    run('1 scripts', [PY, os.path.join(HERE, 'scripts', 'patch_bytecode.py'), bc, '--game', a.game], env=env)
    run('1 scripts', [PY, os.path.join(HERE, 'scripts', 'scene3d_stages.py'), bc, data], env=env)

    run('2 manifest', ['make', '-s', '-C', os.path.join(HERE, 'rsdkmanifest'), 'CXX=' + tools['CXX'], 'GAME=' + a.game],
        env=env)
    manifest_tool = os.path.join(HERE, 'rsdkmanifest', 'rsdkmanifest-s1' if s1 else 'rsdkmanifest')
    shutil.rmtree(mdir, ignore_errors=True)
    os.makedirs(mdir)
    for suffix, player in VARIANTS:
        pdir = os.path.join(mdir, 'p%d' % player)
        run('2 manifest', [manifest_tool, iso, pdir, str(player)],
            log=os.path.join(mdir, 'p%d.log' % player), env=env)
        run('3 atlas', [PY, os.path.join(HERE, 'atlas', 'build_atlas.py'), sdata, pdir, '--suffix', suffix,
                        '--out', os.path.join(data, 'Sprites', 'Atlas')], log=os.path.join(mdir, 'atlas%s.log' % suffix), env=env)
    run('4 tiles', [PY, os.path.join(HERE, 'tiles', 'convert_tiles.py'), sdata, data], env=env)
    run('5 strips', [PY, os.path.join(HERE, 'bgstrips', 'build_bgstrips.py'), data] + (['--game', '1'] if s1 else []),
        log=os.path.join(mdir, 'bgstrips.log'), env=env)  # Sonic 1: BGS3 (segment maps, 4-bit rows; docs/37 phase 3)
    run('6 menus', [PY, os.path.join(HERE, 'menu', 'build_menu.py'), sdata, data] + (['--game', '1'] if s1 else []), env=env)
    run('7 sfx', [PY, os.path.join(HERE, 'audio', 'build_sfx.py'), sdata, data, sbc], env=env)
    run('8 music', [PY, os.path.join(HERE, 'audio', 'build_music.py'), sdata, data, sbc] + (['--game', '1'] if s1 else []), env=env)

    # the console reads 128x128Tiles.ps1 / CollisionMasks.ps1 (written by rsdkmanifest with the engine's own loaders)
    for st in sorted(os.listdir(os.path.join(data, 'Stages'))):
        d = os.path.join(data, 'Stages', st)
        for packed, orig in (('128x128Tiles.ps1', '128x128Tiles.bin'), ('CollisionMasks.ps1', 'CollisionMasks.bin')):
            if os.path.exists(os.path.join(d, packed)) and os.path.exists(os.path.join(d, orig)):
                os.remove(os.path.join(d, orig))
    run('9 layout', [PY, os.path.join(HERE, 'disc', 'stage_local.py'), iso, mdir], env=env)
    order = [PY, os.path.join(HERE, 'disc', 'gen_load_order.py'), iso]
    if a.order or s1:  # Sonic 1: its own boot trace and order file (the Makefile's disc / dist read it for GAME=1)
        order += [os.path.join(HERE, 'disc', 'boot_trace_s1.txt' if s1 else 'boot_trace.txt'),
                  os.path.abspath(a.order) if a.order else os.path.join(HERE, 'disc', 'load_order_s1.txt')]
    run('9 layout', order, env=env)
    n = sum(len(f) for _, _, f in os.walk(iso))
    size = sum(os.path.getsize(os.path.join(r, x)) for r, _, fs in os.walk(iso) for x in fs)
    print('iso root: %d files, %.1f MB' % (n, size / 1048576))


if __name__ == '__main__':
    main()
