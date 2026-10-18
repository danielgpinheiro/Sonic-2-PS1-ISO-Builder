#pragma once
#include <stdint.h>

// The main stack, moved out of the heap (V4-102, from RSDKv3-ps1 CD-092, ../docs/44). nugget's allocator sizes the heap
// up to __stack_start, the RAM top the BIOS stack grows down from: malloc never failed while the heap's last free block
// still overlapped the live stack (Sonic CD's R51A time-warp load handed its 32 KB stream windows out on top of it;
// here the Sonic 2 special stage in the TEST build, V4-095). main() runs the app on this .bss array instead: the heap
// owns everything up to __stack_start, malloc returns NULL at the true end (the windows fall back to 1-sector reads, as
// designed), and psyqo_heap_end() / g_ps1HeapHeadroom count bytes that are really free. psyqo's exception handler has
// its own stack (vector.s, 0x1000). 8 KB, not Sonic CD's 16: every heap guard of this port is tight (zones, card boot).
// Deepest stack measured 4,604 B (Sonic 1 Labyrinth; Sonic 2 4,444 B in ARZ / HPZ: stack checkpoints over every
// test-mode scenario, 2026-10-18).
#define PS1_STACK_BYTES (8 * 1024)

extern uint32_t g_ps1Stack[PS1_STACK_BYTES / 4];
extern volatile uint32_t g_ps1StackOverflow; // frames whose check found the stack's bottom guard words overwritten

int PS1RunOnStack(int (*fn)(), void *top); // calls fn with $sp = top (8-byte aligned), returns its result
void PS1StackPaint();                        // fill the array with the paint word (call before PS1RunOnStack)
bool PS1StackGuardBroken();                  // the bottom guard words are no longer paint
uint32_t PS1StackLowWater();                 // lowest address the stack has written (scan; for checkpoints / GDB)
