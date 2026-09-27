/*
 * Per-stage sprite atlas (phase 3d). Built at build time by tools/atlas/build_atlas.py
 * (format documented there): every frame a stage can draw, packed as 4-bit or 8-bit
 * texture clusters in the free VRAM columns, plus 16-entry CLUT maps for the 4-bit ones.
 * At stage load the VRAM blocks are streamed from CD straight into VRAM: sprites no longer
 * live in main RAM (no GraphicData) and are never decoded at runtime.
 */
#pragma once

#include <stdint.h>

struct PS1AtlasSheet {
    char name[48]; // path under Data/Sprites/, lower case
    uint16_t firstCluster, clusterCount, width, height;
};
struct PS1AtlasCluster {
    uint16_t srcX, srcY, w, h;  // rect in the sheet
    uint8_t tpageX, tpageY;     // texture page (pageX in 64-halfword units, pageY 0/1)
    uint8_t depth;              // 4 or 8
    uint8_t u, v;               // texel position inside the page
    uint8_t picture;            // 0 = resident, n = on-demand picture n-1 (tpage/u/v: its current slot)
    uint16_t clut;              // CLUT index (4-bit) or 0xFFFF = master CLUT (8-bit)
};
struct PS1AtlasClut {
    uint16_t x, y;      // VRAM position (16 halfwords)
    uint8_t map[16];    // master palette index per 4-bit value (map[0] = 0 = transparent)
};
static_assert(sizeof(PS1AtlasSheet) == 56, "atlas sheet record");
static_assert(sizeof(PS1AtlasCluster) == 16, "atlas cluster record");
static_assert(sizeof(PS1AtlasClut) == 20, "atlas clut record");

#define PS1_ATLAS_MAX_SHEETS   32
#define PS1_ATLAS_MAX_CLUSTERS 768
#define PS1_ATLAS_MAX_CLUTS    64 // Sonic 2: 19 at most (docs/30 3.1); tools/atlas/build_atlas.py MAX_CLUTS checks it

extern PS1AtlasSheet   g_ps1AtlasSheets[PS1_ATLAS_MAX_SHEETS];
extern PS1AtlasCluster g_ps1AtlasClusters[PS1_ATLAS_MAX_CLUSTERS];
extern PS1AtlasClut    g_ps1AtlasCluts[PS1_ATLAS_MAX_CLUTS];
extern int             g_ps1AtlasSheetCount, g_ps1AtlasClusterCount, g_ps1AtlasClutCount;
extern uint16_t        g_ps1AtlasFmvX, g_ps1AtlasFmvY; // 15-bit video page, 0xFFFF = none

// Loads Data/Sprites/Atlas/<folder>.atl (player 1 = Tails: <folder>_t.atl): tables into RAM, pixel
// blocks into VRAM (immediate uploads: call outside the chain, e.g. during a stage load).
bool PS1AtlasLoad(const char *folder, int player);
// Sheet index for a sheet path ("Data/Sprites/X/Y.gif" or "X/Y.gif", any case), -1 = none.
int PS1AtlasFindSheet(const char *path);

// On-demand pictures (<folder>.pic, tools/atlas/build_atlas.py): frames too big to keep resident
// (the Secrets gallery) are loaded into one of a few VRAM slots when a draw first needs them.
// PS1AtlasPictureReady: true if the cluster's texels are in VRAM now; otherwise the picture is
// queued (drawn from the next frame on) and false returned. PS1AtlasServicePictures: loads the
// queued pictures (least recently used slot replaced); call at a frame start, before anything is
// chained (it releases the chain arenas for the CD read, as a stage load).
bool PS1AtlasPictureReady(const PS1AtlasCluster *c);
void PS1AtlasServicePictures();

// Verification counters (GDB).
extern volatile uint32_t g_ps1PictureLoads;     // on-demand pictures loaded
extern volatile uint32_t g_ps1PicturePending;   // draws skipped while their picture was queued
extern volatile int32_t  g_ps1PictureSlot[4];   // picture in each slot (-1 = none)
extern volatile uint32_t g_ps1AtlasLoads;       // atlases loaded
extern volatile uint32_t g_ps1AtlasBlockBytes;  // bytes uploaded to VRAM by the last load
extern volatile uint32_t g_ps1AtlasMissing;     // sheet lookups that found nothing
