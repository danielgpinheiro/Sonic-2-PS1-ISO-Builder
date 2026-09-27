#ifndef SCENE_H
#define SCENE_H

#define LAYER_COUNT    (9)
#define DEFORM_STORE   (256)
#define DEFORM_SIZE    (320)
#define DEFORM_COUNT   (DEFORM_STORE + DEFORM_SIZE)
#define PARALLAX_COUNT (0x100)

#define TILE_COUNT    (0x400)
#define TILE_SIZE     (0x10)
#define CHUNK_SIZE    (0x80)
#define TILE_DATASIZE (TILE_SIZE * TILE_SIZE)
#define TILESET_SIZE  (TILE_COUNT * TILE_DATASIZE)

#define TILELAYER_CHUNK_W          (0x100) // the row stride (y << 8) is hard-coded in the engine: keep it
#if RETRO_PLATFORM == RETRO_PS1
// PS1 (2 MB, docs/30 phase 2): Sonic 2's background layers are <= 16 chunks tall (without Egg Gauntlet) and
// the FG (layer 0) <= 133 (Special; story zones <= 32, Credits 52). TileLayer::tiles is a pointer on PS1, layer 0
// into a taller buffer (Scene.cpp). Rows past a buffer are read into a scratch row and counted
// (g_ps1LayerRowsClamped). Line scroll: the BG layers' ysize * 128 lines; the FG's is only cleared.
#define TILELAYER_CHUNK_H      (0x10)
#define PS1_LAYER0_CHUNK_H     (0x88)
#else
#define TILELAYER_CHUNK_H          (0x100)
#endif
#define TILELAYER_CHUNK_COUNT      (TILELAYER_CHUNK_W * TILELAYER_CHUNK_H)
#define TILELAYER_LINESCROLL_COUNT (TILELAYER_CHUNK_H * CHUNK_SIZE)

#define CHUNKTILE_COUNT (0x200 * (8 * 8))

#define CPATH_COUNT (2)

enum StageListNames {
    STAGELIST_PRESENTATION,
    STAGELIST_REGULAR,
    STAGELIST_BONUS,
    STAGELIST_SPECIAL,
    STAGELIST_MAX, // StageList size
};

enum TileLayerTypes {
    LAYER_NOSCROLL,
    LAYER_HSCROLL,
    LAYER_VSCROLL,
    LAYER_3DFLOOR,
    LAYER_3DSKY,
};

enum StageModes {
    STAGEMODE_LOAD,
    STAGEMODE_NORMAL,
    STAGEMODE_PAUSED,
    STAGEMODE_FROZEN,

#if !RETRO_REV00
    STAGEMODE_2P,
#endif

    STAGEMODE_NORMAL_STEP,
    STAGEMODE_PAUSED_STEP,
    STAGEMODE_FROZEN_STEP,

#if !RETRO_REV00
    STAGEMODE_2P_STEP,
#endif
};

enum TileInfo {
    TILEINFO_INDEX,
    TILEINFO_DIRECTION,
    TILEINFO_VISUALPLANE,
    TILEINFO_SOLIDITYA,
    TILEINFO_SOLIDITYB,
    TILEINFO_FLAGSA,
    TILEINFO_ANGLEA,
    TILEINFO_FLAGSB,
    TILEINFO_ANGLEB,
};

enum DeformationModes {
    DEFORM_FG,
    DEFORM_FG_WATER,
    DEFORM_BG,
    DEFORM_BG_WATER,
};

enum CameraStyles {
    CAMERASTYLE_FOLLOW,
    CAMERASTYLE_EXTENDED,
    CAMERASTYLE_EXTENDED_OFFSET_L,
    CAMERASTYLE_EXTENDED_OFFSET_R,
    CAMERASTYLE_HLOCKED,
};

#if RETRO_PLATFORM == RETRO_PS1
// PS1: sized from Sonic 2's GameConfig (lists of 6 / 22 / 8 / 12 stages; folder <= 8, id <= 1, name <= 21
// chars): 12 KB instead of 197 KB. LoadGameConfig clamps (g_ps1StageListClamped).
#define STAGELIST_ENTRY_COUNT (0x40)
struct SceneInfo {
    char name[0x20];
    char folder[0x10];
    char id[0x10];
    bool highlighted;
};
#else
#define STAGELIST_ENTRY_COUNT (0x100)
struct SceneInfo {
    char name[0x40];
    char folder[0x40];
    char id[0x40];
    bool highlighted;
};
#endif

struct CollisionMasks {
    sbyte floorMasks[TILE_COUNT * TILE_SIZE];
    sbyte lWallMasks[TILE_COUNT * TILE_SIZE];
    sbyte rWallMasks[TILE_COUNT * TILE_SIZE];
    sbyte roofMasks[TILE_COUNT * TILE_SIZE];
    uint angles[TILE_COUNT];
    byte flags[TILE_COUNT];
};

struct TileLayer {
#if RETRO_PLATFORM == RETRO_PS1
    ushort *tiles; // TILELAYER_CHUNK_W x (layer 0: PS1_LAYER0_CHUNK_H, others TILELAYER_CHUNK_H) rows, Scene.cpp
#else
    ushort tiles[TILELAYER_CHUNK_COUNT];
#endif
    byte lineScroll[TILELAYER_LINESCROLL_COUNT];
    int parallaxFactor;
    int scrollSpeed;
    int scrollPos;
    int angle;
    int xpos;
    int ypos;
    int zpos;
    int deformationOffset;
    int deformationOffsetW;
    byte type;
    byte xsize;
    byte ysize;
};

struct LineScroll {
    int parallaxFactor[PARALLAX_COUNT];
    int scrollSpeed[PARALLAX_COUNT];
    int scrollPos[PARALLAX_COUNT];
    int linePos[PARALLAX_COUNT];
    int deform[PARALLAX_COUNT];
    byte entryCount;
};

#if RETRO_PLATFORM == RETRO_PS1
// PS1 (docs/30 phase 2): a chunk tile's index (0-1023), direction (0-3) and visual plane (0-1) share one ushort (bits
// 0-9, 10-11, 12-15) and its two collision flags (0-4 in Sonic 2's data) are the nibbles of one byte: three bytes per
// tile instead of six (saves 96 KB). The engine's `tiles128x128.tileIndex[i]`, `.direction[i]`, `.visualPlane[i]` and
// `.collisionFlags[path][i]` read and assign through these views unchanged; a script value past a field is clamped
// and counted (g_ps1TileAttrClamped, Scene.cpp).
extern volatile uint32_t g_ps1TileAttrClamped;
struct PS1Nibble {
    byte *b;
    int shift;
    inline operator byte() const { return (*b >> shift) & 0xF; }
    inline PS1Nibble &operator=(int v)
    {
        if ((uint)v > 0xF) {
            g_ps1TileAttrClamped = g_ps1TileAttrClamped + 1;
            v &= 0xF;
        }
        *b = (byte)((*b & ~(0xF << shift)) | (v << shift));
        return *this;
    }
};
struct PS1NibbleView {
    byte *base;
    int shift;
    inline PS1Nibble operator[](int i) const { return PS1Nibble{ &base[i], shift }; }
};
struct PS1Field16 {
    ushort *w;
    int shift, mask;
    inline operator ushort() const { return (*w >> shift) & mask; }
    inline PS1Field16 &operator=(int v)
    {
        if ((uint)v > (uint)mask) {
            g_ps1TileAttrClamped = g_ps1TileAttrClamped + 1;
            v &= mask;
        }
        *w = (ushort)((*w & ~(mask << shift)) | (v << shift));
        return *this;
    }
};
struct PS1Field16View {
    ushort *base;
    int shift, mask;
    inline PS1Field16 operator[](int i) const { return PS1Field16{ &base[i], shift, mask }; }
};
struct Tiles128x128 {
    ushort ps1Tile[CHUNKTILE_COUNT];    // bits 0-9: tileIndex, 10-11: direction, 12-15: visualPlane
    byte ps1Collision[CHUNKTILE_COUNT]; // low nibble: collisionFlags[0], high: collisionFlags[1]
    PS1Field16View tileIndex{ ps1Tile, 0, 0x3FF };
    PS1Field16View direction{ ps1Tile, 10, 0x3 };
    PS1Field16View visualPlane{ ps1Tile, 12, 0xF };
    PS1NibbleView collisionFlags[CPATH_COUNT] = { { ps1Collision, 0 }, { ps1Collision, 4 } };
};
#else
struct Tiles128x128 {
    int gfxDataPos[CHUNKTILE_COUNT];
    ushort tileIndex[CHUNKTILE_COUNT];
    byte direction[CHUNKTILE_COUNT];
    byte visualPlane[CHUNKTILE_COUNT];
    byte collisionFlags[CPATH_COUNT][CHUNKTILE_COUNT];
};
#endif

extern int stageListCount[STAGELIST_MAX];
extern char stageListNames[STAGELIST_MAX][0x20];
extern SceneInfo stageList[STAGELIST_MAX][STAGELIST_ENTRY_COUNT];

extern int stageMode;

extern int cameraTarget;
extern int cameraStyle;
extern int cameraEnabled;
extern int cameraAdjustY;
extern int xScrollOffset;
extern int yScrollOffset;
extern int cameraXPos;
extern int cameraYPos;
extern int cameraShift;
extern int cameraLockedY;
extern int cameraShakeX;
extern int cameraShakeY;
extern int cameraLag;
extern int cameraLagStyle;

extern int curXBoundary1;
extern int newXBoundary1;
extern int curYBoundary1;
extern int newYBoundary1;
extern int curXBoundary2;
extern int curYBoundary2;
extern int waterLevel;
extern int waterDrawPos;
extern int newXBoundary2;
extern int newYBoundary2;

extern int SCREEN_SCROLL_LEFT;
extern int SCREEN_SCROLL_RIGHT;
#define SCREEN_SCROLL_UP   ((SCREEN_YSIZE / 2) - 16)
#define SCREEN_SCROLL_DOWN ((SCREEN_YSIZE / 2) + 16)

extern int lastXSize;
extern int lastYSize;

extern bool pauseEnabled;
extern bool timeEnabled;
extern bool debugMode;
extern int frameCounter;
extern int stageMilliseconds;
extern int stageSeconds;
extern int stageMinutes;

// Category and Scene IDs
extern int activeStageList;
extern int stageListPosition;
extern char currentStageFolder[0x100];
extern int actID;

extern char titleCardText[0x100];
extern byte titleCardWord2;

extern byte activeTileLayers[4];
extern byte tLayerMidPoint;
extern TileLayer stageLayouts[LAYER_COUNT];

extern int bgDeformationData0[DEFORM_COUNT];
extern int bgDeformationData1[DEFORM_COUNT];
extern int bgDeformationData2[DEFORM_COUNT];
extern int bgDeformationData3[DEFORM_COUNT];

extern LineScroll hParallax;
extern LineScroll vParallax;

extern Tiles128x128 tiles128x128;
extern CollisionMasks collisionMasks[2];

extern byte tilesetGFXData[TILESET_SIZE];

extern ushort tile3DFloorBuffer[0x100 * 0x100];
extern bool drawStageGFXHQ;

void InitFirstStage();
void InitStartingStage(int list, int stage, int player);
void ProcessStage();

void ProcessParallaxAutoScroll();

void ResetBackgroundSettings();
inline void ResetCurrentStageFolder() { strcpy(currentStageFolder, ""); }
inline bool CheckCurrentStageFolder(int stage)
{
    if (strcmp(currentStageFolder, stageList[activeStageList][stage].folder) == 0) {
        return true;
    }
    else {
        strcpy(currentStageFolder, stageList[activeStageList][stage].folder);
        return false;
    }
}

void LoadStageFiles();
int LoadActFile(const char *ext, int stageID, FileInfo *info);
int LoadStageFile(const char *filePath, int stageID, FileInfo *info);

void LoadActLayout();
void LoadStageBackground();
void LoadStageChunks();
void LoadStageCollisions();
void LoadStageGIFFile(int stageID);

#if RETRO_PLATFORM == RETRO_PS1
// PS1: no software 3D floor (v4 Sonic 2 has no 3D floor layer) and no tile pixels in RAM (VRAM, phase 3).
extern volatile uint32_t g_ps1Copy16x16Tile; // Scene.cpp: calls (GDB)
void PS1Copy16x16Tile(int dest, int src);     // ps1/render.cpp: VRAM-to-VRAM copy inside the tile pages
inline void Init3DFloorBuffer(int layerID) {}
inline void Copy16x16Tile(ushort dest, ushort src)
{
    g_ps1Copy16x16Tile = g_ps1Copy16x16Tile + 1;
    PS1Copy16x16Tile(dest, src);
}
#else
inline void Init3DFloorBuffer(int layerID)
{
    for (int y = 0; y < TILELAYER_CHUNK_H; ++y) {
        for (int x = 0; x < TILELAYER_CHUNK_W; ++x) {
            int c                           = stageLayouts[layerID].tiles[(x >> 3) + (y >> 3 << 8)] << 6;
            int tx                          = x & 7;
            tile3DFloorBuffer[x + (y << 8)] = c + tx + ((y & 7) << 3);
        }
    }
}

inline void Copy16x16Tile(ushort dest, ushort src)
{
    byte *destPtr = &tilesetGFXData[TILELAYER_CHUNK_W * dest];
    byte *srcPtr  = &tilesetGFXData[TILELAYER_CHUNK_W * src];
    int cnt       = TILE_DATASIZE;
    while (cnt--) *destPtr++ = *srcPtr++;
}
#endif

void SetLayerDeformation(int selectedDef, int waveLength, int waveWidth, int waveType, int YPos, int waveSize);

void SetPlayerScreenPosition(Entity *target);
void SetPlayerScreenPositionCDStyle(Entity *target);
void SetPlayerHLockedScreenPosition(Entity *target);
void SetPlayerLockedScreenPosition(Entity *target);
void SetPlayerScreenPositionFixed(Entity *target);

#endif // !SCENE_H
