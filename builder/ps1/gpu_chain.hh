/*
 * PS1 DMA-chained renderer helpers (psyqo "fragment chaining").
 *
 * Every frame, the engine's draw calls append primitives to a per-parity arena and
 * link them into psyqo's DMA chain; psyqo sends the chain after frame() returns
 * (during the next frame), so CPU game logic and GPU drawing overlap.
 *
 * Rules (psyqo CONCEPTS.md "Fragment chaining"):
 *  - once chaining, ALL per-frame drawing must be chained (no sendPrimitive /
 *    sendFragment / clear / uploadToVRAM while a chain may be in flight);
 *  - a chained fragment is at most 255 words;
 *  - the chain draws into the buffer that becomes current after the flip, so the
 *    clear uses gpu().getNextClear();
 *  - any immediate GPU operation (tile-set / sprite-sheet uploads at load time) must
 *    call PS1GpuImmediate() first, which waits for the in-flight chain to finish.
 */
#pragma once

#include <stdint.h>

#include <psyqo/primitives/common.hh>
#include <psyqo/primitives/quads.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/primitives/misc.hh>
#include <psyqo/primitives/sprites.hh>

namespace psyqo {
class GPU;
}

void PS1ChainBegin(psyqo::GPU &gpu); // start of a frame: select the arena for this parity
void PS1ChainEnd();                  // end of a frame: close the open tile batch
void PS1GpuImmediate();              // wait for the in-flight chain before an immediate GPU op
// The arenas live on the heap, time-shared with the CD read-ahead windows: a load (ps1/loading)
// releases them (only if this frame chained nothing yet) and gets them back at its end.
void PS1ChainSuspend();
void PS1ChainResume();
bool PS1ChainHasArena(); // false while suspended for a load (or the heap had no room): nothing can be chained
// Arena size (words per parity) of the next allocation (PS1ChainResume); 0 = the normal 64 KB. An FMV
// plays with tiny arenas and lends their heap to the video buffers (ps1/fmv.cpp).
void PS1ChainSetArenaWords(uint32_t words);
int PS1ChainBufferY(); // VRAM Y of the buffer this frame draws into (0 or 256)

// FastFill the next draw buffer. Returns the chained primitive (its colour may still be
// changed until the frame ends: the chain is sent after frame() returns), or nullptr.
psyqo::Prim::FastFill *PS1ChainClear(uint8_t r = 0, uint8_t g = 0, uint8_t b = 0);
void PS1ChainUploadCLUT(const uint16_t *clut256, int16_t x, int16_t y); // 256-entry CLUT + cache flush
void PS1ChainUploadRow(const uint16_t *data, int n, int16_t x, int16_t y); // n (even) halfwords, no flush
void PS1ChainFlushCache();                                     // after CLUT/texture changes
// Copy the last finished frame (the displayed buffer) into the buffer this frame draws into,
// so a frame that draws nothing (paused game) shows the same image in both buffers.
void PS1ChainRepeatLastFrame();
// Chains a row upload of `count` (even) halfwords whose data is written later, before the
// frame ends: keeps its early place in the chain. Returns the number of chunks (data pointers
// in chunkData[], halfword counts in chunkLen[]), 0 if the arena is full.
int PS1ChainReserveRow(int count, int16_t x, int16_t y, uint16_t **chunkData, uint16_t *chunkLen, int maxChunks);
void PS1ChainTPage(psyqo::PrimPieces::TPageAttr attr);         // skipped if identical to the last one
void PS1ChainInvalidateTPage();                                // GPU texpage state unknown
void PS1ChainSprite16(const psyqo::Prim::Sprite16x16 &s);      // batched (tiles)
// Fast tile path: a Sprite16x16 from its raw words (colour 0x80, xy = x | y << 16, tex = u | v << 8 | clut << 16).
void PS1ChainSprite16Raw(uint32_t xy, uint32_t tex);
// Bulk version for tile bands: n 16x16 sprites (colour 0x80) from xy[] / tex[] words, written straight
// into exact-size fragments of <= 4 sprites (no per-tile call or constructor). Keeps the draw order.
void PS1ChainSprite16RawN(const uint32_t *xy, const uint32_t *tex, int n);
// Bulk variable-size sprites (GP0 64h, colour 0x80): n sprites of 3 words each {xy, u|v<<8|clut<<16,
// w|h<<16} in `words`, into exact-size fragments of <= 3 sprites (12 words). Keeps the draw order.
void PS1ChainSpriteRawN(const uint32_t *words, int n);
// Bulk raw GP0 primitives of `wpp` words each (e.g. 9 = textured quad), as many per fragment as fit in
// 14 words. Textured polygons carry their own texpage: the tracked texpage state is invalidated.
// One primitive of `words` (<= 14) written straight into the chain: PS1ChainRawBegin returns where to write
// (nullptr: the arena is full), PS1ChainRawEnd links it. Saves a staging copy for many small primitives.
uint32_t *PS1ChainRawBegin(int words);
void PS1ChainRawEnd();
void PS1ChainPrimsRawN(const uint32_t *words, int wpp, int n);
// Up to n primitives of wpp (<= 14) words, one fragment each, written straight into the arena and linked
// with a single chain() call: PS1ChainBulkBegin returns where primitive 0's words go (primitive k at
// + k * (wpp + 1)), nullptr if the arena is full; PS1ChainBulkEnd(used) links the first `used` of them.
uint32_t *PS1ChainBulkBegin(int n, int wpp);
void PS1ChainBulkEnd(int used);
void PS1ChainSprite(const psyqo::Prim::Sprite &s);
void PS1ChainQuad(const psyqo::Prim::TexturedQuad &q);         // note: quads carry their own texpage
void PS1ChainRect(const psyqo::Prim::Rectangle &r);            // untextured; semi-trans uses the current texpage mode

// Replay (phase 3c partial tint): PS1ChainMark() closes the open batch and returns the arena
// position of the next fragment; PS1ChainReplay(from, to) re-chains copies of the fragments
// linked from `from` up to (not including) `to`, skipping CPU->VRAM uploads. Returns words used.
uint32_t *PS1ChainMark();
int PS1ChainReplay(uint32_t *from, uint32_t *to);
// Same, keeping only fragments whose primitives touch screen lines [y0, y1) (state-only fragments
// always): per-line palette-bank regions (ps1/render.cpp) replay just what they need.
int PS1ChainReplayLines(uint32_t *from, uint32_t *to, int y0, int y1);
// Drawing area (GP0 E3h/E4h) in screen coordinates of the buffer this frame draws into;
// Reset restores the whole 320x240 buffer. Mask control = GP0 E6h (psx-spx "Mask Bit Setting").
void PS1ChainDrawArea(int x0, int y0, int x1, int y1);
// Lines [y0, y1) of the buffer this frame draws into: filled with a colour (FastFill ignores the
// drawing area), or copied from the last finished frame (the displayed buffer).
void PS1ChainFillLines(int y0, int y1, uint8_t r, uint8_t g, uint8_t b);
void PS1ChainCopyLastFrameLines(int y0, int y1);
// GP0(80h) VRAM-to-VRAM copy (halfwords), chained; flush the texture cache before sampling it.
void PS1ChainVramCopy(int sx, int sy, int dx, int dy, int w, int h);
void PS1ChainDrawAreaReset();
void PS1ChainMaskControl(bool forceSet, bool test);
// One-shot hook, run before the next primitive/upload is chained (or by PS1ChainRunHook()).
void PS1ChainSetHook(void (*fn)());
void PS1ChainRunHook();

// Debug/verification counters (read with GDB).
extern volatile uint32_t g_ps1ChainOverflow;  // primitives dropped because the arena was full
extern volatile uint32_t g_ps1ChainHighWater; // max arena words used in one frame
extern volatile uint32_t g_ps1ChainLastWords; // arena words used by the last frame
extern volatile uint32_t g_ps1ChainReplayWords; // words re-chained by PS1ChainReplay this frame
// Verification aid: while true, Sprite16/Sprite/Quad/Rect are dropped (CLUT uploads, clears
// and texpages still go through). Drawing.cpp sets it around object lists for layers-only frames.
extern bool g_ps1ChainSkipPrims;
