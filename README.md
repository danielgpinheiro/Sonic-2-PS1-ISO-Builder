# Sonic 2 PS1 ISO Builder

Build a PlayStation 1 disc image of **Sonic the Hedgehog 2** (the 2013 remaster, running on the Retro Engine v4)
from your own copy of the game. The PS1 executable is prebuilt; this repository contains the tools that convert the
game's data for the PlayStation and put the disc together.

**No game data is included.** You need your own copy of the game's `Data.rsdk`, see [Usage](#usage).

## What does this do / why is it needed?

The PlayStation has 2 MB of RAM, 1 MB of video RAM, 512 KB of sound RAM, no floating point and a 2× CD-ROM drive.
The 2013 version decodes GIFs, Ogg music and the game's script bytecode while it runs; the PS1 can't afford that. So
everything is converted **once, on your computer**, into formats the PS1 hardware uses directly:

- **Sprites** → one texture atlas per stage and per player (Sonic, Tails, Knuckles, Sonic & Tails: 4/8-bit VRAM pages
  + palettes), checked pixel-exact.
- **Tiles and parallax backgrounds** → pre-packed VRAM tile pages and pre-rendered background line strips.
- **Scripts** → the game's own bytecode, converted into the engine's in-memory form per stage, with the PS1 edits
  below.
- **Menus** → the 2013 menus (Start Game, Time Attack, Options) redrawn in 2D from the game's own art and fonts.
- **Sound effects** → SPU-ADPCM, a rate per file so every stage's set fits the sound RAM.
- **Music** → CD-XA audio (44 tracks), streamed and decoded by the CD drive, with the game's loop points.
- **Disc layout** → files placed in the order the game loads them, shared files copied next to each stage, a file
  index so opening a file needs no directory reads.

## Usage

1. Get `Data.rsdk` from your copy of **Sonic the Hedgehog 2 (2013)** for Android or iOS (unmodified; the builder checks
   its SHA-256). TODO(user): how to get it from the installed game.
2. Install the [requirements](#requirements).
3. Run:
   ```bash
   python3 build_iso.py --data /path/to/Data.rsdk
   ```
   Options: `--license licensea.dat` (see [The license file](#the-license-file)), `--out folder`, `--keep-work`,
   `--force` (accept a `Data.rsdk` that isn't the known release).
4. The disc image is in `output/` after about 2 minutes on a recent Mac.

## Output

- `output/Sonic2-PS1.cue` + `output/Sonic2-PS1.bin`: a Mode 2 BIN/CUE image (124 MB). It has to be BIN/CUE, not `.iso`:
  the CD-XA music uses 2336-byte Mode 2 sectors, which a 2048-byte ISO image can't hold.
- `output/SHA256SUMS`.

Built from the known `Data.rsdk` **without** a license file and with the tool versions listed under
[Requirements](#requirements), the image is `Sonic2-PS1.bin` SHA-256
`55a6e62c53f1977c4de87ec768db124a17c46c3c34fab04fbd0e1c55b41f4427`. Other versions of FFmpeg or psxavenc may encode the
audio slightly differently, which changes the hash but not the game. With a license file the license sectors differ, so
the hash does too.

Before writing it, the builder verifies the image byte by byte: sector headers, EDC/ECC of every sector, the license
sectors, the file index, and every file against its source.

## What the PS1 version can do

- The whole game: every zone including Hidden Palace, the special stages, bosses, Time Attack, the ending and credits.
- All four player choices: Sonic, Tails, Knuckles, Sonic & Tails.
- Saving on the **memory card in slot 1** (one block, with a Sonic 2 icon): save files, Time Attack records. Without a
  card the game plays normally and doesn't save.
- The special stages' half-pipe in 3D, transformed and projected by the PS1's geometry coprocessor.
- Options: music and sound effect volume, credits.
- An animated loading icon while the game reads from the disc (zones load in about 5 seconds).

## What the PS1 version can't do

- No Egg Gauntlet (the boss rush): its sprites don't fit the video RAM.
- No online features, 2-player versus, achievements or leaderboards.
- No touch controls, dev menu, settings file or mods from the mobile version.
- The special stages and a few busy zones (Aquatic Ruin, Hidden Palace, Wing Fortress) run below full speed in
  emulators (the special stages at about 70 %): the PS1 draws every frame instead of skipping some.
- Tested in emulators only (PCSX-Redux, including boot through a retail NTSC-U BIOS); not yet on a real console.

## Compromises to make this work

- Screen 320 px wide (the 2013 version is widescreen).
- Music is 4-bit CD-XA ADPCM at 37.8 kHz stereo (the PS1's streaming format).
- Sound effects are SPU-ADPCM at one rate per file, each stage's set capped at 400 KB for shorter loads: most sounds
  play at 16-44 kHz, the longest ones at 11 kHz (Hidden Palace's large set at 9-11 kHz).
- Transparency uses the PS1 GPU's 4 fixed blend levels.
- The special stages' 3D is computed with the geometry coprocessor's fixed-point maths: faces can differ from the
  original by a pixel at their edges.

## Requirements

Tested on macOS (Apple Silicon) with Python 3.9, Pillow 11, NumPy 2.0, FFmpeg 9, psxavenc (git, 2026-09), mkpsxiso 2.30,
clang 22 and make. Linux works the same way; on Windows, use WSL.

| Tool | What for |
|---|---|
| Python 3.8+ with Pillow and NumPy (`pip install -r requirements.txt`) | the converters |
| a C++17 compiler (clang++ or g++) and `make` | builds a small host tool from the engine's source (runs each stage's script startups to find its sprites, and converts the scripts) |
| FFmpeg (`ffmpeg`), with Vorbis decoding | decodes the game's Ogg audio |
| [psxavenc](https://codeberg.org/WonderfulToolchain/psxavenc) | encodes SPU-ADPCM and CD-XA |
| [mkpsxiso](https://github.com/Lameguy64/mkpsxiso) | writes the disc image |

The builder finds the tools on your `PATH`, or through the environment variables `PSXAVENC`, `MKPSXISO`, `FFMPEG` and
`CXX`. It stops with a list if anything is missing. Run it with the Python that has Pillow and NumPy installed.

**macOS** (Homebrew):
```bash
xcode-select --install                      # clang++, make
brew install python ffmpeg meson ninja pkg-config
python3 -m pip install -r requirements.txt
git clone https://codeberg.org/WonderfulToolchain/psxavenc && cd psxavenc
meson setup build && meson compile -C build && cd ..   # then PSXAVENC=$PWD/psxavenc/build/psxavenc
# mkpsxiso: download a release from https://github.com/Lameguy64/mkpsxiso/releases
#           (or build it with CMake) and put `mkpsxiso` on your PATH or in MKPSXISO
```

**Linux** (Debian/Ubuntu):
```bash
sudo apt install python3-pip build-essential ffmpeg meson ninja-build pkg-config \
     libavformat-dev libavcodec-dev libavutil-dev libswresample-dev libswscale-dev cmake
python3 -m pip install -r requirements.txt
# psxavenc and mkpsxiso: as above (meson for psxavenc, CMake or a release for mkpsxiso)
```

## The license file

PlayStation discs carry Sony's license data, which retail consoles check at boot. It can't be distributed, so by default
the image is built **without** it: it plays in emulators (PCSX-Redux, DuckStation, …), on optical drive emulators and on
modded consoles. If you have `licensea.dat` (NTSC-U) from the official SDK, pass `--license licensea.dat` to make a disc
that also boots on retail NTSC-U consoles.

## Playing it

- **Emulator:** open `Sonic2-PS1.cue` in PCSX-Redux or DuckStation. Put a memory card in slot 1 to save.
- **Real hardware:** burn the BIN/CUE at the slowest speed on a CD-R, or copy it to an optical drive emulator. Not yet
  tested on a console: reports are welcome.
- **Controls:** D-pad to move, Cross / Circle / Square to jump, Start to pause.

## How it's made

The executable in `bin/` is built from a PS1 port of the
[RSDKv4 decompilation](https://github.com/RSDKModding/RSDKv4-Decompilation) using
[psyqo](https://github.com/pcsx-redux/nugget/tree/main/psyqo), developed and tested with
[PCSX-Redux](https://github.com/grumpycoders/pcsx-redux). See `bin/README.md` for its version. `builder/` holds the
conversion tools, each documented in its header, and the part of the decompilation the host tool compiles.

PS1 edits to the game's scripts (applied at build time by `builder/tools/scripts/patch_bytecode.py`, each checked
instruction by instruction):
- The busiest script routines (the players, Tails' AI, the special stage's half-pipe, rings and player, and other
  objects) run natively in the executable, with identical results.
- A few loops over the object slots use the PS1's smaller object table.
- The mobile back key's pause is disabled: Start pauses.

## License & credits

- **Retro Engine (RSDK)** and **Sonic the Hedgehog 2 (2013)**: Christian "Taxman" Whitehead and Simon "Stealth"
  Thomley.
- **RSDKv4 decompilation**: Rubberduckycooly and st×tic
  ([RSDKModding](https://github.com/RSDKModding/RSDKv4-Decompilation)).
- **psyqo / nugget / PCSX-Redux**: the PCSX-Redux authors. **EASTL / EABase**: Electronic Arts.
- **[ps1-bare-metal](https://github.com/spicyjpeg/ps1-bare-metal)** (sound and CD-ROM driver model): spicyjpeg.
- **[mkpsxiso](https://github.com/Lameguy64/mkpsxiso)**: Lameguy64 and contributors.
  **[psxavenc](https://codeberg.org/WonderfulToolchain/psxavenc)**: Ben "GreaseMonkey" Russell and Adrian "asie"
  Siekierka.
- **[psx-spx](https://psx-spx.consoledev.net/)** hardware documentation: Martin "nocash" Korth and contributors.
- **Loading icon and memory card icon**: TODO(user): credit.

Licensed under the RSDKv3/v4 decompilation license ([LICENSE.md](LICENSE.md)). **Not for commercial use. No game assets
are distributed** — you build the disc from your own copy of the game. Third-party licenses are in
[licenses/](licenses/README.md). Sonic the Hedgehog is a trademark of SEGA; this project is not affiliated with or
endorsed by SEGA or Sony.
