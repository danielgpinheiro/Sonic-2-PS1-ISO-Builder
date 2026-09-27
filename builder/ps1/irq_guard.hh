/*
 * Lost-IRQ guard (the FMV hang on the PSone: docs/31, from the Sonic CD port 8d7c1cd).
 *
 * I_STAT bits are edge-triggered and are acknowledged by writing 0 to that bit alone (psx-spx
 * interrupts.md "Interrupt Acknowledge"). psyqo acknowledges with a read-modify-write
 * (IRQReg::clear: I_STAT &= ~bit), which also acknowledges any other IRQ that fires between the read
 * and the write, unseen. For the CD-ROM and the DMA that loss is permanent: the device keeps its flag
 * set, so no new edge ever comes. The drive then delivers no more data sectors while its XA audio plays
 * on (the FMV hang, reproduced in DuckStation with the SCPH-101 BIOS), and lost DMA IRQs would stop the
 * GPU callbacks the same way. PCSX-Redux never splits the read-modify-write, so it never showed there.
 *
 * nugget stays upstream, so the port repairs it: a periodic psyqo timer (it runs inside every
 * pumpCallbacks wait too) looks for a device flag without its I_STAT bit, with that IRQ masked so its
 * handler can't be half-way, and makes a new edge by toggling the device's interrupt enable. A BIOS
 * IRQ-chain filter clears the opposite case, a CD latch with no response behind it, which would make
 * psyqo's CDRomDevice::irq() abort (see cdIrqFilter).
 */
#pragma once

#include <stdint.h>

void PS1IrqGuardInstall(); // once, after the CD-ROM device is ready (start())
void PS1IrqGuardCheck();   // one check (the timer calls it; safe from the main loop at any time)

extern volatile uint32_t g_ps1CdIrqKicks;  // lost CD-ROM IRQ edges re-raised
extern volatile uint32_t g_ps1DmaIrqKicks; // lost DMA IRQ edges re-raised
extern volatile uint32_t g_ps1CdIrqStale;  // CD IRQ latches with no response behind them, cleared before psyqo
