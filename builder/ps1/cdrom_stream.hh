/*
 * Continuous CD-ROM sector streaming for the PS1 port (STR video, later audio streams).
 *
 * One SETLOC + READN keeps the drive reading sequentially; every delivered sector is
 * DMA'd from the data-ready IRQ into a caller-provided ring buffer. When the ring is
 * full (or the stream ends / is closed) the action sends PAUSE and completes;
 * PS1StreamPump() later restarts it with an explicit SETLOC at the exact next LBA.
 *
 * Why not "pause, then READN again without SETLOC" (the old readSectorsSequential)?
 * psx-spx cdromdrive.md "Setloc, Read, Pause": after a Pause, a new Read without Setloc
 * resumes at the most recently received sector, returning it a second time. PCSX-Redux
 * does not emulate that, so it only breaks on real hardware. An explicit SETLOC is
 * correct everywhere, and continuous reading avoids seeks altogether.
 *
 * Implemented as a psyqo::CDRomDevice::Action subclass in the port (the Action template
 * is public), so nugget stays unmodified. One CD action at a time: don't issue other
 * file reads while a stream is running.
 */
#pragma once

#include <stdint.h>

// ring: ringSectors * 2048 bytes, word-aligned, owned by the caller.
bool     PS1StreamOpen(uint32_t lba, uint32_t totalSectors, uint8_t *ring, uint32_t ringSectors);
// STR with interleaved XA audio (phase 5): Setfilter(xaFile, xaChannel) + Setmode 2x | XA-ADPCM |
// filter + ReadS. Audio sectors go to the drive's decoder (never to the CPU); totalSectors counts
// the data (video) sectors only. The drive is never paused on purpose (the audio would stop):
// when the ring is full the newest sector is dropped and the reader resyncs to the next frame.
bool     PS1StreamOpenXA(uint32_t lba, uint32_t totalSectors, uint8_t *ring, uint32_t ringSectors, uint8_t xaFile, uint8_t xaChannel);
void     PS1StreamPump();                 // call every frame: restarts reading when the ring has room
uint32_t PS1StreamAvailable();            // sectors ready to consume
const uint8_t *PS1StreamSector(uint32_t i); // i-th ready sector (0 = oldest), valid until consumed
void     PS1StreamConsume(uint32_t n);    // release n sectors
bool     PS1StreamEnded();                // all sectors delivered and consumed
bool     PS1StreamError();                // the drive reported an error
uint32_t PS1StreamDelivered();            // sectors delivered into the ring since open
void     PS1StreamClose();                // stop reading (sends PAUSE now if running); waits until idle, ~3 s at most

// Verification counters (GDB).
extern volatile uint32_t g_ps1StreamRestarts;     // SETLOC restarts after the ring filled up
extern volatile uint32_t g_ps1StreamDiscarded;    // sectors dropped while pausing (re-read later)
extern volatile uint32_t g_ps1StreamInt1;         // data-ready IRQs seen by the stream
extern volatile uint32_t g_ps1StreamCloseTimeout; // closes that gave up waiting for the drive
