/*
 * Loading indicator (phase 5b): a spinning Sonic in the bottom-right corner while the game loads
 * from the disc.
 *
 * Loads are blocking CD reads inside Engine.Init() / frame(); psyqo's blocking actions wait in
 * GPU::pumpCallbacks(), which runs GPU timers, so a periodic timer animates the icon during the
 * load. Nothing flips while loading, so each tick draws straight into the displayed buffer
 * (immediate GPU commands). The hidden framebuffer is scratch VRAM for the icon texture, its
 * CLUT and the saved background; PS1LoadingEnd copies the displayed frame over it, so the frame
 * the next flip shows is the one that stayed on screen during the load.
 *
 * PS1LoadingBegin/End bracket a load (nestable). The icon shows only if the load lasts more than
 * PS1_LOADING_DELAY_US, so short loads never flicker.
 */
#pragma once

#include <stdint.h>

#define PS1_LOADING_DELAY_US 250000

void PS1LoadingBegin();
// ps1/cdrom_fs.cpp: during a stage load, the stage whose local copies (Data/Stages/<folder>/PS1Local/) are read first
void PS1FsSetLoadFolder(const char *folder);
void PS1LoadingEnd();

extern volatile uint32_t g_ps1LoadingLoads;  // loads bracketed
extern volatile uint32_t g_ps1LoadingShown;  // loads long enough to show the icon
extern volatile uint32_t g_ps1LoadingTicks;  // icon frames drawn
extern volatile uint32_t g_ps1LoadingFrame;  // icon frame drawn last (0-7)
extern volatile uint32_t g_ps1LoadingLastUs;   // duration of the last load (microseconds)
extern volatile uint32_t g_ps1LoadingHeadroom; // heap never reached, at the end of the last load
