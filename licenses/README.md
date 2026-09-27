# Third-party licenses

This repository contains the builder scripts, part of the RSDKv4 decompilation (for the host tool that runs the
scripts' startups to find the sprites each stage uses) and a prebuilt PlayStation executable (`bin/Sonic2-PS1.exe`).
No game data.

## Covering this repository

| Component | Where | License |
|---|---|---|
| RSDKv4 decompilation (Rubberduckycooly, st×tic; original RSDK by Christian Whitehead) | `builder/RSDKv4/`, `builder/tools/rsdkmanifest/`, the engine in `bin/Sonic2-PS1.exe` | [../LICENSE.md](../LICENSE.md) — non-commercial, credit the authors, no game assets, DLC off in pre-built executables (the executable is built with `RSDK_AUTOBUILD`) |
| PS1 port code and builder scripts | `build_iso.py`, `builder/`, the port in `bin/Sonic2-PS1.exe` | [../LICENSE.md](../LICENSE.md) (same terms) |

## Compiled into `bin/Sonic2-PS1.exe`

| Component | Authors | License |
|---|---|---|
| psyqo / nugget (PS1 SDK) | PCSX-Redux authors | MIT — [psyqo-nugget-MIT.txt](psyqo-nugget-MIT.txt) |
| EASTL, EABase (C++ containers, via psyqo) | Electronic Arts | BSD 3-Clause — [EASTL-BSD-3-Clause.txt](EASTL-BSD-3-Clause.txt), [EABase-BSD-3-Clause.txt](EABase-BSD-3-Clause.txt) |
| SPU / CD-ROM driver model (ps1-bare-metal) | spicyjpeg | MIT — [ps1-bare-metal-MIT.txt](ps1-bare-metal-MIT.txt) |
| Loading icon and memory card icon | fan-made pixel art, author unknown (contact us for credit) | credited to its author |

## External tools (installed by you, not included)

| Tool | License |
|---|---|
| [mkpsxiso](https://github.com/Lameguy64/mkpsxiso) | GPL 2.0 or later |
| [psxavenc](https://codeberg.org/WonderfulToolchain/psxavenc) | zlib-style — [psxavenc-license.txt](psxavenc-license.txt) |
| [FFmpeg](https://ffmpeg.org) | LGPL 2.1+ / GPL 2+ (depending on the build) |
| [Pillow](https://python-pillow.org), [NumPy](https://numpy.org) | MIT-CMU / BSD |
