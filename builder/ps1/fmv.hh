/*
 * Full-screen FMV for Sonic CD (docs/28 phase 6): the engine's PlayVideoFile / ProcessVideo on the PS1.
 * Data/Videos/<Name>.str (tools/fmv/build_str.py: MDEC 320x176 @ 15 fps + JP / US XA audio) plays
 * letterboxed at Y 32. The video owns the CD (music stopped) and borrows the chain arenas' heap for
 * its buffers: open and close happen at a frame start (PS1FmvBeginFrame), when nothing is chained yet.
 */
#pragma once

#include <stdint.h>

bool PS1FmvRequest(const char *name, int soundtrack); // from PlayVideoFile: plays from the next frame
void PS1FmvBeginFrame();                             // from PS1RenderBeginFrame (after PS1ChainBegin)
// From ProcessVideo, once per frame: decodes / shows the video; skip = a button was pressed (fade out).
// 1 = finished and closed (the engine goes back to the game), 0 = playing.
int PS1FmvUpdate(bool skip);
bool PS1FmvActive(); // a video is open (audio: the CD input carries its XA)

extern volatile uint32_t g_ps1FmvPlays;   // videos opened
extern volatile uint32_t g_ps1FmvFails;   // requests that couldn't open (missing file / heap)
extern volatile uint32_t g_ps1FmvFrames;  // game frames in video mode
extern volatile uint32_t g_ps1FmvSkipped; // videos ended by a button
