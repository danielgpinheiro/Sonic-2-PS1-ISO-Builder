/*
 * Build stamp: the git commit of the build (VERSION.TXT at the disc root, written by tools/disc/build_disc.py with
 * tools/gen_version.py) drawn in the bottom-left corner of the menus' title screen, e.g. "v a1b2c3d4" /
 * "v a1b2c3d4-dirty", "v unknown" when the disc has no VERSION.TXT.
 *
 * PS1VersionLoad (boot, before the first load) reads the file with one sector read into a stack buffer (no heap, no
 * read-ahead window) into a 32-byte static text. ps1/menu.cpp draws it with the menu pack's small label font
 * (drawText; the font has no lower case, so the hash shows in capitals), under the menu's fade. No texture and no VRAM
 * of its own: Sonic CD's self-drawn stamp (RSDKv3-ps1 ps1/version_stamp.cpp) cost ~2 KB of code, which the special
 * stage's heap headroom can't spare here.
 */
#pragma once

#include <stdint.h>

void PS1VersionLoad();        // boot: VERSION.TXT -> the stamp text
const char *PS1VersionText(); // "v a1b2c3d4"

extern volatile uint32_t g_ps1VersionFound; // 1 = VERSION.TXT was read
