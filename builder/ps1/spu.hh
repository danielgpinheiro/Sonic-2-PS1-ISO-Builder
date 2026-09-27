/*
 * PS1 SPU driver (phase 5), modelled on ps1-bare-metal src/common/spu.c (register sequence,
 * blocking DMA in 16-word chunks, voice setup) on nugget's common/hardware/spu.h registers.
 * psyqo's SPU class is not used: its startup initialize() runs first (kernel), then
 * PS1SpuInit() takes the SPU over: dummy block at the end of SPU RAM, all voices parked on it,
 * CD audio input enabled (volume 0 until music plays).
 *
 * SPU RAM: 0x0000-0x0FFF capture buffers (CD L/R, voice 1/3), samples from 0x1000,
 * dummy block at 0x7FFE0, reverb work area 0x7FFF0 (reverb off). Addresses in bytes.
 * Blocking calls only, outside the DMA-chained frame drawing (load time / frame start).
 */
#pragma once

#include <stdint.h>

#define PS1_SPU_SAMPLE_START 0x1000u
#define PS1_SPU_SAMPLE_END   (0x80000u - 32u) // dummy block above
#define PS1_SPU_PITCH_UNIT   4096             // pitch 0x1000 = 44100 Hz

void PS1SpuInit();
// Blocking SPU DMA. `data` 4-byte aligned; `len` a multiple of 64 bytes (16-word chunks);
// `addr` a multiple of 8.
void PS1SpuWrite(uint32_t addr, const void *data, uint32_t len);
void PS1SpuRead(uint32_t addr, void *data, uint32_t len);
// Start `voice` on the sample at `addr` (key on). Volumes 0..0x3FFF (fixed, no sweep).
void PS1SpuVoicePlay(int voice, uint32_t addr, uint32_t rate, uint16_t volL, uint16_t volR);
void PS1SpuVoiceVolume(int voice, uint16_t volL, uint16_t volR); // a playing voice's volumes (no key on)
void PS1SpuVoiceStop(uint32_t voiceMask); // key off
// Current ADSR envelope (ENVX). 0 once a key-on'd sample has ended: psxavenc ends every sample
// with an end-without-repeat block, which releases the voice with a zero envelope (psx-spx);
// PCSX-Redux stops the channel there. ENDX is not emulated by PCSX-Redux, so it isn't used.
uint16_t PS1SpuVoiceEnvelope(int voice);
void PS1SpuSetCdVolume(uint16_t left, uint16_t right); // CD/XA input volume, 0..0x7FFF

extern volatile uint32_t g_ps1SpuDmaBytes; // bytes written to SPU RAM since boot (GDB)
