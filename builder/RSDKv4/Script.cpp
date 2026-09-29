#include "RetroEngine.hpp"
#include <cmath>
#if RETRO_PLATFORM == RETRO_PS1 && PS1_GAME != 2 && !defined(RETRO_PS1_HOST_TOOL)
// Sonic 2's natives are compiled only into its own build (their dispatch cases below): here they are unused statics
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

#if RETRO_PLATFORM == RETRO_PS1
#include "../ps1/video.hh"
void PS1TileChanged(int chunkTile); // ps1/render.cpp

// Guards for the PS1-sized arrays and the VM (for GDB; tools/*_test.sh read them).
volatile uint32_t g_ps1ScriptOverflow  = 0; // LoadBytecode: code / jump table / big values past the PS1-sized arrays
volatile uint32_t g_ps1ScriptDivZero   = 0; // Div / Mod / Rand by 0 (result 0 instead of a trap)
volatile uint32_t g_ps1ScriptBadOpcode = 0; // opcode out of range: sub stopped
volatile uint32_t g_ps1ScriptBadPtr    = 0; // sub/function pointers past the PS1 arrays (not the PC "no sub" sentinels)
volatile uint32_t g_ps1ScriptBigCount  = 0; // big (non-int16) script values of the loaded code
uint32_t g_ps1VmOps = 0; // VM instructions executed (profile: Object.cpp per type)
volatile uint32_t g_ps1Scene3DOverflow = 0; // Scene3D index / count past the PS1-sized buffers (clamped)
volatile int32_t g_ps1Scene3DMaxVertex = 0; // largest vertex / face index the scripts used
volatile int32_t g_ps1Scene3DMaxFace   = 0;
volatile uint32_t g_ps1Prof3DTransformHbl = 0, g_ps1Prof3DSortHbl = 0, g_ps1Prof3DDrawHbl = 0; // last Draw3DScene

// Script code as int16 slots + a side table of the values that don't fit (Script.hpp). The table is sorted by
// code position: LoadBytecode stores in ascending order, and a reload at a lower position (ClearScriptData, a
// new stage after GlobalCode) first drops the entries at or past it. The scripts also write into the code
// area at runtime (object-type locals, SetTableValue tables: <= ~1,030 slots per stage): a slot that must hold a
// big value is inserted once (sorted) and stays in the table, later writes update it in place.
// Lookups go through a bucket index: s_ps1BigBucket[b] = the first entry at or past code slot b * 128 (Sonic 2 has
// ~1 big value per 31 slots, so a lookup is ~2 compares instead of a 12-step binary search: the oscillation
// table update alone went from ~107 hblanks a frame). The index is rebuilt lazily after the table changes.
// A terminator entry (pos INT_MAX) past the last one ends every scan.
struct PS1ScriptBig {
    int pos;
    int value;
};
#define PS1_BIGBUCKET_SHIFT (7)
#define PS1_BIGBUCKET_COUNT ((SCRIPTCODE_COUNT >> PS1_BIGBUCKET_SHIFT) + 1)
static PS1ScriptBig s_ps1ScriptBig[PS1_SCRIPTBIG_COUNT + 1];
static int s_ps1ScriptBigCount = 0;
static ushort s_ps1BigBucket[PS1_BIGBUCKET_COUNT];
static bool s_ps1BigBucketDirty = true;
static void PS1ScriptBigChanged()
{
    s_ps1ScriptBig[s_ps1ScriptBigCount].pos = 0x7FFFFFFF;
    s_ps1BigBucketDirty                     = true;
    g_ps1ScriptBigCount                     = s_ps1ScriptBigCount;
}
static void PS1ScriptBigIndex()
{
    int k = 0;
    for (int b = 0; b < PS1_BIGBUCKET_COUNT; ++b) {
        int start = b << PS1_BIGBUCKET_SHIFT;
        while (s_ps1ScriptBig[k].pos < start) ++k; // the terminator stops it
        s_ps1BigBucket[b] = k;
    }
    s_ps1BigBucketDirty = false;
}
static inline int PS1ScriptBigFind(int pos) // index of the first entry with .pos >= pos
{
    if (s_ps1BigBucketDirty)
        PS1ScriptBigIndex();
    int k = s_ps1BigBucket[pos >> PS1_BIGBUCKET_SHIFT];
    while (s_ps1ScriptBig[k].pos < pos) ++k;
    return k;
}
int PS1ScriptBigValue(int pos)
{
    int k = PS1ScriptBigFind(pos);
    return s_ps1ScriptBig[k].pos == pos ? s_ps1ScriptBig[k].value : PS1_SCRIPT_BIG; // not stored: the literal value
}
__attribute__((noinline)) void PS1ScriptWrite(int pos, int value)
{
    if ((uint)pos >= SCRIPTCODE_COUNT)
        return;
    if (scriptCode.slot[pos] != PS1_SCRIPT_BIG) {
        if (value > PS1_SCRIPT_BIG && value <= 0x7FFF) {
            scriptCode.slot[pos] = (short)value;
            return;
        }
        if (s_ps1ScriptBigCount == PS1_SCRIPTBIG_COUNT) {
            g_ps1ScriptOverflow = g_ps1ScriptOverflow + 1;
            return;
        }
        int k = PS1ScriptBigFind(pos);
        memmove(&s_ps1ScriptBig[k + 1], &s_ps1ScriptBig[k], (s_ps1ScriptBigCount - k) * sizeof(PS1ScriptBig));
        s_ps1ScriptBig[k].pos   = pos;
        s_ps1ScriptBig[k].value = value;
        ++s_ps1ScriptBigCount;
        PS1ScriptBigChanged();
        scriptCode.slot[pos] = PS1_SCRIPT_BIG;
        return;
    }
    int k = PS1ScriptBigFind(pos);
    if (s_ps1ScriptBig[k].pos == pos)
        s_ps1ScriptBig[k].value = value;
}
static bool PS1ScriptStore(int pos, int value)
{
    if (s_ps1ScriptBigCount && s_ps1ScriptBig[s_ps1ScriptBigCount - 1].pos >= pos) {
        while (s_ps1ScriptBigCount && s_ps1ScriptBig[s_ps1ScriptBigCount - 1].pos >= pos) --s_ps1ScriptBigCount;
        PS1ScriptBigChanged();
    }
    if (value > PS1_SCRIPT_BIG && value <= 0x7FFF) {
        scriptCode.slot[pos] = (short)value;
        return true;
    }
    if (s_ps1ScriptBigCount == PS1_SCRIPTBIG_COUNT)
        return false;
    scriptCode.slot[pos]                         = PS1_SCRIPT_BIG;
    s_ps1ScriptBig[s_ps1ScriptBigCount].pos      = pos;
    s_ps1ScriptBig[s_ps1ScriptBigCount++].value = value;
    PS1ScriptBigChanged();
    return true;
}

// The bytecode marks unused subs/functions with PC's last array slots (0x3FFFF / 0x3FFF): ~0.7 MB past the
// PS1-sized arrays. Point them at the PS1 arrays' last slots (kept 0) instead.
#define PS1_PC_SCRIPTCODE_END (0x40000 - 1)
#define PS1_PC_JUMPTABLE_END  (0x4000 - 1)
static void PS1FixScriptPtr(ScriptPtr *p)
{
    if (p->scriptCodePtr == PS1_PC_SCRIPTCODE_END)
        p->scriptCodePtr = SCRIPTCODE_COUNT - 1;
    else if ((uint)p->scriptCodePtr >= SCRIPTCODE_COUNT - 1) {
        g_ps1ScriptBadPtr = g_ps1ScriptBadPtr + 1;
        p->scriptCodePtr  = SCRIPTCODE_COUNT - 1;
    }
    if (p->jumpTablePtr == PS1_PC_JUMPTABLE_END)
        p->jumpTablePtr = JUMPTABLE_COUNT - 1;
    else if ((uint)p->jumpTablePtr >= JUMPTABLE_COUNT - 1) {
        g_ps1ScriptBadPtr = g_ps1ScriptBadPtr + 1;
        p->jumpTablePtr   = JUMPTABLE_COUNT - 1;
    }
}

volatile int32_t g_ps1Scene3DMaxVertexLow = 0; // largest vertex index below 0xF00 (the half-pipe uses 4094/4095 as scratch)
volatile int32_t g_ps1Scene3DMaxCountV = 0, g_ps1Scene3DMaxCountF = 0; // largest counts the scripts set
static inline int PS1Scene3DIndex(int i, int size, volatile int32_t *max)
{
    if (i > *max)
        *max = i;
    if (max == &g_ps1Scene3DMaxVertex && i < 0xF00 && i > g_ps1Scene3DMaxVertexLow)
        g_ps1Scene3DMaxVertexLow = i;
    if ((uint)i < (uint)size)
        return i;
    g_ps1Scene3DOverflow = g_ps1Scene3DOverflow + 1;
    return (i < 0 || size <= 0) ? 0 : size - 1; // size 0: no Scene3D block, the 1-entry dummy
}
// Vertex indices 0x1000 - PS1_VTX_SCRATCH .. 0x1000 (upstream's last slots, used as scratch) -> the buffer's tail.
static inline int PS1VtxMap(int i)
{
    return (i >= 0x1000 - PS1_VTX_SCRATCH && i <= 0x1000) ? i - 0x1000 + g_ps1VtxCap : i;
}
#define PS1_VTX(i)  PS1Scene3DIndex(PS1VtxMap(i), g_ps1VtxCap, &g_ps1Scene3DMaxVertex)
#define PS1_FACE(i) PS1Scene3DIndex(i, g_ps1FaceCap, &g_ps1Scene3DMaxFace)
static inline int PS1Scene3DCount(int n, int size, volatile int32_t *max)
{
    if (n <= 0)
        return n;
    return PS1Scene3DIndex(n - 1, size, max) + 1;
}
#define PS1_VTX_COUNT(n)  PS1Scene3DCount(PS1VtxMap(n), g_ps1VtxCap, &g_ps1Scene3DMaxVertex)
#define PS1_FACE_COUNT(n) PS1Scene3DCount(n, g_ps1FaceCap, &g_ps1Scene3DMaxFace)
// Entity slots from the scripts (object[x], ResetObjectEntity...): past the PS1-sized objectEntityList (Object.hpp)
// they get a zeroed scratch entity (what upstream's blank slots there hold), counted.
volatile uint32_t g_ps1EntityOutOfRange = 0;
volatile int32_t g_ps1EntityOORIndex = 0, g_ps1EntityOORMax = 0, g_ps1EntityOORFrom = 0, g_ps1EntityOORType = 0; // last one
__attribute__((noinline)) static int PS1ObjScratch(int i)
{
    g_ps1EntityOutOfRange = g_ps1EntityOutOfRange + 1;
    g_ps1EntityOORIndex   = i;
    if (i > g_ps1EntityOORMax)
        g_ps1EntityOORMax = i;
    g_ps1EntityOORFrom = objectEntityPos;
    g_ps1EntityOORType = objectEntityList[objectEntityPos].type;
    memset(&objectEntityList[ENTITY_COUNT], 0, sizeof(Entity));
    return ENTITY_COUNT;
}
#define PS1_OBJ(i) objectEntityList[(uint)(i) < ENTITY_COUNT ? (i) : PS1ObjScratch(i)]
#else
#define PS1_VTX(i)        (i)
#define PS1_FACE(i)       (i)
#define PS1_VTX_COUNT(n)  (n)
#define PS1_FACE_COUNT(n) (n)
#define PS1_OBJ(i)        objectEntityList[i]
#endif

#if RETRO_USE_COMPILER
#if !RETRO_REV00
#define COMMON_SCRIPT_VAR_COUNT (34)
#else
#define COMMON_SCRIPT_VAR_COUNT (33)
#endif
#endif

#define SCRIPT_VAR_COUNT (COMMON_SCRIPT_VAR_COUNT + 0x1DF)
int lineID = 0;

enum ScriptVarType { VAR_ALIAS, VAR_STATICVALUE, VAR_TABLE };
enum ScriptVarAccessModifier { ACCESS_NONE, ACCESS_PUBLIC, ACCESS_PRIVATE };

struct ScriptVariableInfo {
    ScriptVariableInfo()
    {
        type   = VAR_ALIAS;
        access = ACCESS_NONE;
        StrCopy(name, "");
        StrCopy(value, "");
    }

    ScriptVariableInfo(byte type, byte access, const char *name, const char *value)
    {
        this->type   = type;
        this->access = access;
        StrCopy(this->name, name);
        StrCopy(this->value, value);
    }

    byte type;
    byte access;
    char name[0x20];
    char value[0x20];
};

struct FunctionInfo {
    FunctionInfo()
    {
        StrCopy(name, "");
        opcodeSize = 0;
    }
    FunctionInfo(const char *functionName, int opSize)
    {
        StrCopy(name, functionName);
        opcodeSize = opSize;
    }
    
#if RETRO_REV03
    char name[0x30];
#else
    char name[0x20];
#endif
    int opcodeSize;
};

#if RETRO_USE_COMPILER
const char variableNames[][0x20] = {
    // Internal Script Values
    "temp0",
    "temp1",
    "temp2",
    "temp3",
    "temp4",
    "temp5",
    "temp6",
    "temp7",
    "checkResult",
    "arrayPos0",
    "arrayPos1",
    "arrayPos2",
    "arrayPos3",
    "arrayPos4",
    "arrayPos5",
    "arrayPos6",
    "arrayPos7",
    "global",
    "local",

    // Object Properties
    "object.entityPos",
    "object.groupID",
    "object.type",
    "object.propertyValue",
    "object.xpos",
    "object.ypos",
    "object.ixpos",
    "object.iypos",
    "object.xvel",
    "object.yvel",
    "object.speed",
    "object.state",
    "object.rotation",
    "object.scale",
    "object.priority",
    "object.drawOrder",
    "object.direction",
    "object.inkEffect",
    "object.alpha",
    "object.frame",
    "object.animation",
    "object.prevAnimation",
    "object.animationSpeed",
    "object.animationTimer",
    "object.angle",
    "object.lookPosX",
    "object.lookPosY",
    "object.collisionMode",
    "object.collisionPlane",
    "object.controlMode",
    "object.controlLock",
    "object.pushing",
    "object.visible",
    "object.tileCollisions",
    "object.interaction",
    "object.gravity",
    "object.up",
    "object.down",
    "object.left",
    "object.right",
    "object.jumpPress",
    "object.jumpHold",
    "object.scrollTracking",
    "object.floorSensorL",
    "object.floorSensorC",
    "object.floorSensorR",
#if !RETRO_REV00
    "object.floorSensorLC",
    "object.floorSensorRC",
#endif
    "object.collisionLeft",
    "object.collisionTop",
    "object.collisionRight",
    "object.collisionBottom",
    "object.outOfBounds",
    "object.spriteSheet",

    // Object Values
    "object.value0",
    "object.value1",
    "object.value2",
    "object.value3",
    "object.value4",
    "object.value5",
    "object.value6",
    "object.value7",
    "object.value8",
    "object.value9",
    "object.value10",
    "object.value11",
    "object.value12",
    "object.value13",
    "object.value14",
    "object.value15",
    "object.value16",
    "object.value17",
    "object.value18",
    "object.value19",
    "object.value20",
    "object.value21",
    "object.value22",
    "object.value23",
    "object.value24",
    "object.value25",
    "object.value26",
    "object.value27",
    "object.value28",
    "object.value29",
    "object.value30",
    "object.value31",
    "object.value32",
    "object.value33",
    "object.value34",
    "object.value35",
    "object.value36",
    "object.value37",
    "object.value38",
    "object.value39",
    "object.value40",
    "object.value41",
    "object.value42",
    "object.value43",
    "object.value44",
    "object.value45",
    "object.value46",
    "object.value47",

    // Stage Properties
    "stage.state",
    "stage.activeList",
    "stage.listPos",
    "stage.timeEnabled",
    "stage.milliSeconds",
    "stage.seconds",
    "stage.minutes",
    "stage.actNum",
    "stage.pauseEnabled",
    "stage.listSize",
    "stage.newXBoundary1",
    "stage.newXBoundary2",
    "stage.newYBoundary1",
    "stage.newYBoundary2",
    "stage.curXBoundary1",
    "stage.curXBoundary2",
    "stage.curYBoundary1",
    "stage.curYBoundary2",
    "stage.deformationData0",
    "stage.deformationData1",
    "stage.deformationData2",
    "stage.deformationData3",
    "stage.waterLevel",
    "stage.activeLayer",
    "stage.midPoint",
    "stage.playerListPos",
    "stage.debugMode",
    "stage.entityPos",

    // Screen Properties
    "screen.cameraEnabled",
    "screen.cameraTarget",
    "screen.cameraStyle",
    "screen.cameraX",
    "screen.cameraY",
    "screen.drawListSize",
    "screen.xcenter",
    "screen.ycenter",
    "screen.xsize",
    "screen.ysize",
    "screen.xoffset",
    "screen.yoffset",
    "screen.shakeX",
    "screen.shakeY",
    "screen.adjustCameraY",

    "touchscreen.down",
    "touchscreen.xpos",
    "touchscreen.ypos",

    // Sound Properties
    "music.volume",
    "music.currentTrack",
    "music.position",

    // Input Properties
    "keyDown.up",
    "keyDown.down",
    "keyDown.left",
    "keyDown.right",
    "keyDown.buttonA",
    "keyDown.buttonB",
    "keyDown.buttonC",
    "keyDown.buttonX",
    "keyDown.buttonY",
    "keyDown.buttonZ",
    "keyDown.buttonL",
    "keyDown.buttonR",
    "keyDown.start",
    "keyDown.select",
    "keyPress.up",
    "keyPress.down",
    "keyPress.left",
    "keyPress.right",
    "keyPress.buttonA",
    "keyPress.buttonB",
    "keyPress.buttonC",
    "keyPress.buttonX",
    "keyPress.buttonY",
    "keyPress.buttonZ",
    "keyPress.buttonL",
    "keyPress.buttonR",
    "keyPress.start",
    "keyPress.select",

    // Menu Properties
    "menu1.selection",
    "menu2.selection",

    // Tile Layer Properties
    "tileLayer.xsize",
    "tileLayer.ysize",
    "tileLayer.type",
    "tileLayer.angle",
    "tileLayer.xpos",
    "tileLayer.ypos",
    "tileLayer.zpos",
    "tileLayer.parallaxFactor",
    "tileLayer.scrollSpeed",
    "tileLayer.scrollPos",
    "tileLayer.deformationOffset",
    "tileLayer.deformationOffsetW",
    "hParallax.parallaxFactor",
    "hParallax.scrollSpeed",
    "hParallax.scrollPos",
    "vParallax.parallaxFactor",
    "vParallax.scrollSpeed",
    "vParallax.scrollPos",

    // 3D Scene Properties
    "scene3D.vertexCount",
    "scene3D.faceCount",
    "scene3D.projectionX",
    "scene3D.projectionY",
#if !RETRO_REV00
    "scene3D.fogColor",
    "scene3D.fogStrength",
#endif

    "vertexBuffer.x",
    "vertexBuffer.y",
    "vertexBuffer.z",
    "vertexBuffer.u",
    "vertexBuffer.v",

    "faceBuffer.a",
    "faceBuffer.b",
    "faceBuffer.c",
    "faceBuffer.d",
    "faceBuffer.flag",
    "faceBuffer.color",

    "saveRAM",
    "engine.state",
#if RETRO_REV00
    "engine.message",
#endif
    "engine.language",
    "engine.onlineActive",
    "engine.sfxVolume",
    "engine.bgmVolume",
#if RETRO_REV00
    "engine.platformID",
#endif
    "engine.trialMode",
#if !RETRO_REV00
    "engine.deviceType",
#endif

// Extras
#if RETRO_REV03
    "screen.currentID",
    "camera.enabled",
    "camera.target",
    "camera.style",
    "camera.xpos",
    "camera.ypos",
    "camera.adjustY",
#endif

// Haptics
#if RETRO_USE_HAPTICS
    "engine.hapticsEnabled",
#endif
};
#endif

const FunctionInfo functions[] = {
    FunctionInfo("End", 0),      // End of Script
    FunctionInfo("Equal", 2),    // Equal
    FunctionInfo("Add", 2),      // Add
    FunctionInfo("Sub", 2),      // Subtract
    FunctionInfo("Inc", 1),      // Increment
    FunctionInfo("Dec", 1),      // Decrement
    FunctionInfo("Mul", 2),      // Multiply
    FunctionInfo("Div", 2),      // Divide
    FunctionInfo("ShR", 2),      // Bit Shift Right
    FunctionInfo("ShL", 2),      // Bit Shift Left
    FunctionInfo("And", 2),      // Bitwise And
    FunctionInfo("Or", 2),       // Bitwise Or
    FunctionInfo("Xor", 2),      // Bitwise Xor
    FunctionInfo("Mod", 2),      // Mod
    FunctionInfo("FlipSign", 1), // Flips the Sign of the value

    FunctionInfo("CheckEqual", 2),    // compare a=b, return result in CheckResult Variable
    FunctionInfo("CheckGreater", 2),  // compare a>b, return result in CheckResult Variable
    FunctionInfo("CheckLower", 2),    // compare a<b, return result in CheckResult Variable
    FunctionInfo("CheckNotEqual", 2), // compare a!=b, return result in CheckResult Variable

    FunctionInfo("IfEqual", 3),          // compare a=b, jump if condition met
    FunctionInfo("IfGreater", 3),        // compare a>b, jump if condition met
    FunctionInfo("IfGreaterOrEqual", 3), // compare a>=b, jump if condition met
    FunctionInfo("IfLower", 3),          // compare a<b, jump if condition met
    FunctionInfo("IfLowerOrEqual", 3),   // compare a<=b, jump if condition met
    FunctionInfo("IfNotEqual", 3),       // compare a!=b, jump if condition met
    FunctionInfo("else", 0),             // The else for an if statement
    FunctionInfo("endif", 0),            // The end if

    FunctionInfo("WEqual", 3),          // compare a=b, loop if condition met
    FunctionInfo("WGreater", 3),        // compare a>b, loop if condition met
    FunctionInfo("WGreaterOrEqual", 3), // compare a>=b, loop if condition met
    FunctionInfo("WLower", 3),          // compare a<b, loop if condition met
    FunctionInfo("WLowerOrEqual", 3),   // compare a<=b, loop if condition met
    FunctionInfo("WNotEqual", 3),       // compare a!=b, loop if condition met
    FunctionInfo("loop", 0),            // While Loop marker

    FunctionInfo("ForEachActive", 3), // foreach loop, iterates through object group lists only if they are active and interaction is true
    FunctionInfo("ForEachAll", 3),    // foreach loop, iterates through objects matching type
    FunctionInfo("next", 0),          // foreach loop, next marker

    FunctionInfo("switch", 2),    // Switch Statement
    FunctionInfo("break", 0),     // break
    FunctionInfo("endswitch", 0), // endswitch

    // Math Functions
    FunctionInfo("Rand", 2),
    FunctionInfo("Sin", 2),
    FunctionInfo("Cos", 2),
    FunctionInfo("Sin256", 2),
    FunctionInfo("Cos256", 2),
    FunctionInfo("ATan2", 3),
    FunctionInfo("Interpolate", 4),
    FunctionInfo("InterpolateXY", 7),

    // Graphics Functions
    FunctionInfo("LoadSpriteSheet", 1),
    FunctionInfo("RemoveSpriteSheet", 1),
    FunctionInfo("DrawSprite", 1),
    FunctionInfo("DrawSpriteXY", 3),
    FunctionInfo("DrawSpriteScreenXY", 3),
    FunctionInfo("DrawTintRect", 4),
    FunctionInfo("DrawNumbers", 7),
    FunctionInfo("DrawActName", 7),
    FunctionInfo("DrawMenu", 3),
    FunctionInfo("SpriteFrame", 6),
    FunctionInfo("EditFrame", 7),
    FunctionInfo("LoadPalette", 5),
    FunctionInfo("RotatePalette", 4),
    FunctionInfo("SetScreenFade", 4),
    FunctionInfo("SetActivePalette", 3),
#if RETRO_REV00
    FunctionInfo("SetPaletteFade", 7),
#else
    FunctionInfo("SetPaletteFade", 6),
#endif
    FunctionInfo("SetPaletteEntry", 3),
    FunctionInfo("GetPaletteEntry", 3),
    FunctionInfo("CopyPalette", 5),
    FunctionInfo("ClearScreen", 1),
    FunctionInfo("DrawSpriteFX", 4),
    FunctionInfo("DrawSpriteScreenFX", 4),

    // More Useful Stuff
    FunctionInfo("LoadAnimation", 1),
    FunctionInfo("SetupMenu", 4),
    FunctionInfo("AddMenuEntry", 3),
    FunctionInfo("EditMenuEntry", 4),
    FunctionInfo("LoadStage", 0),
    FunctionInfo("DrawRect", 8),
    FunctionInfo("ResetObjectEntity", 5),
    FunctionInfo("BoxCollisionTest", 11),
    FunctionInfo("CreateTempObject", 4),

    // Player and Animation Functions
    FunctionInfo("ProcessObjectMovement", 0),
    FunctionInfo("ProcessObjectControl", 0),
    FunctionInfo("ProcessAnimation", 0),
    FunctionInfo("DrawObjectAnimation", 0),

    // Music
    FunctionInfo("SetMusicTrack", 3),
    FunctionInfo("PlayMusic", 1),
    FunctionInfo("StopMusic", 0),
    FunctionInfo("PauseMusic", 0),
    FunctionInfo("ResumeMusic", 0),
    FunctionInfo("SwapMusicTrack", 4),

    // Sound FX
    FunctionInfo("PlaySfx", 2),
    FunctionInfo("StopSfx", 1),
    FunctionInfo("SetSfxAttributes", 3),

    // More Collision Stuff
    FunctionInfo("ObjectTileCollision", 4),
    FunctionInfo("ObjectTileGrip", 4),

    // Bitwise Not
    FunctionInfo("Not", 1),

    // 3D Stuff
    FunctionInfo("Draw3DScene", 0),
    FunctionInfo("SetIdentityMatrix", 1),
    FunctionInfo("MatrixMultiply", 2),
    FunctionInfo("MatrixTranslateXYZ", 4),
    FunctionInfo("MatrixScaleXYZ", 4),
    FunctionInfo("MatrixRotateX", 2),
    FunctionInfo("MatrixRotateY", 2),
    FunctionInfo("MatrixRotateZ", 2),
    FunctionInfo("MatrixRotateXYZ", 4),
#if !RETRO_REV00
    FunctionInfo("MatrixInverse", 1),
#endif
    FunctionInfo("TransformVertices", 3),

    FunctionInfo("CallFunction", 1),
    FunctionInfo("return", 0),

    FunctionInfo("SetLayerDeformation", 6),
    FunctionInfo("CheckTouchRect", 4),
    FunctionInfo("GetTileLayerEntry", 4),
    FunctionInfo("SetTileLayerEntry", 4),

    FunctionInfo("GetBit", 3),
    FunctionInfo("SetBit", 3),

    FunctionInfo("ClearDrawList", 1),
    FunctionInfo("AddDrawListEntityRef", 2),
    FunctionInfo("GetDrawListEntityRef", 3),
    FunctionInfo("SetDrawListEntityRef", 3),

    FunctionInfo("Get16x16TileInfo", 4),
    FunctionInfo("Set16x16TileInfo", 4),
    FunctionInfo("Copy16x16Tile", 2),
    FunctionInfo("GetAnimationByName", 2),
    FunctionInfo("ReadSaveRAM", 0),
    FunctionInfo("WriteSaveRAM", 0),

#if !RETRO_REV02
    FunctionInfo("LoadFontFile", 1),
    FunctionInfo("LoadTextFile", 3),
#else
    FunctionInfo("LoadTextFile", 2),
#endif
    FunctionInfo("GetTextInfo", 5),
#if !RETRO_REV02
    FunctionInfo("DrawText", 7),
#endif
    FunctionInfo("GetVersionNumber", 2),

    FunctionInfo("GetTableValue", 3),
    FunctionInfo("SetTableValue", 3),

    FunctionInfo("CheckCurrentStageFolder", 1),
    FunctionInfo("Abs", 1),

    FunctionInfo("CallNativeFunction", 1),
    FunctionInfo("CallNativeFunction2", 3),
    FunctionInfo("CallNativeFunction4", 5),

    FunctionInfo("SetObjectRange", 1),
#if RETRO_REV02
    FunctionInfo("GetObjectValue", 3),
    FunctionInfo("SetObjectValue", 3),
    FunctionInfo("CopyObject", 3),
#endif
    FunctionInfo("Print", 3),

#if RETRO_REV03
    // Extras
    FunctionInfo("CheckCameraProximity", 4),
    FunctionInfo("SetScreenCount", 1),
    FunctionInfo("SetScreenVertices", 5),
    FunctionInfo("GetInputDeviceID", 2),
    FunctionInfo("GetFilteredInputDeviceID", 4),
    FunctionInfo("GetInputDeviceType", 2),
    FunctionInfo("IsInputDeviceAssigned", 1),
    FunctionInfo("AssignInputSlotToDevice", 2),
    FunctionInfo("IsInputSlotAssigned", 1),
    FunctionInfo("ResetInputSlotAssignments", 0),
#endif
#if RETRO_PLATFORM == RETRO_PS1
    // PS1 extensions, only in bytecode rewritten by tools/scripts/patch_bytecode.py (docs/30): hot script loops run
    // natively, with every side effect of the script (temps, table / local writes).
    FunctionInfo("PS1Oscillate", 4),
    FunctionInfo("PS1Ring", 0),
    FunctionInfo("PS1BridgeDraw", 0),
    FunctionInfo("PS1LoseRing", 0),
    FunctionInfo("PS1HorizontalDoor", 0),
    FunctionInfo("PS1ButtonBridge", 0),
    FunctionInfo("PS1PlaneSwitchV", 0),
    FunctionInfo("PS1PlaneSwitchH", 0),
    FunctionInfo("PS1RotatePlatform", 0),
    FunctionInfo("PS1RotatePlatformDraw", 0),
    FunctionInfo("PS1HPZBridge", 0),
    FunctionInfo("PS1HPZBridgeDraw", 0),
    FunctionInfo("PS1StageSetup", 0),
    FunctionInfo("PS1HUDDraw", 0),
    FunctionInfo("PS1TurretPlatform", 0),
    FunctionInfo("PS1BeltPlatform", 0),
    FunctionInfo("PS1HFlipper", 0),
    FunctionInfo("PS1Earthquake", 0),
    FunctionInfo("PS1Spikes", 0),
    FunctionInfo("PS1PlayerInput", 0),
    FunctionInfo("PS1CLedge", 0),
    FunctionInfo("PS1SteamPiston", 0),
    FunctionInfo("PS1PlayerFn2", 0),
    FunctionInfo("PS1PlayerFn3", 0),
    FunctionInfo("PS1PlayerFn4", 0),
    FunctionInfo("PS1PlayerFn5", 0),
    FunctionInfo("PS1PlayerFn6", 0),
    FunctionInfo("PS1PlayerFn51", 0),
    FunctionInfo("PS1PlayerFn52", 0),
    FunctionInfo("PS1PlayerFn53", 0),
    FunctionInfo("PS1PlayerState10", 0),
    FunctionInfo("PS1PlayerState12", 0),
    FunctionInfo("PS1TailsFn61", 0),
    FunctionInfo("PS1TailsFn62", 0),
    FunctionInfo("PS1TailsFn66", 0),
    FunctionInfo("PS1TailsFn67", 0),
    FunctionInfo("PS1MPZSetup", 0),
    FunctionInfo("PS1Monitor", 0),
    FunctionInfo("PS1InvisibleBlock", 0),
    FunctionInfo("PS1SpecialRing", 0),
    FunctionInfo("PS1Halfpipe", 0),
    FunctionInfo("PS1HalfpipeSegment", 0),
    FunctionInfo("PS1PlayerFaces", 0),
    FunctionInfo("PS1SpecialPlayerRun", 0),
    FunctionInfo("PS1SpecialSetupSort", 0),
    FunctionInfo("PS1SpecialSetupUpdate", 0),
#if PS1_GAME == 1
    FunctionInfo("PS1SSRotPos", 0),
    FunctionInfo("PS1SSBlockCollide", 0),
    FunctionInfo("PS1SSBlockDraw", 2),
    FunctionInfo("PS1SSRing", 0),
    FunctionInfo("PS1SSBlockUpdate", 0),
    FunctionInfo("PS1SSPlaceDraw", 1),
    FunctionInfo("PS1SSAnimDraw", 1),
    FunctionInfo("PS1SSGemDraw", 0),
    FunctionInfo("PS1SSObjUpdate", 1),
    FunctionInfo("PS1S1ZoneObj", 1),
    FunctionInfo("PS1S1Spring", 1),
    FunctionInfo("PS1S1LZSetup", 1),
#endif
#endif
};

#if RETRO_USE_COMPILER

int scriptValueListCount = 0;
// clang-format off
ScriptVariableInfo scriptValueList[SCRIPT_VAR_COUNT] = {
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "true", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "false", "0"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "FX_SCALE", "0"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "FX_ROTATE", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "FX_ROTOZOOM", "2"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "FX_INK", "3"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "PRESENTATION_STAGE", "0"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "REGULAR_STAGE", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "BONUS_STAGE", "2"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "SPECIAL_STAGE", "3"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "MENU_1", "0"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "MENU_2", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "C_TOUCH", "0"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "C_SOLID", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "C_SOLID2", "2"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "C_PLATFORM", "3"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "C_BOX", "65536"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "MAT_WORLD", "0"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "MAT_VIEW", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "MAT_TEMP", "2"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "FX_FLIP", "5"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "FACING_LEFT", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "FACING_RIGHT", "0"),
#if !RETRO_REV00
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "STAGE_2P_MODE", "4"),
#endif
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "STAGE_FROZEN", "3"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "STAGE_PAUSED", "2"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "STAGE_RUNNING", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "RESET_GAME", "2"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "STANDARD", "0"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "MOBILE", "1"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "DEVICE_XBOX", "2"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "DEVICE_PSN", "3"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "DEVICE_IOS", "4"),
    ScriptVariableInfo(VAR_ALIAS, ACCESS_PUBLIC, "DEVICE_ANDROID", "5"),
};
// clang-format on

const char scriptEvaluationTokens[][0x4] = { "=",  "+=", "-=", "++", "--", "*=", "/=", ">>=", "<<=", "&=",
                                             "|=", "^=", "%=", "==", ">",  ">=", "<",  "<=",  "!=" };

enum ScriptReadModes { READMODE_NORMAL = 0, READMODE_STRING = 1, READMODE_COMMENTLINE = 2, READMODE_ENDLINE = 3, READMODE_EOF = 4 };
enum ScriptParseModes {
    PARSEMODE_SCOPELESS    = 0,
    PARSEMODE_PLATFORMSKIP = 1,
    PARSEMODE_FUNCTION     = 2,
    PARSEMODE_SWITCHREAD   = 3,
    PARSEMODE_TABLEREAD    = 4,
    PARSEMODE_ERROR        = 0xFF
};
#endif

enum ScriptVarTypes { SCRIPTVAR_VAR = 1, SCRIPTVAR_INTCONST = 2, SCRIPTVAR_STRCONST = 3 };
enum ScriptVarArrTypes { VARARR_NONE = 0, VARARR_ARRAY = 1, VARARR_ENTNOPLUS1 = 2, VARARR_ENTNOMINUS1 = 3 };

enum ScrVar {
    VAR_TEMP0,
    VAR_TEMP1,
    VAR_TEMP2,
    VAR_TEMP3,
    VAR_TEMP4,
    VAR_TEMP5,
    VAR_TEMP6,
    VAR_TEMP7,
    VAR_CHECKRESULT,
    VAR_ARRAYPOS0,
    VAR_ARRAYPOS1,
    VAR_ARRAYPOS2,
    VAR_ARRAYPOS3,
    VAR_ARRAYPOS4,
    VAR_ARRAYPOS5,
    VAR_ARRAYPOS6,
    VAR_ARRAYPOS7,
    VAR_GLOBAL,
    VAR_LOCAL,
    VAR_OBJECTENTITYPOS,
    VAR_OBJECTGROUPID,
    VAR_OBJECTTYPE,
    VAR_OBJECTPROPERTYVALUE,
    VAR_OBJECTXPOS,
    VAR_OBJECTYPOS,
    VAR_OBJECTIXPOS,
    VAR_OBJECTIYPOS,
    VAR_OBJECTXVEL,
    VAR_OBJECTYVEL,
    VAR_OBJECTSPEED,
    VAR_OBJECTSTATE,
    VAR_OBJECTROTATION,
    VAR_OBJECTSCALE,
    VAR_OBJECTPRIORITY,
    VAR_OBJECTDRAWORDER,
    VAR_OBJECTDIRECTION,
    VAR_OBJECTINKEFFECT,
    VAR_OBJECTALPHA,
    VAR_OBJECTFRAME,
    VAR_OBJECTANIMATION,
    VAR_OBJECTPREVANIMATION,
    VAR_OBJECTANIMATIONSPEED,
    VAR_OBJECTANIMATIONTIMER,
    VAR_OBJECTANGLE,
    VAR_OBJECTLOOKPOSX,
    VAR_OBJECTLOOKPOSY,
    VAR_OBJECTCOLLISIONMODE,
    VAR_OBJECTCOLLISIONPLANE,
    VAR_OBJECTCONTROLMODE,
    VAR_OBJECTCONTROLLOCK,
    VAR_OBJECTPUSHING,
    VAR_OBJECTVISIBLE,
    VAR_OBJECTTILECOLLISIONS,
    VAR_OBJECTINTERACTION,
    VAR_OBJECTGRAVITY,
    VAR_OBJECTUP,
    VAR_OBJECTDOWN,
    VAR_OBJECTLEFT,
    VAR_OBJECTRIGHT,
    VAR_OBJECTJUMPPRESS,
    VAR_OBJECTJUMPHOLD,
    VAR_OBJECTSCROLLTRACKING,
    VAR_OBJECTFLOORSENSORL,
    VAR_OBJECTFLOORSENSORC,
    VAR_OBJECTFLOORSENSORR,
#if !RETRO_REV00
    VAR_OBJECTFLOORSENSORLC,
    VAR_OBJECTFLOORSENSORRC,
#endif
    VAR_OBJECTCOLLISIONLEFT,
    VAR_OBJECTCOLLISIONTOP,
    VAR_OBJECTCOLLISIONRIGHT,
    VAR_OBJECTCOLLISIONBOTTOM,
    VAR_OBJECTOUTOFBOUNDS,
    VAR_OBJECTSPRITESHEET,
    VAR_OBJECTVALUE0,
    VAR_OBJECTVALUE1,
    VAR_OBJECTVALUE2,
    VAR_OBJECTVALUE3,
    VAR_OBJECTVALUE4,
    VAR_OBJECTVALUE5,
    VAR_OBJECTVALUE6,
    VAR_OBJECTVALUE7,
    VAR_OBJECTVALUE8,
    VAR_OBJECTVALUE9,
    VAR_OBJECTVALUE10,
    VAR_OBJECTVALUE11,
    VAR_OBJECTVALUE12,
    VAR_OBJECTVALUE13,
    VAR_OBJECTVALUE14,
    VAR_OBJECTVALUE15,
    VAR_OBJECTVALUE16,
    VAR_OBJECTVALUE17,
    VAR_OBJECTVALUE18,
    VAR_OBJECTVALUE19,
    VAR_OBJECTVALUE20,
    VAR_OBJECTVALUE21,
    VAR_OBJECTVALUE22,
    VAR_OBJECTVALUE23,
    VAR_OBJECTVALUE24,
    VAR_OBJECTVALUE25,
    VAR_OBJECTVALUE26,
    VAR_OBJECTVALUE27,
    VAR_OBJECTVALUE28,
    VAR_OBJECTVALUE29,
    VAR_OBJECTVALUE30,
    VAR_OBJECTVALUE31,
    VAR_OBJECTVALUE32,
    VAR_OBJECTVALUE33,
    VAR_OBJECTVALUE34,
    VAR_OBJECTVALUE35,
    VAR_OBJECTVALUE36,
    VAR_OBJECTVALUE37,
    VAR_OBJECTVALUE38,
    VAR_OBJECTVALUE39,
    VAR_OBJECTVALUE40,
    VAR_OBJECTVALUE41,
    VAR_OBJECTVALUE42,
    VAR_OBJECTVALUE43,
    VAR_OBJECTVALUE44,
    VAR_OBJECTVALUE45,
    VAR_OBJECTVALUE46,
    VAR_OBJECTVALUE47,
    VAR_STAGESTATE,
    VAR_STAGEACTIVELIST,
    VAR_STAGELISTPOS,
    VAR_STAGETIMEENABLED,
    VAR_STAGEMILLISECONDS,
    VAR_STAGESECONDS,
    VAR_STAGEMINUTES,
    VAR_STAGEACTNUM,
    VAR_STAGEPAUSEENABLED,
    VAR_STAGELISTSIZE,
    VAR_STAGENEWXBOUNDARY1,
    VAR_STAGENEWXBOUNDARY2,
    VAR_STAGENEWYBOUNDARY1,
    VAR_STAGENEWYBOUNDARY2,
    VAR_STAGECURXBOUNDARY1,
    VAR_STAGECURXBOUNDARY2,
    VAR_STAGECURYBOUNDARY1,
    VAR_STAGECURYBOUNDARY2,
    VAR_STAGEDEFORMATIONDATA0,
    VAR_STAGEDEFORMATIONDATA1,
    VAR_STAGEDEFORMATIONDATA2,
    VAR_STAGEDEFORMATIONDATA3,
    VAR_STAGEWATERLEVEL,
    VAR_STAGEACTIVELAYER,
    VAR_STAGEMIDPOINT,
    VAR_STAGEPLAYERLISTPOS,
    VAR_STAGEDEBUGMODE,
    VAR_STAGEENTITYPOS,
    VAR_SCREENCAMERAENABLED,
    VAR_SCREENCAMERATARGET,
    VAR_SCREENCAMERASTYLE,
    VAR_SCREENCAMERAX,
    VAR_SCREENCAMERAY,
    VAR_SCREENDRAWLISTSIZE,
    VAR_SCREENXCENTER,
    VAR_SCREENYCENTER,
    VAR_SCREENXSIZE,
    VAR_SCREENYSIZE,
    VAR_SCREENXOFFSET,
    VAR_SCREENYOFFSET,
    VAR_SCREENSHAKEX,
    VAR_SCREENSHAKEY,
    VAR_SCREENADJUSTCAMERAY,
    VAR_TOUCHSCREENDOWN,
    VAR_TOUCHSCREENXPOS,
    VAR_TOUCHSCREENYPOS,
    VAR_MUSICVOLUME,
    VAR_MUSICCURRENTTRACK,
    VAR_MUSICPOSITION,
    VAR_KEYDOWNUP,
    VAR_KEYDOWNDOWN,
    VAR_KEYDOWNLEFT,
    VAR_KEYDOWNRIGHT,
    VAR_KEYDOWNBUTTONA,
    VAR_KEYDOWNBUTTONB,
    VAR_KEYDOWNBUTTONC,
    VAR_KEYDOWNBUTTONX,
    VAR_KEYDOWNBUTTONY,
    VAR_KEYDOWNBUTTONZ,
    VAR_KEYDOWNBUTTONL,
    VAR_KEYDOWNBUTTONR,
    VAR_KEYDOWNSTART,
    VAR_KEYDOWNSELECT,
    VAR_KEYPRESSUP,
    VAR_KEYPRESSDOWN,
    VAR_KEYPRESSLEFT,
    VAR_KEYPRESSRIGHT,
    VAR_KEYPRESSBUTTONA,
    VAR_KEYPRESSBUTTONB,
    VAR_KEYPRESSBUTTONC,
    VAR_KEYPRESSBUTTONX,
    VAR_KEYPRESSBUTTONY,
    VAR_KEYPRESSBUTTONZ,
    VAR_KEYPRESSBUTTONL,
    VAR_KEYPRESSBUTTONR,
    VAR_KEYPRESSSTART,
    VAR_KEYPRESSSELECT,
    VAR_MENU1SELECTION,
    VAR_MENU2SELECTION,
    VAR_TILELAYERXSIZE,
    VAR_TILELAYERYSIZE,
    VAR_TILELAYERTYPE,
    VAR_TILELAYERANGLE,
    VAR_TILELAYERXPOS,
    VAR_TILELAYERYPOS,
    VAR_TILELAYERZPOS,
    VAR_TILELAYERPARALLAXFACTOR,
    VAR_TILELAYERSCROLLSPEED,
    VAR_TILELAYERSCROLLPOS,
    VAR_TILELAYERDEFORMATIONOFFSET,
    VAR_TILELAYERDEFORMATIONOFFSETW,
    VAR_HPARALLAXPARALLAXFACTOR,
    VAR_HPARALLAXSCROLLSPEED,
    VAR_HPARALLAXSCROLLPOS,
    VAR_VPARALLAXPARALLAXFACTOR,
    VAR_VPARALLAXSCROLLSPEED,
    VAR_VPARALLAXSCROLLPOS,
    VAR_SCENE3DVERTEXCOUNT,
    VAR_SCENE3DFACECOUNT,
    VAR_SCENE3DPROJECTIONX,
    VAR_SCENE3DPROJECTIONY,
#if !RETRO_REV00
    VAR_SCENE3DFOGCOLOR,
    VAR_SCENE3DFOGSTRENGTH,
#endif
    VAR_VERTEXBUFFERX,
    VAR_VERTEXBUFFERY,
    VAR_VERTEXBUFFERZ,
    VAR_VERTEXBUFFERU,
    VAR_VERTEXBUFFERV,
    VAR_FACEBUFFERA,
    VAR_FACEBUFFERB,
    VAR_FACEBUFFERC,
    VAR_FACEBUFFERD,
    VAR_FACEBUFFERFLAG,
    VAR_FACEBUFFERCOLOR,
    VAR_SAVERAM,
    VAR_ENGINESTATE,
#if RETRO_REV00
    VAR_ENGINEMESSAGE,
#endif
    VAR_ENGINELANGUAGE,
    VAR_ENGINEONLINEACTIVE,
    VAR_ENGINESFXVOLUME,
    VAR_ENGINEBGMVOLUME,
#if RETRO_REV00
    VAR_ENGINEPLATFORMID, // v3-style device type aka Windows/Mac/Android/etc
#endif
    VAR_ENGINETRIALMODE,
#if !RETRO_REV00
    VAR_ENGINEDEVICETYPE, // v4-style device type aka Standard/Mobile/Etc
#endif

#if RETRO_REV03
    // Extras
    VAR_SCREENCURRENTID,
    VAR_CAMERAENABLED,
    VAR_CAMERATARGET,
    VAR_CAMERASTYLE,
    VAR_CAMERAXPOS,
    VAR_CAMERAYPOS,
    VAR_CAMERAADJUSTY,
#endif

#if RETRO_USE_HAPTICS
    VAR_HAPTICSENABLED,
#endif
    VAR_MAX_CNT
};

enum ScrFunc {
    FUNC_END,
    FUNC_EQUAL,
    FUNC_ADD,
    FUNC_SUB,
    FUNC_INC,
    FUNC_DEC,
    FUNC_MUL,
    FUNC_DIV,
    FUNC_SHR,
    FUNC_SHL,
    FUNC_AND,
    FUNC_OR,
    FUNC_XOR,
    FUNC_MOD,
    FUNC_FLIPSIGN,
    FUNC_CHECKEQUAL,
    FUNC_CHECKGREATER,
    FUNC_CHECKLOWER,
    FUNC_CHECKNOTEQUAL,
    FUNC_IFEQUAL,
    FUNC_IFGREATER,
    FUNC_IFGREATEROREQUAL,
    FUNC_IFLOWER,
    FUNC_IFLOWEROREQUAL,
    FUNC_IFNOTEQUAL,
    FUNC_ELSE,
    FUNC_ENDIF,
    FUNC_WEQUAL,
    FUNC_WGREATER,
    FUNC_WGREATEROREQUAL,
    FUNC_WLOWER,
    FUNC_WLOWEROREQUAL,
    FUNC_WNOTEQUAL,
    FUNC_LOOP,
    FUNC_FOREACHACTIVE,
    FUNC_FOREACHALL,
    FUNC_NEXT,
    FUNC_SWITCH,
    FUNC_BREAK,
    FUNC_ENDSWITCH,
    FUNC_RAND,
    FUNC_SIN,
    FUNC_COS,
    FUNC_SIN256,
    FUNC_COS256,
    FUNC_ATAN2,
    FUNC_INTERPOLATE,
    FUNC_INTERPOLATEXY,
    FUNC_LOADSPRITESHEET,
    FUNC_REMOVESPRITESHEET,
    FUNC_DRAWSPRITE,
    FUNC_DRAWSPRITEXY,
    FUNC_DRAWSPRITESCREENXY,
    FUNC_DRAWTINTRECT,
    FUNC_DRAWNUMBERS,
    FUNC_DRAWACTNAME,
    FUNC_DRAWMENU,
    FUNC_SPRITEFRAME,
    FUNC_EDITFRAME,
    FUNC_LOADPALETTE,
    FUNC_ROTATEPALETTE,
    FUNC_SETSCREENFADE,
    FUNC_SETACTIVEPALETTE,
    FUNC_SETPALETTEFADE,
    FUNC_SETPALETTEENTRY,
    FUNC_GETPALETTEENTRY,
    FUNC_COPYPALETTE,
    FUNC_CLEARSCREEN,
    FUNC_DRAWSPRITEFX,
    FUNC_DRAWSPRITESCREENFX,
    FUNC_LOADANIMATION,
    FUNC_SETUPMENU,
    FUNC_ADDMENUENTRY,
    FUNC_EDITMENUENTRY,
    FUNC_LOADSTAGE,
    FUNC_DRAWRECT,
    FUNC_RESETOBJECTENTITY,
    FUNC_BOXCOLLISIONTEST,
    FUNC_CREATETEMPOBJECT,
    FUNC_PROCESSOBJECTMOVEMENT,
    FUNC_PROCESSOBJECTCONTROL,
    FUNC_PROCESSANIMATION,
    FUNC_DRAWOBJECTANIMATION,
    FUNC_SETMUSICTRACK,
    FUNC_PLAYMUSIC,
    FUNC_STOPMUSIC,
    FUNC_PAUSEMUSIC,
    FUNC_RESUMEMUSIC,
    FUNC_SWAPMUSICTRACK,
    FUNC_PLAYSFX,
    FUNC_STOPSFX,
    FUNC_SETSFXATTRIBUTES,
    FUNC_OBJECTTILECOLLISION,
    FUNC_OBJECTTILEGRIP,
    FUNC_NOT,
    FUNC_DRAW3DSCENE,
    FUNC_SETIDENTITYMATRIX,
    FUNC_MATRIXMULTIPLY,
    FUNC_MATRIXTRANSLATEXYZ,
    FUNC_MATRIXSCALEXYZ,
    FUNC_MATRIXROTATEX,
    FUNC_MATRIXROTATEY,
    FUNC_MATRIXROTATEZ,
    FUNC_MATRIXROTATEXYZ,
#if !RETRO_REV00
    FUNC_MATRIXINVERSE,
#endif
    FUNC_TRANSFORMVERTICES,
    FUNC_CALLFUNCTION,
    FUNC_RETURN,
    FUNC_SETLAYERDEFORMATION,
    FUNC_CHECKTOUCHRECT,
    FUNC_GETTILELAYERENTRY,
    FUNC_SETTILELAYERENTRY,
    FUNC_GETBIT,
    FUNC_SETBIT,
    FUNC_CLEARDRAWLIST,
    FUNC_ADDDRAWLISTENTITYREF,
    FUNC_GETDRAWLISTENTITYREF,
    FUNC_SETDRAWLISTENTITYREF,
    FUNC_GET16X16TILEINFO,
    FUNC_SET16X16TILEINFO,
    FUNC_COPY16X16TILE,
    FUNC_GETANIMATIONBYNAME,
    FUNC_READSAVERAM,
    FUNC_WRITESAVERAM,
#if !RETRO_REV02
    FUNC_LOADTEXTFONT,
#endif
    FUNC_LOADTEXTFILE,
    FUNC_GETTEXTINFO,
#if !RETRO_REV02
    FUNC_DRAWTEXT,
#endif
    FUNC_GETVERSIONNUMBER,
    FUNC_GETTABLEVALUE,
    FUNC_SETTABLEVALUE,
    FUNC_CHECKCURRENTSTAGEFOLDER,
    FUNC_ABS,
    FUNC_CALLNATIVEFUNCTION,
    FUNC_CALLNATIVEFUNCTION2,
    FUNC_CALLNATIVEFUNCTION4,
    FUNC_SETOBJECTRANGE,
#if RETRO_REV02
    FUNC_GETOBJECTVALUE,
    FUNC_SETOBJECTVALUE,
    FUNC_COPYOBJECT,
#endif
    FUNC_PRINT,

#if RETRO_REV03
    // Extras
    FUNC_CHECKCAMERAPROXIMITY,
    FUNC_SETSCREENCOUNT,
    FUNC_SETSCREENVERTICES,
    FUNC_GETINPUTDEVICEID,
    FUNC_GETFILTEREDINPUTDEVICEID,
    FUNC_GETINPUTDEVICETYPE,
    FUNC_ISINPUTDEVICEASSIGNED,
    FUNC_ASSIGNINPUTSLOTTODEVICE,
    FUNC_ISSLOTASSIGNED,
    FUNC_RESETINPUTSLOTASSIGNMENTS,
#endif
#if RETRO_PLATFORM == RETRO_PS1
    FUNC_PS1OSCILLATE,
    FUNC_PS1RING,
    FUNC_PS1BRIDGEDRAW,
    FUNC_PS1LOSERING,
    FUNC_PS1HORIZONTALDOOR,
    FUNC_PS1BUTTONBRIDGE,
    FUNC_PS1PLANESWITCHV,
    FUNC_PS1PLANESWITCHH,
    FUNC_PS1ROTATEPLATFORM,
    FUNC_PS1ROTATEPLATFORMDRAW,
    FUNC_PS1HPZBRIDGE,
    FUNC_PS1HPZBRIDGEDRAW,
    FUNC_PS1STAGESETUP,
    FUNC_PS1HUDDRAW,
    FUNC_PS1TURRETPLATFORM,
    FUNC_PS1BELTPLATFORM,
    FUNC_PS1HFLIPPER,
    FUNC_PS1EARTHQUAKE,
    FUNC_PS1SPIKES,
    FUNC_PS1PLAYERINPUT,
    FUNC_PS1CLEDGE,
    FUNC_PS1STEAMPISTON,
    FUNC_PS1PLAYERFN2,
    FUNC_PS1PLAYERFN3,
    FUNC_PS1PLAYERFN4,
    FUNC_PS1PLAYERFN5,
    FUNC_PS1PLAYERFN6,
    FUNC_PS1PLAYERFN51,
    FUNC_PS1PLAYERFN52,
    FUNC_PS1PLAYERFN53,
    FUNC_PS1PLAYERSTATE10,
    FUNC_PS1PLAYERSTATE12,
    FUNC_PS1TAILSFN61,
    FUNC_PS1TAILSFN62,
    FUNC_PS1TAILSFN66,
    FUNC_PS1TAILSFN67,
    FUNC_PS1MPZSETUP,
    FUNC_PS1MONITOR,
    FUNC_PS1INVISIBLEBLOCK,
    FUNC_PS1SPECIALRING,
    FUNC_PS1HALFPIPE,
    FUNC_PS1HALFPIPESEGMENT,
    FUNC_PS1PLAYERFACES,
    FUNC_PS1SPECIALPLAYERRUN,
    FUNC_PS1SPECIALSETUPSORT,
    FUNC_PS1SPECIALSETUPUPDATE,
#if PS1_GAME == 1
    // Sonic 1's own natives (docs/37), after Sonic 2's: its build only (the tools list them too, after the same numbers)
    FUNC_PS1SSROTPOS,
    FUNC_PS1SSBLOCKCOLLIDE,
    FUNC_PS1SSBLOCKDRAW,
    FUNC_PS1SSRING,
    FUNC_PS1SSBLOCKUPDATE,
    FUNC_PS1SSPLACEDRAW,
    FUNC_PS1SSANIMDRAW,
    FUNC_PS1SSGEMDRAW,
    FUNC_PS1SSOBJUPDATE,
    FUNC_PS1S1ZONEOBJ,
    FUNC_PS1S1SPRING,
    FUNC_PS1S1LZSETUP,
#endif
#endif
    FUNC_MAX_CNT
};

ObjectScript objectScriptList[OBJECT_COUNT];
ScriptFunction scriptFunctionList[FUNCTION_COUNT];
#if RETRO_USE_COMPILER
int scriptFunctionCount = 0;
#endif

#if RETRO_PLATFORM == RETRO_PS1
PS1ScriptCode scriptCode;
#else
int scriptCode[SCRIPTCODE_COUNT];
#endif
int jumpTable[JUMPTABLE_COUNT];
int jumpTableStack[JUMPSTACK_COUNT];
int functionStack[FUNCSTACK_COUNT];
int foreachStack[FORSTACK_COUNT];

int scriptCodePos     = 0;
int scriptCodeOffset  = 0;
int jumpTablePos      = 0;
int jumpTableOffset   = 0;
int jumpTableStackPos = 0;
int functionStackPos  = 0;
int foreachStackPos   = 0;

#if !(RETRO_PLATFORM == RETRO_PS1 && !defined(RETRO_PS1_HOST_TOOL)) // PS1: in the scratchpad (Script.hpp)
ScriptEngine scriptEng = ScriptEngine();
#endif
#if RETRO_PLATFORM == RETRO_PS1
char scriptText[PS1_SCRIPTTEXT_SIZE];
#define PS1_TXT(c) scriptText[(uint)(c) < PS1_SCRIPTTEXT_SIZE - 1 ? (c) : PS1_SCRIPTTEXT_SIZE - 1]
#else
char scriptText[0x4000];
#define PS1_TXT(c) scriptText[c]
#endif

#if RETRO_USE_COMPILER
void CheckAliasText(char *text)
{
    if (FindStringToken(text, "publicalias", 1) == 0) {
#if !RETRO_USE_ORIGINAL_CODE
        if (scriptValueListCount >= SCRIPT_VAR_COUNT) {
            SetupTextMenu(&gameMenu[0], 0);
            AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
            AddTextMenuEntry(&gameMenu[0], " ");
            AddTextMenuEntry(&gameMenu[0], "TOO MANY ALIASES, STATIC");
            AddTextMenuEntry(&gameMenu[0], "VALUES, AND TABLES");
            Engine.gameMode = ENGINE_SCRIPTERROR;
            return;
        }
#endif

        ScriptVariableInfo *variable = &scriptValueList[scriptValueListCount];
        MEM_ZEROP(variable);

        int textStrPos = 11;
        int varStrPos  = 0;
        int parseMode  = 0;

        while (text[textStrPos]) {
            switch (parseMode) {
                default: break;

                case 0:
                    if (text[textStrPos] == ':') {
                        textStrPos++;
                        variable->value[varStrPos] = 0;
                        varStrPos                  = 0;
                        parseMode                  = 1;
                    }
                    else {
                        variable->value[varStrPos++] = text[textStrPos++];
                    }
                    break;

                case 1: variable->name[varStrPos++] = text[textStrPos++]; break;
            }
        }

        variable->access = ACCESS_PUBLIC;

#if !RETRO_USE_ORIGINAL_CODE
        for (int v = 0; v < scriptValueListCount; ++v) {
            if (StrComp(scriptValueList[v].name, variable->name))
                PrintLog("WARNING: Variable Name '%s' has already been used!", variable->name);
        }
#endif

        ++scriptValueListCount;
    }
    else if (FindStringToken(text, "privatealias", 1) == 0) {
#if !RETRO_USE_ORIGINAL_CODE
        if (scriptValueListCount >= SCRIPT_VAR_COUNT) {
            SetupTextMenu(&gameMenu[0], 0);
            AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
            AddTextMenuEntry(&gameMenu[0], " ");
            AddTextMenuEntry(&gameMenu[0], "TOO MANY ALIASES, STATIC");
            AddTextMenuEntry(&gameMenu[0], "VALUES, AND TABLES");
            Engine.gameMode = ENGINE_SCRIPTERROR;
            return;
        }
#endif

        ScriptVariableInfo *variable = &scriptValueList[scriptValueListCount];
        MEM_ZEROP(variable);

        int textStrPos = 12;
        int varStrPos  = 0;
        int parseMode  = 0;

        while (text[textStrPos]) {
            switch (parseMode) {
                default: break;

                case 0:
                    if (text[textStrPos] == ':') {
                        textStrPos++;
                        variable->value[varStrPos] = 0;
                        varStrPos                  = 0;
                        parseMode                  = 1;
                    }
                    else {
                        variable->value[varStrPos++] = text[textStrPos++];
                    }
                    break;

                case 1: variable->name[varStrPos++] = text[textStrPos++]; break;
            }
        }

        variable->access = ACCESS_PRIVATE;

#if !RETRO_USE_ORIGINAL_CODE
        for (int v = 0; v < scriptValueListCount; ++v) {
            if (StrComp(scriptValueList[v].name, variable->name))
                PrintLog("WARNING: Variable Name '%s' has already been used!", variable->name);
        }
#endif

        ++scriptValueListCount;
    }
}
void CheckStaticText(char *text)
{
    if (FindStringToken(text, "publicvalue", 1) == 0) {
#if !RETRO_USE_ORIGINAL_CODE
        if (scriptValueListCount >= SCRIPT_VAR_COUNT) {
            SetupTextMenu(&gameMenu[0], 0);
            AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
            AddTextMenuEntry(&gameMenu[0], " ");
            AddTextMenuEntry(&gameMenu[0], "TOO MANY ALIASES, STATIC");
            AddTextMenuEntry(&gameMenu[0], "VALUES, AND TABLES");
            Engine.gameMode = ENGINE_SCRIPTERROR;
            return;
        }
#endif

        ScriptVariableInfo *variable = &scriptValueList[scriptValueListCount];
        MEM_ZEROP(variable);

        int textStrPos = 11;
        int varStrPos  = 0;
        int parseMode  = 0;

        StrCopy(variable->value, "0"); // default value is 0
        while (text[textStrPos]) {
            switch (parseMode) {
                default: break;

                case 0:
                    if (text[textStrPos] == '=') {
                        textStrPos++;
                        variable->name[varStrPos] = 0;
                        varStrPos                 = 0;
                        parseMode                 = 1;
                    }
                    else {
                        variable->name[varStrPos++] = text[textStrPos++];
                    }
                    break;

                case 1: variable->value[varStrPos++] = text[textStrPos++]; break;
            }
        }

        variable->access = ACCESS_PUBLIC;

        if (!ConvertStringToInteger(variable->value, &scriptCode[scriptCodePos]))
            scriptCode[scriptCodePos] = 0;

        StrCopy(variable->value, "local[");
        AppendIntegerToString(variable->value, scriptCodePos++);
        StrAdd(variable->value, "]");

#if !RETRO_USE_ORIGINAL_CODE
        for (int v = 0; v < scriptValueListCount; ++v) {
            if (StrComp(scriptValueList[v].name, variable->name))
                PrintLog("WARNING: Variable Name '%s' has already been used!", variable->name);
        }
#endif

        ++scriptValueListCount;
    }
    else if (FindStringToken(text, "privatevalue", 1) == 0) {
#if !RETRO_USE_ORIGINAL_CODE
        if (scriptValueListCount >= SCRIPT_VAR_COUNT) {
            SetupTextMenu(&gameMenu[0], 0);
            AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
            AddTextMenuEntry(&gameMenu[0], " ");
            AddTextMenuEntry(&gameMenu[0], "TOO MANY ALIASES, STATIC");
            AddTextMenuEntry(&gameMenu[0], "VALUES, AND TABLES");
            Engine.gameMode = ENGINE_SCRIPTERROR;
            return;
        }
#endif

        ScriptVariableInfo *variable = &scriptValueList[scriptValueListCount];
        MEM_ZEROP(variable);

        int textStrPos = 12;
        int varStrPos  = 0;
        int parseMode  = 0;

        StrCopy(variable->value, "0"); // default value is 0
        while (text[textStrPos]) {
            switch (parseMode) {
                default: break;

                case 0:
                    if (text[textStrPos] == '=') {
                        textStrPos++;
                        variable->name[varStrPos] = 0;
                        varStrPos                 = 0;
                        parseMode                 = 1;
                    }
                    else {
                        variable->name[varStrPos++] = text[textStrPos++];
                    }
                    break;

                case 1: variable->value[varStrPos++] = text[textStrPos++]; break;
            }
        }

        variable->access = ACCESS_PRIVATE;

        if (!ConvertStringToInteger(variable->value, &scriptCode[scriptCodePos]))
            scriptCode[scriptCodePos] = 0;

        StrCopy(variable->value, "local[");
        AppendIntegerToString(variable->value, scriptCodePos++);
        StrAdd(variable->value, "]");

#if !RETRO_USE_ORIGINAL_CODE
        for (int v = 0; v < scriptValueListCount; ++v) {
            if (StrComp(scriptValueList[v].name, variable->name))
                PrintLog("WARNING: Variable Name '%s' has already been used!", variable->name);
        }
#endif

        ++scriptValueListCount;
    }
}
bool CheckTableText(char *text)
{
    bool hasValues = false;

    if (FindStringToken(text, "publictable", 1) == 0) {
#if !RETRO_USE_ORIGINAL_CODE
        if (scriptValueListCount >= SCRIPT_VAR_COUNT) {
            SetupTextMenu(&gameMenu[0], 0);
            AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
            AddTextMenuEntry(&gameMenu[0], " ");
            AddTextMenuEntry(&gameMenu[0], "TOO MANY ALIASES, STATIC");
            AddTextMenuEntry(&gameMenu[0], "VALUES, AND TABLES");
            Engine.gameMode = ENGINE_SCRIPTERROR;
            return false;
        }
#endif

        ScriptVariableInfo *variable = &scriptValueList[scriptValueListCount];
        MEM_ZEROP(variable);

        int textStrPos = 11;
        int varStrPos  = 0;

        while (text[textStrPos]) {
            if (text[textStrPos] == '[' || text[textStrPos] == ']') {
                variable->name[varStrPos] = 0;
                textStrPos++;
                break;
            }
            else {
                variable->name[varStrPos++] = text[textStrPos++];
            }
        }

        if (FindStringToken(text, "]", 1) < 1) {
            // has default values, we'll stop here and read stuff in a seperate mode
            scriptCode[scriptCodePos] = 0;
            StrCopy(variable->value, "");
            AppendIntegerToString(variable->value, scriptCodePos);
            scriptCodeOffset = scriptCodePos++;
            hasValues        = true;
        }
        else {
            // no default values, just an array size

            varStrPos = 0;
            while (text[textStrPos]) {
                if (text[textStrPos] == '[' || text[textStrPos] == ']') {
                    variable->value[varStrPos] = 0;
                    textStrPos++;
                    break;
                }
                else {
                    variable->value[varStrPos++] = text[textStrPos++];
                }
            }

            // array size can be an variable (alias), how cool!
            for (int v = 0; v < scriptValueListCount; ++v) {
                if (StrComp(variable->value, scriptValueList[v].name))
                    StrCopy(variable->value, scriptValueList[v].value);
            }

            if (!ConvertStringToInteger(variable->value, &scriptCode[scriptCodePos])) {
                scriptCode[scriptCodePos] = 1;
#if !RETRO_USE_ORIGINAL_CODE
                PrintLog("WARNING: Unable to parse table size!");
#endif
            }

            StrCopy(variable->value, "");
            AppendIntegerToString(variable->value, scriptCodePos);

            int valueCount = scriptCode[scriptCodePos++];
            for (int v = 0; v < valueCount; ++v) scriptCode[scriptCodePos++] = 0;
        }

        variable->access = ACCESS_PUBLIC;
        scriptValueListCount++;
    }
    else if (FindStringToken(text, "privatetable", 1) == 0) {
#if !RETRO_USE_ORIGINAL_CODE
        if (scriptValueListCount >= SCRIPT_VAR_COUNT) {
            SetupTextMenu(&gameMenu[0], 0);
            AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
            AddTextMenuEntry(&gameMenu[0], " ");
            AddTextMenuEntry(&gameMenu[0], "TOO MANY ALIASES, STATIC");
            AddTextMenuEntry(&gameMenu[0], "VALUES, AND TABLES");
            Engine.gameMode = ENGINE_SCRIPTERROR;
            return false;
        }
#endif

        ScriptVariableInfo *variable = &scriptValueList[scriptValueListCount];
        MEM_ZEROP(variable);

        int textStrPos = 12;
        int varStrPos  = 0;

        while (text[textStrPos]) {
            if (text[textStrPos] == '[' || text[textStrPos] == ']') {
                variable->name[varStrPos] = 0;
                textStrPos++;
                break;
            }
            else {
                variable->name[varStrPos++] = text[textStrPos++];
            }
        }

        if (FindStringToken(text, "]", 1) < 1) {
            // has default values, we'll stop here and read stuff in a seperate mode
            scriptCode[scriptCodePos] = 0;
            StrCopy(variable->value, "");
            AppendIntegerToString(variable->value, scriptCodePos);
            scriptCodeOffset = scriptCodePos++;
            hasValues        = true;
        }
        else {
            // no default values, just an array size

            varStrPos = 0;
            while (text[textStrPos]) {
                if (text[textStrPos] == '[' || text[textStrPos] == ']') {
                    variable->value[varStrPos] = 0;
                    textStrPos++;
                    break;
                }
                else {
                    variable->value[varStrPos++] = text[textStrPos++];
                }
            }

            // array size can be an variable (alias), how cool!
            for (int v = 0; v < scriptValueListCount; ++v) {
                if (StrComp(variable->value, scriptValueList[v].name))
                    StrCopy(variable->value, scriptValueList[v].value);
            }

            if (!ConvertStringToInteger(variable->value, &scriptCode[scriptCodePos])) {
                scriptCode[scriptCodePos] = 1;
#if !RETRO_USE_ORIGINAL_CODE
                PrintLog("WARNING: Unable to parse table size!");
#endif
            }

            StrCopy(variable->value, "");
            AppendIntegerToString(variable->value, scriptCodePos);

            int valueCount = scriptCode[scriptCodePos++];
            for (int v = 0; v < valueCount; ++v) scriptCode[scriptCodePos++] = 0;
        }

        variable->access = ACCESS_PRIVATE;
        scriptValueListCount++;
    }

    return hasValues;
}
void ConvertArithmaticSyntax(char *text)
{
    int token  = 0;
    int offset = 0;
    int findID = 0;
    char dest[260];

    for (int i = FUNC_EQUAL; i <= FUNC_MOD; ++i) {
        findID = FindStringToken(text, scriptEvaluationTokens[i - 1], 1);
        if (findID > -1) {
            offset = findID;
            token  = i;
        }
    }

    if (token > 0) {
        StrCopy(dest, functions[token].name);
        StrAdd(dest, "(");
        findID = StrLength(dest);
        for (int i = 0; i < offset; ++i) dest[findID++] = text[i];
        if (functions[token].opcodeSize > 1) {
            dest[findID] = ',';
            int len      = StrLength(scriptEvaluationTokens[token - 1]);
            offset += len;
            ++findID;
            while (text[offset]) dest[findID++] = text[offset++];
        }
        dest[findID] = 0;
        StrAdd(dest, ")");
        StrCopy(text, dest);
    }
}
void ConvertConditionalStatement(char *text)
{
    char dest[260];
    int compareOp  = -1;
    int strPos     = 0;
    int destStrPos = 0;

    if (FindStringToken(text, "if", 1) == 0) {
        for (int i = 0; i < 6; ++i) {
            destStrPos = FindStringToken(text, scriptEvaluationTokens[i + FUNC_MOD], 1);
            if (destStrPos > -1) {
                strPos    = destStrPos;
                compareOp = i;
            }
        }

        if (compareOp > -1) {
            text[strPos] = ',';
            StrCopy(dest, functions[compareOp + FUNC_IFEQUAL].name);
            StrAdd(dest, "(");
            AppendIntegerToString(dest, jumpTablePos - jumpTableOffset);
            StrAdd(dest, ",");

            destStrPos = StrLength(dest);
            for (int i = 2; text[i]; ++i) {
                if (text[i] != '=' && text[i] != '(' && text[i] != ')')
                    dest[destStrPos++] = text[i];
            }
            dest[destStrPos] = 0;

            StrAdd(dest, ")");
            StrCopy(text, dest);

            jumpTableStack[++jumpTableStackPos] = jumpTablePos;
            jumpTable[jumpTablePos++]       = -1;
            jumpTable[jumpTablePos++]       = 0;
        }
    }
    else if (FindStringToken(text, "while", 1) == 0) {
        for (int i = 0; i < 6; ++i) {
            destStrPos = FindStringToken(text, scriptEvaluationTokens[i + FUNC_MOD], 1);
            if (destStrPos > -1) {
                strPos    = destStrPos;
                compareOp = i;
            }
        }

        if (compareOp > -1) {
            text[strPos] = ',';
            StrCopy(dest, functions[compareOp + FUNC_WEQUAL].name);
            StrAdd(dest, "(");
            AppendIntegerToString(dest, jumpTablePos - jumpTableOffset);
            StrAdd(dest, ",");

            destStrPos = StrLength(dest);
            for (int i = 5; text[i]; ++i) {
                if (text[i] != '=' && text[i] != '(' && text[i] != ')')
                    dest[destStrPos++] = text[i];
            }
            dest[destStrPos] = 0;

            StrAdd(dest, ")");
            StrCopy(text, dest);

            jumpTableStack[++jumpTableStackPos] = jumpTablePos;
            jumpTable[jumpTablePos++]       = scriptCodePos - scriptCodeOffset;
            jumpTable[jumpTablePos++]       = 0;
        }
    }
    else if (FindStringToken(text, "foreach", 1) == 0) {
        int argStrPos = FindStringToken(text, ",", 2);

        if (argStrPos > -1) {
            StrCopy(dest, functions[text[argStrPos + 2] == 'C' ? (int)FUNC_FOREACHACTIVE : (int)FUNC_FOREACHALL].name);
            StrAdd(dest, "(");
            AppendIntegerToString(dest, jumpTablePos - jumpTableOffset);
            StrAdd(dest, ",");

            destStrPos = StrLength(dest);
            for (int i = 7; text[i] && i < argStrPos; ++i) {
                if (text[i] != '(' && text[i] != ')')
                    dest[destStrPos++] = text[i];
            }
            dest[destStrPos] = 0;

            StrAdd(dest, ")");
            StrCopy(text, dest);

            jumpTableStack[++jumpTableStackPos] = jumpTablePos;
            jumpTable[jumpTablePos++]       = scriptCodePos - scriptCodeOffset;
            jumpTable[jumpTablePos++]       = 0;
        }
    }
}
bool ConvertSwitchStatement(char *text)
{
    if (FindStringToken(text, "switch", 1) != 0)
        return false;

    char switchText[260];
    StrCopy(switchText, "switch");
    StrAdd(switchText, "(");
    AppendIntegerToString(switchText, jumpTablePos - jumpTableOffset);
    StrAdd(switchText, ",");
    int pos = StrLength(switchText);
    for (int i = 6; text[i]; ++i) {
        if (text[i] != '=' && text[i] != '(' && text[i] != ')')
            switchText[pos++] = text[i];
    }
    switchText[pos] = 0;
    StrAdd(switchText, ")");
    StrCopy(text, switchText);
    jumpTableStack[++jumpTableStackPos] = jumpTablePos;
    jumpTable[jumpTablePos++]       = 0x10000;
    jumpTable[jumpTablePos++]       = -0x10000;
    jumpTable[jumpTablePos++]       = -1;
    jumpTable[jumpTablePos++]       = 0;

    return true;
}
void ConvertFunctionText(char *text)
{
    char arrayStr[0x80];
    char funcName[132];

    int opcode     = 0;
    int opcodeSize = 0;
    int textPos    = 0;
    int namePos    = 0;

    for (namePos = 0; text[namePos] != '(' && text[namePos]; ++namePos) funcName[namePos] = text[namePos];
    funcName[namePos] = 0;

    for (int i = 0; i < FUNC_MAX_CNT; ++i) {
        if (StrComp(funcName, functions[i].name)) {
            opcode     = i;
            opcodeSize = functions[i].opcodeSize;
            textPos    = StrLength(functions[i].name);
            i          = FUNC_MAX_CNT;
        }
    }

    if (opcode <= 0) {
        SetupTextMenu(&gameMenu[0], 0);
        AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
        AddTextMenuEntry(&gameMenu[0], " ");
        AddTextMenuEntry(&gameMenu[0], "OPCODE NOT FOUND");
        AddTextMenuEntry(&gameMenu[0], funcName);
#if !RETRO_USE_ORIGINAL_CODE
        AddTextMenuEntry(&gameMenu[0], " ");
        AddTextMenuEntry(&gameMenu[0], "LINE NUMBER");
        char buffer[0x10];
        buffer[0] = 0;
        AppendIntegerToString(buffer, lineID);
        AddTextMenuEntry(&gameMenu[0], buffer);
#endif
        Engine.gameMode = ENGINE_SCRIPTERROR;
    }
    else {
        scriptCode[scriptCodePos++] = opcode;
        if (StrComp("else", functions[opcode].name))
            jumpTable[jumpTableStack[jumpTableStackPos]] = scriptCodePos - scriptCodeOffset;

        if (StrComp("endif", functions[opcode].name) == 1) {
            int jPos                = jumpTableStack[jumpTableStackPos];
            jumpTable[jPos + 1] = scriptCodePos - scriptCodeOffset;
            if (jumpTable[jPos] == -1)
                jumpTable[jPos] = (scriptCodePos - scriptCodeOffset) - 1;
            --jumpTableStackPos;
        }

        if (StrComp("endswitch", functions[opcode].name)) {
            int jPos                = jumpTableStack[jumpTableStackPos];
            jumpTable[jPos + 3] = scriptCodePos - scriptCodeOffset;
            if (jumpTable[jPos + 2] == -1) {
                jumpTable[jPos + 2] = (scriptCodePos - scriptCodeOffset) - 1;
                int caseCnt             = abs(jumpTable[jPos + 1] - jumpTable[jPos]) + 1;

                int jOffset = jPos + 4;
                for (int c = 0; c < caseCnt; ++c) {
                    if (jumpTable[jOffset + c] < 0)
                        jumpTable[jOffset + c] = jumpTable[jPos + 2];
                }
            }
            --jumpTableStackPos;
        }

        if (StrComp("loop", functions[opcode].name) || StrComp("next", functions[opcode].name)) {
            jumpTable[jumpTableStack[jumpTableStackPos--] + 1] = scriptCodePos - scriptCodeOffset;
        }

        for (int i = 0; i < opcodeSize; ++i) {
            ++textPos;
            int funcNamePos = 0;
            int mode        = 0;
            int prevMode    = 0;
            int arrayStrPos = 0;
            while (((text[textPos] != ',' && text[textPos] != ')') || mode == 2) && text[textPos]) {
                switch (mode) {
                    case 0: // normal
                        if (text[textPos] == '[')
                            mode = 1;
                        else if (text[textPos] == '"') {
                            prevMode                = mode;
                            mode                    = 2;
                            funcName[funcNamePos++] = '"';
                        }
                        else
                            funcName[funcNamePos++] = text[textPos];
                        ++textPos;
                        break;

                    case 1: // array val
                        if (text[textPos] == ']')
                            mode = 0;
                        else if (text[textPos] == '"') {
                            prevMode = mode;
                            mode     = 2;
                        }
                        else
                            arrayStr[arrayStrPos++] = text[textPos];
                        ++textPos;
                        break;

                    case 2: // string
                        if (text[textPos] == '"') {
                            mode                    = prevMode;
                            funcName[funcNamePos++] = '"';
                        }
                        else
                            funcName[funcNamePos++] = text[textPos];
                        ++textPos;
                        break;
                }
            }
            funcName[funcNamePos] = 0;
            arrayStr[arrayStrPos] = 0;

            for (int v = 0; v < scriptValueListCount; ++v) {
                if (StrComp(funcName, scriptValueList[v].name)) {
                    CopyAliasStr(funcName, scriptValueList[v].value, 0);
                    if (FindStringToken(scriptValueList[v].value, "[", 1) > -1)
                        CopyAliasStr(arrayStr, scriptValueList[v].value, 1);
                }
            }

            if (arrayStr[0]) {
                char arrStrBuf[0x80];
                int arrPos = 0;
                int bufPos = 0;
                if (arrayStr[0] == '+' || arrayStr[0] == '-')
                    ++arrPos;
                while (arrayStr[arrPos]) arrStrBuf[bufPos++] = arrayStr[arrPos++];
                arrStrBuf[bufPos] = 0;

                for (int v = 0; v < scriptValueListCount; ++v) {
                    if (StrComp(arrStrBuf, scriptValueList[v].name)) {
                        char pref = arrayStr[0];
                        CopyAliasStr(arrayStr, scriptValueList[v].value, 0);

                        if (pref == '+' || pref == '-') {
                            int len = StrLength(arrayStr);
                            for (int i = len; i >= 0; --i) arrayStr[i + 1] = arrayStr[i];
                            arrayStr[0] = pref;
                        }
                    }
                }
            }

            // Eg: temp0 = game.variable
            for (int v = 0; v < globalVariablesCount; ++v) {
                if (StrComp(funcName, globalVariableNames[v])) {
                    StrCopy(funcName, "global");
                    arrayStr[0] = 0;
                    AppendIntegerToString(arrayStr, v);
                }
            }

            // Eg: temp0 = Function1
            for (int f = 0; f < scriptFunctionCount; ++f) {
                if (StrComp(funcName, scriptFunctionList[f].name)) {
                    funcName[0] = 0;
                    AppendIntegerToString(funcName, f);
                }
            }

            // Eg: temp0 = TypeName[Player Object]
            if (StrComp(funcName, "TypeName")) {
                funcName[0] = '0';
                funcName[1] = 0;

                int o = 0;
                for (; o < OBJECT_COUNT; ++o) {
                    if (StrComp(arrayStr, typeNames[o])) {
                        funcName[0] = 0;
                        AppendIntegerToString(funcName, o);
                        break;
                    }
                }

                if (o == OBJECT_COUNT)
                    PrintLog("WARNING: Unknown typename \"%s\", on line %d", arrayStr, lineID);
            }

#if !RETRO_USE_ORIGINAL_CODE
            // Eg: temp0 = SfxName[Jump]
            if (StrComp(funcName, "SfxName")) {
                funcName[0] = '0';
                funcName[1] = 0;

                int s = 0;
                for (; s < SFX_COUNT; ++s) {
                    if (StrComp(arrayStr, sfxNames[s])) {
                        funcName[0] = 0;
                        AppendIntegerToString(funcName, s);
                        break;
                    }
                }

                if (s == SFX_COUNT)
                    PrintLog("WARNING: Unknown sfxName \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = VarName[player.lives]
            if (StrComp(funcName, "VarName")) {
                funcName[0] = '0';
                funcName[1] = 0;

                int v = 0;
                for (; v < globalVariablesCount; ++v) {
                    if (StrComp(arrayStr, globalVariableNames[v])) {
                        funcName[0] = 0;
                        AppendIntegerToString(funcName, v);
                        break;
                    }
                }

                if (v == globalVariablesCount)
                    PrintLog("WARNING: Unknown varName \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = AchievementName[Ring King]
            if (StrComp(funcName, "AchievementName")) {
                funcName[0] = '0';
                funcName[1] = 0;

                int a = 0;
                for (; a < achievementCount; ++a) {
                    char buf[0x40];
                    char *str = achievements[a].name;
                    int pos   = 0;

                    while (*str) {
                        if (*str != ' ')
                            buf[pos++] = *str;
                        str++;
                    }
                    buf[pos] = 0;

                    if (StrComp(arrayStr, buf)) {
                        funcName[0] = 0;
                        AppendIntegerToString(funcName, a);
                        break;
                    }
                }

                if (a == achievementCount)
                    PrintLog("WARNING: Unknown AchievementName \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = PlayerName[SONIC]
            if (StrComp(funcName, "PlayerName")) {
                funcName[0] = '0';
                funcName[1] = 0;

                int p = 0;
                for (; p < PLAYER_COUNT; ++p) {
                    char buf[0x40];
                    char *str = playerNames[p];
                    int pos   = 0;

                    while (*str) {
                        if (*str != ' ')
                            buf[pos++] = *str;
                        str++;
                    }
                    buf[pos] = 0;

                    if (StrComp(arrayStr, buf)) {
                        funcName[0] = 0;
                        AppendIntegerToString(funcName, p);
                        break;
                    }
                }

                if (p == PLAYER_COUNT)
                    PrintLog("WARNING: Unknown PlayerName \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = StageName[R - GREEN HILL ZONE 1]
            if (StrComp(funcName, "StageName")) {
                funcName[0] = '0';
                funcName[1] = 0;

                int s = -1;
                if (StrLength(arrayStr) >= 2) {
                    char list = arrayStr[0];
                    switch (list) {
                        case 'P': list = STAGELIST_PRESENTATION; break;
                        case 'R': list = STAGELIST_REGULAR; break;
                        case 'S': list = STAGELIST_SPECIAL; break;
                        case 'B': list = STAGELIST_BONUS; break;
                    }
                    s = GetSceneID(list, &arrayStr[2]);
                }

                if (s == -1) {
                    char buf[0x40];
                    sprintf(buf, "WARNING: Unknown StageName \"%s\", on line %d", arrayStr, lineID);
                    PrintLog(buf);
                    s = 0;
                }
                funcName[0] = 0;
                AppendIntegerToString(funcName, s);
            }
#endif

            // Storing Values
            int constant = 0;
            if (ConvertStringToInteger(funcName, &constant)) {
                scriptCode[scriptCodePos++] = SCRIPTVAR_INTCONST;
                scriptCode[scriptCodePos++] = constant;
            }
            else if (funcName[0] == '"') {
                scriptCode[scriptCodePos++] = SCRIPTVAR_STRCONST;
                scriptCode[scriptCodePos++] = StrLength(funcName) - 2;

                int scriptTextPos = 1;
                arrayStrPos       = 0;
                while (scriptTextPos > -1) {
                    switch (arrayStrPos) {
                        case 0:
                            scriptCode[scriptCodePos] = funcName[scriptTextPos] << 24;
                            ++arrayStrPos;
                            break;

                        case 1:
                            scriptCode[scriptCodePos] += funcName[scriptTextPos] << 16;
                            ++arrayStrPos;
                            break;

                        case 2:
                            scriptCode[scriptCodePos] += funcName[scriptTextPos] << 8;
                            ++arrayStrPos;
                            break;

                        case 3:
                            scriptCode[scriptCodePos++] += funcName[scriptTextPos];
                            arrayStrPos = 0;
                            break;

                        default: break;
                    }

                    if (funcName[scriptTextPos] == '"') {
                        if (arrayStrPos > 0)
                            ++scriptCodePos;
                        scriptTextPos = -1;
                    }
                    else {
                        scriptTextPos++;
                    }
                }
            }
            else {
                scriptCode[scriptCodePos++] = SCRIPTVAR_VAR;

                if (arrayStr[0]) {
                    scriptCode[scriptCodePos] = VARARR_ARRAY;

                    if (arrayStr[0] == '+')
                        scriptCode[scriptCodePos] = VARARR_ENTNOPLUS1;

                    if (arrayStr[0] == '-')
                        scriptCode[scriptCodePos] = VARARR_ENTNOMINUS1;

                    ++scriptCodePos;

                    if (arrayStr[0] == '-' || arrayStr[0] == '+') {
                        for (int i = 0; i < StrLength(arrayStr); ++i) arrayStr[i] = arrayStr[i + 1];
                    }

                    if (ConvertStringToInteger(arrayStr, &constant) == 1) {
                        scriptCode[scriptCodePos++] = 0;
                        scriptCode[scriptCodePos++] = constant;
                    }
                    else {
                        if (StrComp(arrayStr, "arrayPos0"))
                            constant = 0;
                        if (StrComp(arrayStr, "arrayPos1"))
                            constant = 1;
                        if (StrComp(arrayStr, "arrayPos2"))
                            constant = 2;
                        if (StrComp(arrayStr, "arrayPos3"))
                            constant = 3;
                        if (StrComp(arrayStr, "arrayPos4"))
                            constant = 4;
                        if (StrComp(arrayStr, "arrayPos5"))
                            constant = 5;
                        if (StrComp(arrayStr, "arrayPos6"))
                            constant = 6;
                        if (StrComp(arrayStr, "arrayPos7"))
                            constant = 7;
                        if (StrComp(arrayStr, "tempObjectPos"))
                            constant = 8;

                        scriptCode[scriptCodePos++] = 1;
                        scriptCode[scriptCodePos++] = constant;
                    }
                }
                else {
                    scriptCode[scriptCodePos++] = VARARR_NONE;
                }

                constant = -1;
                for (int i = 0; i < VAR_MAX_CNT; ++i) {
                    if (StrComp(funcName, variableNames[i]))
                        constant = i;
                }

                if (constant == -1 && Engine.gameMode != ENGINE_SCRIPTERROR) {
                    SetupTextMenu(&gameMenu[0], 0);
                    AddTextMenuEntry(&gameMenu[0], "SCRIPT PARSING FAILED");
                    AddTextMenuEntry(&gameMenu[0], " ");
                    AddTextMenuEntry(&gameMenu[0], "OPERAND NOT FOUND");
                    AddTextMenuEntry(&gameMenu[0], funcName);
                    AddTextMenuEntry(&gameMenu[0], " ");
                    AddTextMenuEntry(&gameMenu[0], "LINE NUMBER");
                    funcName[0] = 0;
                    AppendIntegerToString(funcName, lineID);
                    AddTextMenuEntry(&gameMenu[0], funcName);
                    Engine.gameMode = ENGINE_SCRIPTERROR;
                    constant        = 0;
                }

                scriptCode[scriptCodePos++] = constant;
            }
        }
    }
}
void CheckCaseNumber(char *text)
{
    if (FindStringToken(text, "case", 1) != 0)
        return;

    char caseString[128];
    char caseChar = text[4];

    int textPos    = 5;
    int caseStrPos = 0;
    while (caseChar) {
        if (caseChar != ':')
            caseString[caseStrPos++] = caseChar;
        caseChar = text[textPos++];
    }
    caseString[caseStrPos] = 0;

    bool foundValue = false;

    if (FindStringToken(caseString, "[", 1) >= 0) {
        char caseValue[0x80];
        char arrayStr[0x80];

        int textPos     = 0;
        int funcNamePos = 0;
        int mode        = 0;
        int arrayStrPos = 0;
        while (caseString[textPos] != ':' && caseString[textPos]) {
            if (mode) {
                if (caseString[textPos] == ']')
                    mode = 0;
                else
                    arrayStr[arrayStrPos++] = caseString[textPos];
                ++textPos;
            }
            else {
                if (caseString[textPos] == '[')
                    mode = 1;
                else
                    caseValue[funcNamePos++] = caseString[textPos];
                ++textPos;
            }
        }
        caseValue[funcNamePos] = 0;
        arrayStr[arrayStrPos]  = 0;

        // Eg: temp0 = TypeName[Player Object]
        if (StrComp(caseValue, "TypeName")) {
            caseValue[0] = '0';
            caseValue[1] = 0;

            int o = 0;
            for (; o < OBJECT_COUNT; ++o) {
                if (StrComp(arrayStr, typeNames[o])) {
                    caseValue[0] = 0;
                    AppendIntegerToString(caseValue, o);
                    break;
                }
            }

            if (o == OBJECT_COUNT)
                PrintLog("WARNING: Unknown typename \"%s\", on line %d", arrayStr, lineID);
        }

        // Eg: temp0 = SfxName[Jump]
        if (StrComp(caseValue, "SfxName")) {
            caseValue[0] = '0';
            caseValue[1] = 0;

            int s = 0;
            for (; s < SFX_COUNT; ++s) {
                if (StrComp(arrayStr, sfxNames[s])) {
                    caseValue[0] = 0;
                    AppendIntegerToString(caseValue, s);
                    break;
                }
            }

            if (s == SFX_COUNT)
                PrintLog("WARNING: Unknown sfxName \"%s\", on line %d", arrayStr, lineID);
        }

#if !RETRO_USE_ORIGINAL_CODE
        // Eg: temp0 = VarName[player.lives]
        if (StrComp(caseValue, "VarName")) {
            caseValue[0] = '0';
            caseValue[1] = 0;

            int v = 0;
            for (; v < globalVariablesCount; ++v) {
                if (StrComp(arrayStr, globalVariableNames[v])) {
                    caseValue[0] = 0;
                    AppendIntegerToString(caseValue, v);
                    break;
                }
            }

            if (v == globalVariablesCount) {
                PrintLog("WARNING: Unknown varName \"%s\", on line %d", arrayStr, lineID);
            }
        }

        // Eg: temp0 = AchievementName[Ring King]
        if (StrComp(caseValue, "AchievementName")) {
            caseValue[0] = '0';
            caseValue[1] = 0;

            int a = 0;
            for (; a < achievementCount; ++a) {
                char buf[0x40];
                char *str = achievements[a].name;
                int pos   = 0;

                while (*str) {
                    if (*str != ' ')
                        buf[pos++] = *str;
                    str++;
                }
                buf[pos] = 0;

                if (StrComp(arrayStr, buf)) {
                    caseValue[0] = 0;
                    AppendIntegerToString(caseValue, a);
                    break;
                }
            }

            if (a == achievementCount) {
                PrintLog("WARNING: Unknown AchievementName \"%s\", on line %d", arrayStr, lineID);
            }
        }

        // Eg: temp0 = PlayerName[SONIC]
        if (StrComp(caseValue, "PlayerName")) {
            caseValue[0] = '0';
            caseValue[1] = 0;

            int p = 0;
            for (; p < PLAYER_COUNT; ++p) {
                char buf[0x40];
                char *str = playerNames[p];
                int pos   = 0;

                while (*str) {
                    if (*str != ' ')
                        buf[pos++] = *str;
                    str++;
                }
                buf[pos] = 0;

                if (StrComp(arrayStr, buf)) {
                    caseValue[0] = 0;
                    AppendIntegerToString(caseValue, p);
                    break;
                }
            }

            if (p == PLAYER_COUNT) {
                PrintLog("WARNING: Unknown PlayerName \"%s\", on line %d", arrayStr, lineID);
            }
        }

        // Eg: temp0 = StageName[R - GREEN HILL ZONE 1]
        if (StrComp(caseValue, "StageName")) {
            caseValue[0] = '0';
            caseValue[1] = 0;

            int s = -1;
            if (StrLength(arrayStr) >= 2) {
                char list = arrayStr[0];
                switch (list) {
                    case 'P': list = STAGELIST_PRESENTATION; break;
                    case 'R': list = STAGELIST_REGULAR; break;
                    case 'S': list = STAGELIST_SPECIAL; break;
                    case 'B': list = STAGELIST_BONUS; break;
                }
                s = GetSceneID(list, &arrayStr[2]);
            }

            if (s == -1) {
                PrintLog("WARNING: Unknown StageName \"%s\", on line %d", arrayStr, lineID);
                s = 0;
            }
            caseValue[0] = 0;
            AppendIntegerToString(caseValue, s);
        }
#endif
        StrCopy(caseString, caseValue);
        foundValue = true;
    }

    for (int a = 0; a < scriptValueListCount && !foundValue; ++a) {
        if (StrComp(scriptValueList[a].name, caseString)) {
            StrCopy(caseString, scriptValueList[a].value);
            break;
        }
    }

    int caseID = 0;
    if (ConvertStringToInteger(caseString, &caseID)) {
        int stackValue = jumpTableStack[jumpTableStackPos];
        if (caseID < jumpTable[stackValue])
            jumpTable[stackValue] = caseID;
        stackValue++;
        if (caseID > jumpTable[stackValue])
            jumpTable[stackValue] = caseID;
    }
    else {
        PrintLog("WARNING: unable to convert case string \"%s\" to int, on line %d", caseString, lineID);
    }
}
bool ReadSwitchCase(char *text)
{
    char caseText[0x80];
    if (FindStringToken(text, "case", 1) == 0) {
        int textPos       = 4;
        int caseStringPos = 0;
        while (text[textPos]) {
            if (text[textPos] != ':')
                caseText[caseStringPos++] = text[textPos];
            ++textPos;
        }
        caseText[caseStringPos] = 0;

        bool foundValue = false;
        if (FindStringToken(caseText, "[", 1) >= 0) {
            char caseValue[0x80];
            char arrayStr[0x80];

            int textPos     = 0;
            int funcNamePos = 0;
            int mode        = 0;
            int arrayStrPos = 0;
            while (caseText[textPos] != ':' && caseText[textPos]) {
                if (mode) {
                    if (caseText[textPos] == ']')
                        mode = 0;
                    else
                        arrayStr[arrayStrPos++] = caseText[textPos];
                    ++textPos;
                }
                else {
                    if (caseText[textPos] == '[')
                        mode = 1;
                    else
                        caseValue[funcNamePos++] = caseText[textPos];
                    ++textPos;
                }
            }
            caseValue[funcNamePos] = 0;
            arrayStr[arrayStrPos]  = 0;

            // Eg: temp0 = TypeName[Player Object]
            if (StrComp(caseValue, "TypeName")) {
                caseValue[0] = '0';
                caseValue[1] = 0;

                int o = 0;
                for (; o < OBJECT_COUNT; ++o) {
                    if (StrComp(arrayStr, typeNames[o])) {
                        caseValue[0] = 0;
                        AppendIntegerToString(caseValue, o);
                        break;
                    }
                }

                if (o == OBJECT_COUNT)
                    PrintLog("WARNING: Unknown typename \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = SfxName[Jump]
            if (StrComp(caseValue, "SfxName")) {
                caseValue[0] = '0';
                caseValue[1] = 0;

                int s = 0;
                for (; s < SFX_COUNT; ++s) {
                    if (StrComp(arrayStr, sfxNames[s])) {
                        caseValue[0] = 0;
                        AppendIntegerToString(caseValue, s);
                        break;
                    }
                }

                if (s == SFX_COUNT)
                    PrintLog("WARNING: Unknown sfxName \"%s\", on line %d", arrayStr, lineID);
            }

#if !RETRO_USE_ORIGINAL_CODE
            // Eg: temp0 = VarName[player.lives]
            if (StrComp(caseValue, "VarName")) {
                caseValue[0] = '0';
                caseValue[1] = 0;

                int v = 0;
                for (; v < globalVariablesCount; ++v) {
                    if (StrComp(arrayStr, globalVariableNames[v])) {
                        caseValue[0] = 0;
                        AppendIntegerToString(caseValue, v);
                        break;
                    }
                }

                if (v == globalVariablesCount)
                    PrintLog("WARNING: Unknown varName \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = AchievementName[Ring King]
            if (StrComp(caseValue, "AchievementName")) {
                caseValue[0] = '0';
                caseValue[1] = 0;

                int a = 0;
                for (; a < achievementCount; ++a) {
                    char buf[0x40];
                    char *str = achievements[a].name;
                    int pos   = 0;

                    while (*str) {
                        if (*str != ' ')
                            buf[pos++] = *str;
                        str++;
                    }
                    buf[pos] = 0;

                    if (StrComp(arrayStr, buf)) {
                        caseValue[0] = 0;
                        AppendIntegerToString(caseValue, a);
                        break;
                    }
                }

                if (a == achievementCount)
                    PrintLog("WARNING: Unknown AchievementName \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = PlayerName[SONIC]
            if (StrComp(caseValue, "PlayerName")) {
                caseValue[0] = '0';
                caseValue[1] = 0;

                int p = 0;
                for (; p < PLAYER_COUNT; ++p) {
                    char buf[0x40];
                    char *str = playerNames[p];
                    int pos   = 0;

                    while (*str) {
                        if (*str != ' ')
                            buf[pos++] = *str;
                        str++;
                    }
                    buf[pos] = 0;

                    if (StrComp(arrayStr, buf)) {
                        caseValue[0] = 0;
                        AppendIntegerToString(caseValue, p);
                        break;
                    }
                }

                if (p == PLAYER_COUNT)
                    PrintLog("WARNING: Unknown PlayerName \"%s\", on line %d", arrayStr, lineID);
            }

            // Eg: temp0 = StageName[R - GREEN HILL ZONE 1]
            if (StrComp(caseValue, "StageName")) {
                caseValue[0] = '0';
                caseValue[1] = 0;

                int s = -1;
                if (StrLength(arrayStr) >= 2) {
                    char list = arrayStr[0];
                    switch (list) {
                        case 'P': list = STAGELIST_PRESENTATION; break;
                        case 'R': list = STAGELIST_REGULAR; break;
                        case 'S': list = STAGELIST_SPECIAL; break;
                        case 'B': list = STAGELIST_BONUS; break;
                    }
                    s = GetSceneID(list, &arrayStr[2]);
                }

                if (s == -1) {
                    PrintLog("WARNING: Unknown StageName \"%s\", on line %d", arrayStr, lineID);
                    s = 0;
                }
                caseValue[0] = 0;
                AppendIntegerToString(caseValue, s);
            }
#endif
            StrCopy(caseText, caseValue);
            foundValue = true;
        }

        for (int v = 0; v < scriptValueListCount && !foundValue; ++v) {
            if (StrComp(caseText, scriptValueList[v].name)) {
                StrCopy(caseText, scriptValueList[v].value);
                break;
            }
        }

        int val = 0;

        int jPos    = jumpTableStack[jumpTableStackPos];
        int jOffset = jPos + 4;
        if (ConvertStringToInteger(caseText, &val))
            jumpTable[val - jumpTable[jPos] + jOffset] = scriptCodePos - scriptCodeOffset;
        else
            PrintLog("WARNING: unable to read case string \"%s\" as an int, on line %d", caseText, lineID);

        return true;
    }
    else if (FindStringToken(text, "default", 1) == 0) {
        int jumpTablepos                = jumpTableStack[jumpTableStackPos];
        jumpTable[jumpTablepos + 2] = scriptCodePos - scriptCodeOffset;
        int cnt                         = abs(jumpTable[jumpTablepos + 1] - jumpTable[jumpTablepos]) + 1;

        int jOffset = jumpTablepos + 4;
        for (int i = 0; i < cnt; ++i) {
            if (jumpTable[jOffset + i] < 0)
                jumpTable[jOffset + i] = scriptCodePos - scriptCodeOffset;
        }

        return true;
    }

    return false;
}
void ReadTableValues(char *text)
{
    int textStrPos = 0;

    char valueBuffer[256];
    int valueBufferPos = 0;

    while (text[textStrPos]) {
        valueBuffer[valueBufferPos++] = text[textStrPos++];

        while (text[textStrPos] == ',') {
            valueBuffer[valueBufferPos] = 0;
            ++scriptCode[scriptCodeOffset];
            if (!ConvertStringToInteger(valueBuffer, &scriptCode[scriptCodePos])) {
                scriptCode[scriptCodePos] = 0;
#if !RETRO_USE_ORIGINAL_CODE
                PrintLog("WARNING: unable to parse table value \"%s\" as an int, on line %d", valueBuffer, lineID);
#endif
            }
            scriptCodePos++;
            valueBufferPos = 0;
            textStrPos++;
        }
    }

    if (StrLength(valueBuffer)) {
        valueBuffer[valueBufferPos] = 0;
        ++scriptCode[scriptCodeOffset];
        if (!ConvertStringToInteger(valueBuffer, &scriptCode[scriptCodePos])) {
            scriptCode[scriptCodePos] = 0;
#if !RETRO_USE_ORIGINAL_CODE
            PrintLog("WARNING: unable to parse table value \"%s\" as an int, on line %d", valueBuffer, lineID);
#endif
        }
        scriptCodePos++;
    }
}
void AppendIntegerToString(char *text, int value)
{
    int textPos = 0;
    while (true) {
        if (!text[textPos])
            break;
        ++textPos;
    }

    int cnt = 0;
    int v   = value;
    while (v != 0) {
        v /= 10;
        cnt++;
    }

    v = 0;
    for (int i = cnt - 1; i >= 0; --i) {
        v = value / pow(10, i);
        v %= 10;

        int strValue = v + '0';
        if (strValue < '0' || strValue > '9') {
            // what
        }
        text[textPos++] = strValue;
    }
    if (value == 0)
        text[textPos++] = '0';
    text[textPos] = 0;
}
void AppendIntegerToStringW(ushort *text, int value)
{
    int textPos = 0;
    while (true) {
        if (!text[textPos])
            break;
        ++textPos;
    }

    int cnt = 0;
    int v   = value;
    while (v != 0) {
        v /= 10;
        cnt++;
    }

    v = 0;
    for (int i = cnt - 1; i >= 0; --i) {
        v = value / pow(10, i);
        v %= 10;

        int strValue = v + '0';
        if (strValue < '0' || strValue > '9') {
            // what
        }
        text[textPos++] = strValue;
    }
    if (value == 0)
        text[textPos++] = '0';
    text[textPos] = 0;
}
#endif

bool ConvertStringToInteger(const char *text, int *value)
{
    int charID    = 0;
    bool negative = false;
    int base      = 10;
    *value        = 0;
    if (*text != '+' && !(*text >= '0' && *text <= '9') && *text != '-')
        return false;
    int strLength = StrLength(text) - 1;
    uint charVal  = 0;
    if (*text == '-') {
        negative = true;
        charID   = 1;
        --strLength;
    }
    else if (*text == '+') {
        charID = 1;
        --strLength;
    }

    if (text[charID] == '0') {
        if (text[charID + 1] == 'x' || text[charID + 1] == 'X')
            base = 0x10;
#if !RETRO_USE_ORIGINAL_CODE
        else if (text[charID + 1] == 'b' || text[charID + 1] == 'B')
            base = 0b10;
        else if (text[charID + 1] == 'o' || text[charID + 1] == 'O')
            base = 0010; // base 8
#endif

        if (base != 10) {
            charID += 2;
            strLength -= 2;
        }
    }

    while (strLength > -1) {
        bool flag = text[charID] < '0';
        if (!flag) {
            if (base == 0x10 && text[charID] > 'f')
                flag = true;
#if !RETRO_USE_ORIGINAL_CODE
            if (base == 0010 && text[charID] > '7')
                flag = true;
            if (base == 0b10 && text[charID] > '1')
                flag = true;
#endif
        }

        if (flag) {
            return 0;
        }
        if (strLength <= 0) {
            if (text[charID] >= '0' && text[charID] <= '9') {
                *value = text[charID] + *value - '0';
            }
            else if (text[charID] >= 'a' && text[charID] <= 'f') {
                charVal = text[charID] - 'a';
                charVal += 10;
                *value += charVal;
            }
            else if (text[charID] >= 'A' && text[charID] <= 'F') {
                charVal = text[charID] - 'A';
                charVal += 10;
                *value += charVal;
            }
        }
        else {
            int strlen = strLength + 1;
            charVal    = 0;
            if (text[charID] >= '0' && text[charID] <= '9') {
                charVal = text[charID] - '0';
            }
            else if (text[charID] >= 'a' && text[charID] <= 'f') {
                charVal = text[charID] - 'a';
                charVal += 10;
            }
            else if (text[charID] >= 'A' && text[charID] <= 'F') {
                charVal = text[charID] - 'A';
                charVal += 10;
            }
            for (; --strlen; charVal *= base)
                ;
            *value += charVal;
        }
        --strLength;
        ++charID;
    }

    if (negative)
        *value = -*value;

    return true;
}

#if RETRO_USE_COMPILER
void CopyAliasStr(char *dest, char *text, bool arrayIndex)
{
    int textPos     = 0;
    int destPos     = 0;
    bool arrayValue = false;
    if (arrayIndex) {
        while (text[textPos]) {
            if (arrayValue) {
                if (text[textPos] == ']')
                    arrayValue = false;
                else
                    dest[destPos++] = text[textPos];
                ++textPos;
            }
            else {
                if (text[textPos] == '[')
                    arrayValue = true;
                ++textPos;
            }
        }
    }
    else {
        while (text[textPos]) {
            if (arrayValue) {
                if (text[textPos] == ']')
                    arrayValue = false;
                ++textPos;
            }
            else {
                if (text[textPos] == '[')
                    arrayValue = true;
                else
                    dest[destPos++] = text[textPos];
                ++textPos;
            }
        }
    }
    dest[destPos] = 0;
}
bool CheckOpcodeType(char *text)
{
    while (true) {
        int c = *text;
        if (!*text)
            break;
        ++text;
        if (c == '(')
            return false;
    }
    return true;
}

void ParseScriptFile(char *scriptName, int scriptID)
{
    jumpTableStackPos = 0;
    lineID            = 0;

    for (int f = 0; f < scriptFunctionCount; ++f) {
        if (scriptFunctionList[f].access != ACCESS_PUBLIC)
            StrCopy(scriptFunctionList[f].name, "");
    }

    int newScriptValueCount = COMMON_SCRIPT_VAR_COUNT;
    for (int v = COMMON_SCRIPT_VAR_COUNT; v < scriptValueListCount; ++v) {
        if (scriptValueList[v].access != ACCESS_PUBLIC) {
            StrCopy(scriptValueList[v].name, "");
        }
        else {
            if (newScriptValueCount != v)
                memcpy(&scriptValueList[newScriptValueCount], &scriptValueList[v], sizeof(ScriptVariableInfo));

            newScriptValueCount++;
        }
    }
    scriptValueListCount = newScriptValueCount;

    for (int v = scriptValueListCount; v < SCRIPT_VAR_COUNT; ++v) {
        MEM_ZERO(scriptValueList[v]);
    }

    FileInfo info;
    char scriptPath[0x40];

    // Try the original script folder
    StrCopy(scriptPath, "Data/Scripts/");
    StrAdd(scriptPath, scriptName);
    if (LoadFile(scriptPath, &info)) {
        int readMode   = READMODE_NORMAL;
        int parseMode  = PARSEMODE_SCOPELESS;
        char prevChar  = 0;
        char curChar   = 0;
        int switchDeep = 0;

        while (readMode < READMODE_EOF) {
            int textPos               = 0;
            readMode                  = READMODE_NORMAL;
            bool disableLineIncrement = false;

            while (readMode < READMODE_ENDLINE) {
                prevChar = curChar;
                FileRead(&curChar, 1);
                if (readMode == READMODE_STRING) {
                    if (curChar == '\t' || curChar == '\r' || curChar == '\n' || curChar == ';' || readMode >= READMODE_COMMENTLINE) {
                        if ((curChar == '\n' && prevChar != '\r') || (curChar == '\n' && prevChar == '\r')) {
                            readMode            = READMODE_ENDLINE;
                            scriptText[textPos] = 0;
                            if (curChar == ';')
                                disableLineIncrement = true;
                        }
                    }
                    else if (curChar != '/' || textPos <= 0) {
                        scriptText[textPos++] = curChar;
                        if (curChar == '"')
                            readMode = READMODE_NORMAL;
                    }
                    else if (curChar == '/' && prevChar == '/') {
                        readMode              = READMODE_COMMENTLINE;
                        scriptText[--textPos] = 0;
                    }
                    else {
                        scriptText[textPos++] = curChar;
                    }
                }
                else if (curChar == ' ' || curChar == '\t' || curChar == '\r' || curChar == '\n' || curChar == ';'
                         || readMode >= READMODE_COMMENTLINE) {
                    if ((curChar == '\n' && prevChar != '\r') || (curChar == '\n' && prevChar == '\r') || curChar == ';') {
                        readMode            = READMODE_ENDLINE;
                        scriptText[textPos] = 0;
                        if (curChar == ';')
                            disableLineIncrement = true;
                    }
                }
                else if (curChar != '/' || textPos <= 0) {
                    scriptText[textPos++] = curChar;
                    if (curChar == '"' && !readMode)
                        readMode = READMODE_STRING;
                }
                else if (curChar == '/' && prevChar == '/') {
                    readMode              = READMODE_COMMENTLINE;
                    scriptText[--textPos] = 0;
                }
                else {
                    scriptText[textPos++] = curChar;
                }
                if (ReachedEndOfFile()) {
                    scriptText[textPos] = 0;
                    readMode            = READMODE_EOF;
                }
            }

            switch (parseMode) {
                case PARSEMODE_SCOPELESS:
                    if (!disableLineIncrement)
                        ++lineID;

                    CheckAliasText(scriptText);
                    CheckStaticText(scriptText);

                    if (CheckTableText(scriptText)) {
                        parseMode = PARSEMODE_TABLEREAD;
                        StrCopy(scriptText, "");
                    }

                    if (StrComp(scriptText, "eventObjectUpdate")) {
                        parseMode                                          = PARSEMODE_FUNCTION;
                        objectScriptList[scriptID].eventUpdate.scriptCodePtr = scriptCodePos;
                        objectScriptList[scriptID].eventUpdate.jumpTablePtr  = jumpTablePos;
                        scriptCodeOffset                                   = scriptCodePos;
                        jumpTableOffset                                    = jumpTablePos;
                    }

                    if (StrComp(scriptText, "eventObjectDraw")) {
                        parseMode                                          = PARSEMODE_FUNCTION;
                        objectScriptList[scriptID].eventDraw.scriptCodePtr = scriptCodePos;
                        objectScriptList[scriptID].eventDraw.jumpTablePtr  = jumpTablePos;
                        scriptCodeOffset                                   = scriptCodePos;
                        jumpTableOffset                                    = jumpTablePos;
                    }

                    if (StrComp(scriptText, "eventObjectStartup")) {
                        parseMode                                             = PARSEMODE_FUNCTION;
                        objectScriptList[scriptID].eventStartup.scriptCodePtr = scriptCodePos;
                        objectScriptList[scriptID].eventStartup.jumpTablePtr  = jumpTablePos;
                        scriptCodeOffset                                      = scriptCodePos;
                        jumpTableOffset                                       = jumpTablePos;
                    }

                    if (FindStringToken(scriptText, "reservefunction", 1) == 0) { // forward decl
                        char funcName[0x40];
                        for (textPos = 15; scriptText[textPos]; ++textPos) funcName[textPos - 15] = scriptText[textPos];
                        funcName[textPos - 15] = 0;
                        int funcID             = -1;
                        for (int f = 0; f < scriptFunctionCount; ++f) {
                            if (StrComp(funcName, scriptFunctionList[f].name))
                                funcID = f;
                        }

                        if (scriptFunctionCount < FUNCTION_COUNT && funcID == -1) {
                            StrCopy(scriptFunctionList[scriptFunctionCount++].name, funcName);
                        }
                        else {
                            PrintLog("WARNING: Function %s has already been reserved!", funcName);
                        }

                        parseMode = PARSEMODE_SCOPELESS;
                    }
                    else if (FindStringToken(scriptText, "publicfunction", 1) == 0) { // regular public decl
                        char funcName[0x40];
                        for (textPos = 14; scriptText[textPos]; ++textPos) funcName[textPos - 14] = scriptText[textPos];

                        funcName[textPos - 14] = 0;
                        int funcID             = -1;
                        for (int f = 0; f < scriptFunctionCount; ++f) {
                            if (StrComp(funcName, scriptFunctionList[f].name))
                                funcID = f;
                        }

                        if (funcID <= -1) {
                            if (scriptFunctionCount >= FUNCTION_COUNT) {
                                parseMode = PARSEMODE_SCOPELESS;
                            }
                            else {
                                StrCopy(scriptFunctionList[scriptFunctionCount].name, funcName);
                                scriptFunctionList[scriptFunctionCount].access            = ACCESS_PUBLIC;
                                scriptFunctionList[scriptFunctionCount].ptr.scriptCodePtr = scriptCodePos;
                                scriptFunctionList[scriptFunctionCount].ptr.jumpTablePtr  = jumpTablePos;

                                scriptCodeOffset = scriptCodePos;
                                jumpTableOffset  = jumpTablePos;
                                parseMode        = PARSEMODE_FUNCTION;
                                ++scriptFunctionCount;
                            }
                        }
                        else {
                            StrCopy(scriptFunctionList[funcID].name, funcName);
                            scriptFunctionList[funcID].access            = ACCESS_PUBLIC;
                            scriptFunctionList[funcID].ptr.scriptCodePtr = scriptCodePos;
                            scriptFunctionList[funcID].ptr.jumpTablePtr  = jumpTablePos;

                            scriptCodeOffset = scriptCodePos;
                            jumpTableOffset  = jumpTablePos;
                            parseMode        = PARSEMODE_FUNCTION;
                        }
                    }
                    else if (FindStringToken(scriptText, "privatefunction", 1) == 0) { // regular private decl
                        char funcName[0x40];
                        for (textPos = 15; scriptText[textPos]; ++textPos) funcName[textPos - 15] = scriptText[textPos];

                        funcName[textPos - 15] = 0;
                        int funcID             = -1;
                        for (int f = 0; f < scriptFunctionCount; ++f) {
                            if (StrComp(funcName, scriptFunctionList[f].name))
                                funcID = f;
                        }

                        if (funcID <= -1) {
                            if (scriptFunctionCount >= FUNCTION_COUNT) {
                                parseMode = PARSEMODE_SCOPELESS;
                            }
                            else {
                                StrCopy(scriptFunctionList[scriptFunctionCount].name, funcName);
                                scriptFunctionList[scriptFunctionCount].access            = ACCESS_PRIVATE;
                                scriptFunctionList[scriptFunctionCount].ptr.scriptCodePtr = scriptCodePos;
                                scriptFunctionList[scriptFunctionCount].ptr.jumpTablePtr  = jumpTablePos;

                                scriptCodeOffset = scriptCodePos;
                                jumpTableOffset  = jumpTablePos;
                                parseMode        = PARSEMODE_FUNCTION;
                                ++scriptFunctionCount;
                            }
                        }
                        else {
                            StrCopy(scriptFunctionList[funcID].name, funcName);
                            scriptFunctionList[funcID].access            = ACCESS_PRIVATE;
                            scriptFunctionList[funcID].ptr.scriptCodePtr = scriptCodePos;
                            scriptFunctionList[funcID].ptr.jumpTablePtr  = jumpTablePos;

                            scriptCodeOffset = scriptCodePos;
                            jumpTableOffset  = jumpTablePos;
                            parseMode        = PARSEMODE_FUNCTION;
                        }
                    }
                    break;

                case PARSEMODE_PLATFORMSKIP:
                    if (!disableLineIncrement)
                        ++lineID;

                    if (FindStringToken(scriptText, "#endplatform", 1) == 0)
                        parseMode = PARSEMODE_FUNCTION;
                    break;

                case PARSEMODE_FUNCTION:
                    if (!disableLineIncrement)
                        ++lineID;

                    if (scriptText[0]) {
                        if (StrComp(scriptText, "endevent")) {
                            scriptCode[scriptCodePos++] = FUNC_END;
                            parseMode                   = PARSEMODE_SCOPELESS;
                        }
                        else if (StrComp(scriptText, "endfunction")) {
                            scriptCode[scriptCodePos++] = FUNC_RETURN;
                            parseMode                   = PARSEMODE_SCOPELESS;
                        }
                        else if (FindStringToken(scriptText, "#platform:", 1) == 0) {
                            if (FindStringToken(scriptText, Engine.gamePlatform, 1) == -1
                                && FindStringToken(scriptText, Engine.gameRenderType, 1) == -1
#if RETRO_USE_HAPTICS
                                && FindStringToken(scriptText, Engine.gameHapticSetting, 1) == -1
#endif
#if !RETRO_USE_ORIGINAL_CODE && RETRO_REV03
                                && FindStringToken(scriptText, Engine.releaseType, 1) == -1 // general flag for standalone/origins content switching
#endif
#if !RETRO_USE_ORIGINAL_CODE
                                && FindStringToken(scriptText, "USE_DECOMP", 1) == -1 // general flag for decomp-only stuff
#endif
#if RETRO_USE_NETWORKING
                                && FindStringToken(scriptText, "USE_NETWORKING", 1) == -1
#endif
#if RETRO_USE_MOD_LOADER
                                && FindStringToken(scriptText, "USE_MOD_LOADER", 1) == -1
#endif
                            ) {
                                parseMode = PARSEMODE_PLATFORMSKIP;
                            }
                        }
                        else if (FindStringToken(scriptText, "#endplatform", 1) == -1) {
                            ConvertConditionalStatement(scriptText);
                            if (ConvertSwitchStatement(scriptText)) {
                                parseMode    = PARSEMODE_SWITCHREAD;
                                info.readPos = (int)GetFilePosition();
                                switchDeep   = 0;
                            }
                            ConvertArithmaticSyntax(scriptText);
                            if (!ReadSwitchCase(scriptText)) {
                                ConvertFunctionText(scriptText);
                                if (Engine.gameMode == ENGINE_SCRIPTERROR) {
                                    AddTextMenuEntry(&gameMenu[0], " ");
                                    AddTextMenuEntry(&gameMenu[0], "ERROR IN");
                                    AddTextMenuEntry(&gameMenu[0], scriptName);
                                    parseMode = PARSEMODE_ERROR;
                                }
                            }
                        }
                    }
                    break;

                case PARSEMODE_SWITCHREAD:
                    if (FindStringToken(scriptText, "switch", 1) == 0)
                        ++switchDeep;

                    if (switchDeep) {
                        if (FindStringToken(scriptText, "endswitch", 1) == 0)
                            --switchDeep;
                    }
                    else if (FindStringToken(scriptText, "endswitch", 1) == 0) {
                        SetFilePosition(info.readPos);
                        parseMode  = PARSEMODE_FUNCTION;
                        int jPos   = jumpTableStack[jumpTableStackPos];
                        switchDeep = abs(jumpTable[jPos + 1] - jumpTable[jPos]) + 1;
                        for (textPos = 0; textPos < switchDeep; ++textPos) jumpTable[jumpTablePos++] = -1;
                    }
                    else {
                        CheckCaseNumber(scriptText);
                    }
                    break;

                case PARSEMODE_TABLEREAD:
                    if (!disableLineIncrement)
                        ++lineID;

                    if (FindStringToken(scriptText, "endtable", 1) == 0) {
                        parseMode = PARSEMODE_SCOPELESS;
                    }
                    else {
                        if (StrLength(scriptText) >= 1)
                            ReadTableValues(scriptText);

                        parseMode = PARSEMODE_TABLEREAD;
                    }
                    break;

                default: break;
            }
        }

        CloseFile();
    }
}
#endif

#if RETRO_PLATFORM == RETRO_PS1
// Script images (docs/30 phase 9.3, golden rule): what LoadBytecode leaves in the PS1's script arrays after a stage's
// bytecode (GlobalCode + the stage's own) is written at build time by the host build of this same code
// (tools/rsdkmanifest: right after the stage's LoadBytecode, before any startup writes into the tables) as
// Data/Stages/<stage>/Scripts.ps1, and the console reads it straight into the arrays: no bytecode decoding at a stage load
// (~50 vblanks a zone), and GlobalCode.bin isn't read. The stage loads are transfer-bound, so the arrays are stored in
// runs (95 % of the code slots fit a byte, 99 % of the jump entries 16 bits): an int16 image was 1.7x the bytecode and
// saved nothing. File: "PSC2", then int32 code slots, jump table entries, big values, object scripts (from id 1),
// functions; the code slots and the jump table as runs (u16 header: bit 15 = wide, bits 0-14 = count; then count u8 /
// int16 slots, or count u16 / int32 entries, each run padded to 2 bytes); the big-value table (pos, value); each object
// script's 3 ScriptPtrs; each function's ScriptPtr (all little endian, as the engine's own arrays). Expanding the runs is
// a copy loop (~2 vblanks), not the bytecode's per-byte decode.
static int s_ps1ScriptEnd   = 1; // one past the last object script id LoadBytecode filled (since ClearScriptData)
static int s_ps1FunctionEnd = 0; // one past the last function it filled
static bool s_ps1ImageLoaded = false;
volatile uint32_t g_ps1ScriptImages = 0, g_ps1ScriptImageBad = 0; // images loaded / refused (GDB)
volatile uint32_t g_ps1ScriptImageOn = 1; // GDB A/B: 0 = decode the bytecode (initialised data: set before the game starts)
static void PS1ScriptImagePath(char *path)
{
    StrCopy(path, "Data/Stages/");
    StrAdd(path, stageList[activeStageList][stageListPosition].folder);
    StrAdd(path, "/Scripts.ps1");
}
#if defined(RETRO_PS1_HOST_TOOL)
// Runs of narrow (fits `narrow` unsigned bytes) / wide values: see the format above.
template <typename T> static bool PS1WriteRuns(FILE *f, const T *v, int count, uint32_t narrowMax)
{
    for (int i = 0; i < count;) {
        bool wide = (uint32_t)v[i] > narrowMax || v[i] < 0;
        int n     = 1;
        while (i + n < count && n < 0x7FFF && (((uint32_t)v[i + n] > narrowMax || v[i + n] < 0) == wide)) ++n;
        uint16_t h = (uint16_t)(n | (wide ? 0x8000 : 0));
        if (fwrite(&h, 2, 1, f) != 1)
            return false;
        for (int k = 0; k < n; ++k) {
            T x = v[i + k];
            if (wide ? fwrite(&x, sizeof(T), 1, f) != 1 : (narrowMax == 0xFF ? fputc((int)x, f) == EOF : fwrite(&x, 2, 1, f) != 1))
                return false;
        }
        if (!wide && narrowMax == 0xFF && (n & 1) && fputc(0, f) == EOF) // pad to 2 bytes
            return false;
        i += n;
    }
    return true;
}
static void PS1WriteScriptImage()
{
    char path[0x80];
    PS1ScriptImagePath(path);
    FILE *f = fopen(path, "wb");
    int head[5] = { scriptCodePos, jumpTablePos, s_ps1ScriptBigCount, s_ps1ScriptEnd - 1, s_ps1FunctionEnd };
    bool ok = f && fwrite("PSC2", 1, 4, f) == 4 && fwrite(head, 4, 5, f) == 5
              && PS1WriteRuns<short>(f, scriptCode.slot, scriptCodePos, 0xFF)
              && PS1WriteRuns<int>(f, jumpTable, jumpTablePos, 0xFFFF)
              && fwrite(s_ps1ScriptBig, sizeof(PS1ScriptBig), s_ps1ScriptBigCount, f) == (size_t)s_ps1ScriptBigCount;
    for (int s = 1; ok && s < s_ps1ScriptEnd; ++s) {
        const ObjectScript &o = objectScriptList[s];
        ok = fwrite(&o.eventUpdate, sizeof(ScriptPtr), 1, f) == 1 && fwrite(&o.eventDraw, sizeof(ScriptPtr), 1, f) == 1
             && fwrite(&o.eventStartup, sizeof(ScriptPtr), 1, f) == 1;
    }
    for (int fn = 0; ok && fn < s_ps1FunctionEnd; ++fn) ok = fwrite(&scriptFunctionList[fn].ptr, sizeof(ScriptPtr), 1, f) == 1;
    if (f)
        fclose(f);
    if (!ok) {
        perror(path);
        exit(1);
    }
}
#else
// The runs (see above) expanded into `dst` in place, no buffer: a narrow run's n values (u8 code slots / u16 jump entries)
// are read into the upper half of their own span, then widened from the front (value k lands at bytes before the
// unread value k + 1). An odd u8 run's pad byte lands in the next slot, which the next run (or the cleared tail) owns.
template <typename T> static bool PS1ReadRuns(T *dst, int count, bool bytes)
{
    for (int i = 0; i < count;) {
        uint16_t h;
        FileRead(&h, 2);
        int n = h & 0x7FFF;
        if (!n || i + n > count)
            return false;
        if (h & 0x8000) {
            FileRead(&dst[i], n * (int)sizeof(T));
        }
        else if (bytes) {
            uint8_t *src = (uint8_t *)&dst[i] + n * sizeof(T) - n;
            FileRead(src, n + (n & 1));
            for (int k = 0; k < n; ++k) dst[i + k] = (T)src[k];
        }
        else {
            uint8_t *src = (uint8_t *)&dst[i] + n * sizeof(T) - 2 * n;
            FileRead(src, 2 * n);
            for (int k = 0; k < n; ++k) dst[i + k] = (T)(src[2 * k] | src[2 * k + 1] << 8);
        }
        i += n;
    }
    return true;
}

// The stage's image, if the disc has one: loads the whole script state (GlobalCode's and the stage's), and the stage's own
// LoadBytecode call that follows does nothing. Any size past the PS1 arrays refuses it (decoded as before).
static bool PS1LoadScriptImage()
{
    if (s_ps1ImageLoaded)
        return true;
    if (!g_ps1ScriptImageOn)
        return false;
    char path[0x80];
    PS1ScriptImagePath(path);
    FileInfo info;
    if (!LoadFile(path, &info))
        return false;
    char tag[4];
    int head[5];
    FileRead(tag, 4);
    FileRead(head, sizeof(head));
    const int codePos = head[0], jumpPos = head[1], bigCount = head[2], scripts = head[3], fns = head[4];
    if (memcmp(tag, "PSC2", 4) || (uint)codePos > SCRIPTCODE_COUNT - 1 || (uint)jumpPos > JUMPTABLE_COUNT - 1
        || (uint)bigCount > PS1_SCRIPTBIG_COUNT || (uint)scripts > OBJECT_COUNT - 1 || (uint)fns > FUNCTION_COUNT
        || !PS1ReadRuns<short>(scriptCode.slot, codePos, true) || !PS1ReadRuns<int>(jumpTable, jumpPos, false)) {
        g_ps1ScriptImageBad = g_ps1ScriptImageBad + 1;
        CloseFile();
        return false; // (the arrays are cleared again by the next stage load; this one decodes)
    }
    FileRead(s_ps1ScriptBig, bigCount * (int)sizeof(PS1ScriptBig));
    for (int s = 1; s <= scripts; ++s) {
        ScriptPtr p[3];
        FileRead(p, sizeof(p));
        objectScriptList[s].eventUpdate  = p[0];
        objectScriptList[s].eventDraw    = p[1];
        objectScriptList[s].eventStartup = p[2];
    }
    for (int fn = 0; fn < fns; ++fn) FileRead(&scriptFunctionList[fn].ptr, sizeof(ScriptPtr));
    CloseFile();
    scriptCodePos       = codePos;
    jumpTablePos        = jumpPos;
    s_ps1ScriptBigCount = bigCount;
    PS1ScriptBigChanged();
    s_ps1ScriptEnd   = scripts + 1;
    s_ps1FunctionEnd = fns;
    s_ps1ImageLoaded = true;
    g_ps1ScriptImages = g_ps1ScriptImages + 1;
    return true;
}
#endif
#endif

void LoadBytecode(int stageListID, int scriptID)
{
#if RETRO_PLATFORM == RETRO_PS1 && !defined(RETRO_PS1_HOST_TOOL)
    if (PS1LoadScriptImage())
        return;
#endif
    char scriptPath[0x40];
    switch (stageListID) {
        case STAGELIST_PRESENTATION:
        case STAGELIST_REGULAR:
        case STAGELIST_BONUS:
        case STAGELIST_SPECIAL:
            StrCopy(scriptPath, "Bytecode/");
            StrAdd(scriptPath, stageList[stageListID][stageListPosition].folder);
            StrAdd(scriptPath, ".bin");
            break;
        case 4: StrCopy(scriptPath, "Bytecode/GlobalCode.bin"); break;
        default: break;
    }

    FileInfo info;
    if (LoadFile(scriptPath, &info)) {
        byte fileBuffer    = 0;
#if RETRO_PLATFORM != RETRO_PS1
        int *scriptCodePtr = &scriptCode[scriptCodePos];
#endif
        int *jumpTablePtr  = &jumpTable[jumpTablePos];

        FileRead(&fileBuffer, 1);
        int scriptCodeSize = fileBuffer;
        FileRead(&fileBuffer, 1);
        scriptCodeSize |= fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        scriptCodeSize |= fileBuffer << 16;
        FileRead(&fileBuffer, 1);
        scriptCodeSize |= fileBuffer << 24;
#if RETRO_PLATFORM == RETRO_PS1
        if (scriptCodePos + scriptCodeSize > SCRIPTCODE_COUNT - 1) { // PS1-sized array (Script.hpp); last slot = the 0 "no sub" sentinel
            g_ps1ScriptOverflow = g_ps1ScriptOverflow + 1;
            CloseFile();
            return;
        }
        while (scriptCodeSize > 0) { // as below, each int into an int16 slot or the big-value table
            FileRead(&fileBuffer, 1);
            int blockSize = fileBuffer & 0x7F;
            bool wide     = fileBuffer >= 0x80;
            while (blockSize > 0) {
                int value = 0;
                if (wide) {
                    FileRead(&fileBuffer, 1);
                    value = fileBuffer;
                    FileRead(&fileBuffer, 1);
                    value |= fileBuffer << 8;
                    FileRead(&fileBuffer, 1);
                    value |= fileBuffer << 16;
                    FileRead(&fileBuffer, 1);
                    value |= fileBuffer << 24;
                }
                else {
                    FileRead(&fileBuffer, 1);
                    value = fileBuffer;
                }
                if (!PS1ScriptStore(scriptCodePos, value)) {
                    g_ps1ScriptOverflow = g_ps1ScriptOverflow + 1;
                    CloseFile();
                    return;
                }
                ++scriptCodePos;
                --scriptCodeSize;
                --blockSize;
            }
        }
#else
        while (scriptCodeSize > 0) {
            FileRead(&fileBuffer, 1);
            int blockSize = fileBuffer & 0x7F;
            if (fileBuffer >= 0x80) {
                while (blockSize > 0) {
                    FileRead(&fileBuffer, 1);
                    *scriptCodePtr = fileBuffer;
                    FileRead(&fileBuffer, 1);
                    *scriptCodePtr |= fileBuffer << 8;
                    FileRead(&fileBuffer, 1);
                    *scriptCodePtr |= fileBuffer << 16;
                    FileRead(&fileBuffer, 1);
                    *scriptCodePtr |= fileBuffer << 24;

                    ++scriptCodePtr;
                    ++scriptCodePos;
                    --scriptCodeSize;
                    --blockSize;
                }
            }
            else {
                while (blockSize > 0) {
                    FileRead(&fileBuffer, 1);
                    *scriptCodePtr = fileBuffer;

                    ++scriptCodePtr;
                    ++scriptCodePos;
                    --scriptCodeSize;
                    --blockSize;
                }
            }
        }

#endif

        FileRead(&fileBuffer, 1);
        int jumpTableSize = fileBuffer;
        FileRead(&fileBuffer, 1);
        jumpTableSize |= fileBuffer << 8;
        FileRead(&fileBuffer, 1);
        jumpTableSize |= fileBuffer << 16;
        FileRead(&fileBuffer, 1);
        jumpTableSize |= fileBuffer << 24;
#if RETRO_PLATFORM == RETRO_PS1
        if (jumpTablePos + jumpTableSize > JUMPTABLE_COUNT - 1) {
            g_ps1ScriptOverflow = g_ps1ScriptOverflow + 1;
            CloseFile();
            return;
        }
#endif

        while (jumpTableSize > 0) {
            FileRead(&fileBuffer, 1);
            int blockSize = fileBuffer & 0x7F;

            if (fileBuffer >= 0x80) {
                while (blockSize > 0) {
                    FileRead(&fileBuffer, 1);
                    *jumpTablePtr = fileBuffer;
                    FileRead(&fileBuffer, 1);
                    *jumpTablePtr |= fileBuffer << 8;
                    FileRead(&fileBuffer, 1);
                    *jumpTablePtr |= fileBuffer << 16;
                    FileRead(&fileBuffer, 1);
                    *jumpTablePtr |= fileBuffer << 24;

                    ++jumpTablePtr;
                    ++jumpTablePos;
                    --jumpTableSize;
                    --blockSize;
                }
            }
            else {
                while (blockSize > 0) {
                    FileRead(&fileBuffer, 1);
                    *jumpTablePtr = fileBuffer;

                    ++jumpTablePtr;
                    ++jumpTablePos;
                    --jumpTableSize;
                    --blockSize;
                }
            }
        }

        FileRead(&fileBuffer, 1);
        int scriptCount = fileBuffer;
        FileRead(&fileBuffer, 1);
        scriptCount |= fileBuffer << 8;

        for (int s = 0; s < scriptCount; ++s) {
            ObjectScript *script = &objectScriptList[scriptID + s];

            FileRead(&fileBuffer, 1);
            script->eventUpdate.scriptCodePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            script->eventUpdate.scriptCodePtr |= fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            script->eventUpdate.scriptCodePtr |= fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            script->eventUpdate.scriptCodePtr |= fileBuffer << 24;

            FileRead(&fileBuffer, 1);
            script->eventDraw.scriptCodePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            script->eventDraw.scriptCodePtr |= fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            script->eventDraw.scriptCodePtr |= fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            script->eventDraw.scriptCodePtr |= fileBuffer << 24;

            FileRead(&fileBuffer, 1);
            script->eventStartup.scriptCodePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            script->eventStartup.scriptCodePtr |= (fileBuffer << 8);
            FileRead(&fileBuffer, 1);
            script->eventStartup.scriptCodePtr |= (fileBuffer << 16);
            FileRead(&fileBuffer, 1);
            script->eventStartup.scriptCodePtr |= fileBuffer << 24;
        }

        for (int s = 0; s < scriptCount; ++s) {
            ObjectScript *script = &objectScriptList[scriptID + s];

            FileRead(&fileBuffer, 1);
            script->eventUpdate.jumpTablePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            script->eventUpdate.jumpTablePtr |= fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            script->eventUpdate.jumpTablePtr |= fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            script->eventUpdate.jumpTablePtr |= fileBuffer << 24;

            FileRead(&fileBuffer, 1);
            script->eventDraw.jumpTablePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            script->eventDraw.jumpTablePtr |= fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            script->eventDraw.jumpTablePtr |= fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            script->eventDraw.jumpTablePtr |= fileBuffer << 24;

            FileRead(&fileBuffer, 1);
            script->eventStartup.jumpTablePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            script->eventStartup.jumpTablePtr |= fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            script->eventStartup.jumpTablePtr |= fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            script->eventStartup.jumpTablePtr |= fileBuffer << 24;
        }

        FileRead(&fileBuffer, 1);
        int functionCount = fileBuffer;
        FileRead(&fileBuffer, 1);
        functionCount |= fileBuffer << 8;

        for (int f = 0; f < functionCount; ++f) {
            ScriptFunction *function = &scriptFunctionList[f];

            FileRead(&fileBuffer, 1);
            function->ptr.scriptCodePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            function->ptr.scriptCodePtr |= fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            function->ptr.scriptCodePtr |= fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            function->ptr.scriptCodePtr |= fileBuffer << 24;
        }

        for (int f = 0; f < functionCount; ++f) {
            ScriptFunction *function = &scriptFunctionList[f];

            FileRead(&fileBuffer, 1);
            function->ptr.jumpTablePtr = fileBuffer;
            FileRead(&fileBuffer, 1);
            function->ptr.jumpTablePtr |= fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            function->ptr.jumpTablePtr |= fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            function->ptr.jumpTablePtr |= fileBuffer << 24;
        }

#if RETRO_PLATFORM == RETRO_PS1
        for (int sc = 0; sc < scriptCount; ++sc) {
            ObjectScript *script = &objectScriptList[scriptID + sc];
            PS1FixScriptPtr(&script->eventUpdate);
            PS1FixScriptPtr(&script->eventDraw);
            PS1FixScriptPtr(&script->eventStartup);
        }
        for (int f = 0; f < functionCount; ++f) PS1FixScriptPtr(&scriptFunctionList[f].ptr);
        if (scriptID + scriptCount > s_ps1ScriptEnd)
            s_ps1ScriptEnd = scriptID + scriptCount;
        if (functionCount > s_ps1FunctionEnd)
            s_ps1FunctionEnd = functionCount;
#endif

        CloseFile();
#if defined(RETRO_PS1_HOST_TOOL)
        if (stageListID != 4) // the stage's own bytecode: the state the console's image must hold
            PS1WriteScriptImage();
#endif
    }
}

#if RETRO_PLATFORM == RETRO_PS1
static byte s_ps1OpSize[FUNC_MAX_CNT]; // the operand count of each opcode (functions[].opcodeSize)
#endif
void ClearScriptData()
{
#if RETRO_PLATFORM == RETRO_PS1
    for (int f = 0; f < FUNC_MAX_CNT; ++f) s_ps1OpSize[f] = functions[f].opcodeSize;
#endif
#if RETRO_PLATFORM == RETRO_PS1
    memset(&scriptCode, 0, sizeof(scriptCode));
    s_ps1ScriptBigCount = 0;
    PS1ScriptBigChanged();
    s_ps1ScriptEnd   = 1;
    s_ps1FunctionEnd = 0;
    s_ps1ImageLoaded = false;
#else
    memset(scriptCode, 0, sizeof(scriptCode));
#endif
    memset(jumpTable, 0, sizeof(jumpTable));

    memset(foreachStack, -1, sizeof(foreachStack));
    memset(jumpTableStack, 0, sizeof(jumpTableStack));
    memset(functionStack, 0, sizeof(functionStack));

    scriptFrameCount = 0;

    scriptCodePos     = 0;
    jumpTablePos      = 0;
    jumpTableStackPos = 0;
    functionStackPos  = 0;

    scriptCodePos    = 0;
    scriptCodeOffset = 0;
    jumpTablePos     = 0;
    jumpTableOffset  = 0;

#if RETRO_USE_COMPILER
    scriptFunctionCount = 0;

    lineID = 0;

    scriptValueListCount = COMMON_SCRIPT_VAR_COUNT;
    for (int v = COMMON_SCRIPT_VAR_COUNT; v < SCRIPT_VAR_COUNT; ++v) {
        MEM_ZERO(scriptValueList[v]);
    }
#endif

    ClearAnimationData();

    for (int o = 0; o < OBJECT_COUNT; ++o) {
        ObjectScript *scriptInfo               = &objectScriptList[o];
        scriptInfo->eventUpdate.scriptCodePtr  = SCRIPTCODE_COUNT - 1;
        scriptInfo->eventUpdate.jumpTablePtr   = JUMPTABLE_COUNT - 1;
        scriptInfo->eventDraw.scriptCodePtr    = SCRIPTCODE_COUNT - 1;
        scriptInfo->eventDraw.jumpTablePtr     = JUMPTABLE_COUNT - 1;
        scriptInfo->eventStartup.scriptCodePtr = SCRIPTCODE_COUNT - 1;
        scriptInfo->eventStartup.jumpTablePtr  = JUMPTABLE_COUNT - 1;
        scriptInfo->frameListOffset            = 0;
        scriptInfo->spriteSheetID              = 0;
        scriptInfo->animFile                   = GetDefaultAnimationRef();
        typeNames[o][0]                        = 0;
    }

    for (int s = globalSFXCount; s < globalSFXCount + stageSFXCount; ++s) {
        sfxNames[s][0] = 0;
    }

    for (int f = 0; f < FUNCTION_COUNT; ++f) {
        scriptFunctionList[f].ptr.scriptCodePtr = SCRIPTCODE_COUNT - 1;
        scriptFunctionList[f].ptr.jumpTablePtr  = JUMPTABLE_COUNT - 1;
    }

    SetObjectTypeName("Blank Object", OBJ_TYPE_BLANKOBJECT);
}

#if RETRO_PLATFORM == RETRO_PS1
// Sonic 2's Ring update sub (GlobalCode object "Ring"), natively (docs/30 CPU work): the most common object, ~21
// VM instructions a frame each (4.6 hblanks on the emulator; 11-16 on screen in several zones). The patcher
// (tools/scripts/patch_bytecode.py) replaces the sub with `PS1Ring / End` only when its instructions match this
// transcription exactly, constants included. Every statement below is one script instruction, in order, with the
// same variables (this = objectEntityPos, p = arrayPos6: each player of group 256), so temps, checkResult, array
// positions and every entity / global write end as the script leaves them.
static void PS1ResetObjectEntity(int slot, int type, int prop, int x, int y) // FUNC_RESETOBJECTENTITY's body
{
    Entity *newEnt = &PS1_OBJ(slot);
    memset(newEnt, 0, sizeof(Entity));
    newEnt->type               = type;
    newEnt->propertyValue      = prop;
    newEnt->xpos               = x;
    newEnt->ypos               = y;
    newEnt->direction          = FLIP_NONE;
    newEnt->priority           = PRIORITY_BOUNDS;
    newEnt->drawOrder          = 3;
    newEnt->scale              = 512;
    newEnt->inkEffect          = INK_NONE;
    newEnt->objectInteractions = true;
    newEnt->visible            = true;
    newEnt->tileCollisions     = true;
}
static void PS1RingUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr = scriptEng.checkResult;
    int *G  = globalVariables;
    int self = objectEntityPos;
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) { // ForEachActive 256, ARRAYPOS6
        ap[6]    = players.entityRefs[loop];
        Entity &me = PS1_OBJ(self);
        cr   = PS1_OBJ(0).state == 27; // CheckEqual OBJECTSTATE[0], 27
        t[0] = cr;
        cr   = PS1_OBJ(ap[6]).state == 27;
        t[0] = cr;
        cr   = PS1_OBJ(ap[6]).state == 26;
        t[0] |= cr;
        if (t[0] == 0) {
            TouchCollision(&PS1_OBJ(self), -8, -8, 8, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 1) {
                me.type      = 12;
                me.drawOrder = PS1_OBJ(ap[6]).values[18];
                if (PS1_OBJ(ap[6]).values[16] == 1) {
                    PS1_OBJ(0).values[0]++;
                    if (PS1_OBJ(0).values[0] > 999)
                        PS1_OBJ(0).values[0] = 999;
                }
                else {
                    PS1_OBJ(ap[6]).values[0]++;
                    if (PS1_OBJ(ap[6]).values[0] > 999)
                        PS1_OBJ(ap[6]).values[0] = 999;
                }
                if (PS1_OBJ(0).values[0] >= G[21]) {
                    if (G[0] != 2) {
                        G[25]++;
                        PlaySfx(24, 0);
                        PauseSound();
                        PS1ResetObjectEntity(25, 34, 2, 0, 0);
                        PS1_OBJ(25).priority = 1;
                    }
                    G[21] += 100;
                    if (G[21] > 300)
                        G[21] = 1000;
                }
                if (G[13] == 1) {
                    if (G[108] == 0) {
                        if (ap[6] == 0)
                            G[119]++;
                        else
                            G[125]++;
                    }
                    else {
                        if (ap[6] == 1)
                            G[119]++;
                        else
                            G[125]++;
                    }
                }
                if (G[20] == 0) {
                    PlaySfx(1, 0);
                    SetSfxAttributes(1, -1, -100);
                    G[20] = 1;
                }
                else {
                    PlaySfx(2, 0);
                    SetSfxAttributes(2, -1, 100);
                    G[20] = 0;
                }
            }
            else {
                if (PS1_OBJ(self).state == 0) {
                    if (PS1_OBJ(ap[6]).values[37] == 4) {
                        TouchCollision(&PS1_OBJ(self), -64, -64, 64, 64, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                        if (cr == 1) {
                            PS1_OBJ(self).state     = 1;
                            PS1_OBJ(self).values[1] = ap[6];
                        }
                    }
                }
            }
        }
    }
    Entity &me = PS1_OBJ(self);
    if (me.state == 1) {
        ap[0] = me.values[1];
        if (PS1_OBJ(ap[0]).values[37] != 4) {
            me.type           = 11;
            me.animationSpeed = 128;
            me.alpha          = 256;
        }
        else {
            ap[0] = me.values[1];
            if (me.xpos > PS1_OBJ(ap[0]).xpos) {
                if (me.xvel > 0)
                    me.xvel -= 49152;
                else
                    me.xvel -= 12288;
            }
            else {
                if (me.xvel < 0)
                    me.xvel += 49152;
                else
                    me.xvel += 12288;
            }
            if (me.ypos > PS1_OBJ(ap[0]).ypos) {
                if (me.yvel > 0)
                    me.yvel -= 49152;
                else
                    me.yvel -= 12288;
            }
            else {
                if (me.yvel < 0)
                    me.yvel += 49152;
                else
                    me.yvel += 12288;
            }
            me.xpos += me.xvel;
            me.ypos += me.yvel;
        }
    }
}
#if PS1_GAME == 1
// CallNativeFunction2 op0 op1 op2 (FUNC_CALLNATIVEFUNCTION2's body, operands as locals: nothing reads them back).
static void PS1CallNative2(int op0, int op1, int op2)
{
    if (op0 >= 0 && op0 < NATIIVEFUNCTION_COUNT) {
        if (StrLength(scriptText)) {
            void (*func)(int *, char *) = (void (*)(int *, char *))nativeFunction[op0];
            if (func)
                func(&op2, scriptText);
        }
        else {
            void (*func)(int *, int *) = (void (*)(int *, int *))nativeFunction[op0];
            if (func)
                func(&op1, &op2);
        }
    }
}
#if PS1_GAME == 1
// TouchCollision(me, l, t, r, b, o, C_BOX x4) for Sonic 1's natives, whose own box is always given (constants): the
// same test and result without GetHitbox(me) (read by upstream, never used when the values are explicit) or the call;
// with the debug hitboxes on, the real one (it records them).
static inline void PS1TouchS1(Entity *me, int l, int t, int r, int b, Entity *o)
{
#if !RETRO_USE_ORIGINAL_CODE
    if (showHitboxes) {
        TouchCollision(me, l, t, r, b, o, C_BOX, C_BOX, C_BOX, C_BOX);
        return;
    }
#endif
    AnimationFile *a = objectScriptList[o->type].animFile;
    Hitbox *h        = &hitboxList[a->hitboxListOffset + animFrames[animationList[a->aniListOffset + o->animation].frameListOffset + o->frame].hitboxID];
    int mx = me->xpos >> 16, my = me->ypos >> 16, ox = o->xpos >> 16, oy = o->ypos >> 16;
    scriptEng.checkResult = h->right[0] + ox > l + mx && h->left[0] + ox < r + mx && h->bottom[0] + oy > t + my && h->top[0] + oy < b + my;
}
#endif

// Sonic 1's Ring update sub (GlobalCode object "Ring", docs/37 phase 2b): Sonic 2's (PS1RingUpdate) without the
// 2P counters, with Sonic 1's global numbering and its achievements (200 rings; 30 rings in a row while rolling).
static void PS1RingUpdateS1()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr = scriptEng.checkResult;
    int *G  = globalVariables;
    int self = objectEntityPos;
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) { // ForEachActive 256, ARRAYPOS6
        ap[6]    = players.entityRefs[loop];
        Entity &me = PS1_OBJ(self);
        cr   = PS1_OBJ(0).state == 26; // CheckEqual OBJECTSTATE[0], 26
        t[0] = cr;
        cr   = PS1_OBJ(ap[6]).state == 26;
        t[0] = cr;
        cr   = PS1_OBJ(ap[6]).state == 25;
        t[0] |= cr;
        if (t[0] == 0) {
            PS1TouchS1(&PS1_OBJ(self), -8, -8, 8, 8, &PS1_OBJ(ap[6]));
            if (cr == 1) {
                me.type = 12;
                PS1_OBJ(0).values[0]++;
                if (PS1_OBJ(0).values[0] > 999)
                    PS1_OBJ(0).values[0] = 999;
                if (PS1_OBJ(0).values[0] >= G[20]) {
                    if (G[0] != 2) {
                        G[23]++;
                        PlaySfx(24, 0);
                        PauseSound();
                        PS1ResetObjectEntity(25, 38, 2, 0, 0);
                        PS1_OBJ(25).priority = 1;
                    }
                    if (debugMode == 0) {
                        if (PS1_OBJ(0).values[0] >= 200)
                            PS1CallNative2(G[99], 4, 100);
                    }
                    G[20] += 100;
                    if (G[20] > 300)
                        G[20] = 1000;
                }
                if (me.propertyValue == 1) {
                    if (debugMode == 0) {
                        if (G[5] == 0) {
                            if (ap[6] == 0) {
                                if (PS1_OBJ(0).animation == G[64]) {
                                    G[104]++;
                                    if (G[104] == 30) {
                                        PS1CallNative2(G[99], 0, 100);
                                        G[104] = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                if (G[19] == 0) {
                    PlaySfx(1, 0);
                    SetSfxAttributes(1, -1, -100);
                    G[19] = 1;
                }
                else {
                    PlaySfx(2, 0);
                    SetSfxAttributes(2, -1, 100);
                    G[19] = 0;
                }
            }
            else {
                if (PS1_OBJ(self).state == 0) {
                    if (PS1_OBJ(ap[6]).values[37] == 4) {
                        PS1TouchS1(&PS1_OBJ(self), -64, -64, 64, 64, &PS1_OBJ(ap[6]));
                        if (cr == 1) {
                            PS1_OBJ(self).state     = 1;
                            PS1_OBJ(self).values[1] = ap[6];
                        }
                    }
                }
            }
        }
    }
    Entity &me = PS1_OBJ(self);
    if (me.state == 1) {
        ap[0] = me.values[1];
        if (PS1_OBJ(ap[0]).values[37] != 4) {
            me.type           = 11;
            me.animationSpeed = 128;
            me.alpha          = 256;
        }
        else {
            ap[0] = me.values[1];
            if (me.xpos > PS1_OBJ(ap[0]).xpos) {
                if (me.xvel > 0)
                    me.xvel -= 49152;
                else
                    me.xvel -= 12288;
            }
            else {
                if (me.xvel < 0)
                    me.xvel += 49152;
                else
                    me.xvel += 12288;
            }
            if (me.ypos > PS1_OBJ(ap[0]).ypos) {
                if (me.yvel > 0)
                    me.yvel -= 49152;
                else
                    me.yvel -= 12288;
            }
            else {
                if (me.yvel < 0)
                    me.yvel += 49152;
                else
                    me.yvel += 12288;
            }
            me.xpos += me.xvel;
            me.ypos += me.yvel;
        }
    }
}
#endif
#endif
#if RETRO_PLATFORM == RETRO_PS1
// Sonic 2's Lose Ring update sub (GlobalCode object "Lose Ring": the rings scattered when the player is hit, up to
// 32 at once, ~20 VM instructions each a frame), natively, like PS1RingUpdate: one statement per script instruction,
// in order; patched in only when the sub matches LOSERING_SIG exactly.
static void PS1LoseRingUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    me.yvel += 6144;
    me.xpos += me.xvel;
    me.ypos += me.yvel;
    if (me.yvel >= 0) {
        ObjectFloorCollision(0, 8, 0); // ObjectTileCollision CSIDE_FLOOR, 0, 8, 0
        if (cr == 1) {
            t[0] = me.yvel;
            t[0] >>= 2;
            me.yvel -= t[0];
            me.yvel = -me.yvel;
            if (me.yvel > -65536)
                me.yvel = -65536;
        }
    }
    me.values[0]++;
    if (me.values[0] == 256) {
        me.type      = 0;
        me.xvel      = 0;
        me.yvel      = 0;
        me.values[0] = 0;
    }
    else {
        me.animationTimer += me.animationSpeed;
        if (me.animationTimer > 255) {
            me.animationTimer -= 256;
            me.frame = me.frame + 1;
            if (me.frame == 8) {
                me.frame = 0;
                if (me.animationSpeed > 16)
                    me.animationSpeed -= 16;
            }
        }
        if (me.values[0] >= 240)
            me.alpha -= 16;
    }
    if (me.values[0] > 63) {
        TypeGroupList &players = objectTypeGroupList[256];
        for (int loop = 0; loop < players.listSize; ++loop) { // ForEachActive 256, ARRAYPOS6
            ap[6] = players.entityRefs[loop];
            cr    = PS1_OBJ(ap[6]).state == 27;
            t[0]  = cr;
            cr    = PS1_OBJ(ap[6]).state == 26;
            t[0] |= cr;
            if (t[0] == 0) {
                TouchCollision(&PS1_OBJ(self), -8, -8, 8, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                if (cr == 1) {
                    me.type = 12;
                    if (me.propertyValue == 0)
                        me.drawOrder = 4;
                    else
                        me.drawOrder = 2;
                    me.values[0] = 0;
                    me.values[1] = 0;
                    me.values[0] = 0;
                    me.frame     = 0;
                    if (PS1_OBJ(ap[6]).values[16] == 1) {
                        PS1_OBJ(0).values[0]++;
                        if (PS1_OBJ(0).values[0] > 999)
                            PS1_OBJ(0).values[0] = 999;
                    }
                    else {
                        PS1_OBJ(ap[6]).values[0]++;
                        if (PS1_OBJ(ap[6]).values[0] > 999)
                            PS1_OBJ(ap[6]).values[0] = 999;
                    }
                    if (PS1_OBJ(0).values[0] >= G[21]) {
                        if (G[0] != 2) {
                            G[25]++;
                            PlaySfx(24, 0);
                            PauseSound();
                            PS1ResetObjectEntity(25, 34, 2, 0, 0);
                            PS1_OBJ(25).priority = 1;
                            G[21] += 100;
                            if (G[21] > 300)
                                G[21] = 1000;
                        }
                    }
                    if (G[20] == 0) {
                        PlaySfx(1, 0);
                        SetSfxAttributes(1, -1, -100);
                        G[20] = 1;
                    }
                    else {
                        PlaySfx(2, 0);
                        SetSfxAttributes(2, -1, 100);
                        G[20] = 0;
                    }
                }
            }
        }
    }
}
#endif
#if RETRO_PLATFORM == RETRO_PS1
// Metropolis's Button Bridge update sub (stage object "Button Bridge", up to 9 on screen), natively: one statement
// per script instruction, in order; the switch cases as its jump table maps them (state 0 / 1 / 2 / 3; the carry
// switch on the collision result: 1, and 2 or 3). Patched in only when the sub and its jump-table entries match
// BUTTONBRIDGE_SIG exactly (tools/scripts/patch_bytecode.py).
static void PS1ButtonBridgeSlide(int dirSign) // states 1 and 3: dirSign -1 = state 1 (direction 0 moves left)
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    t[0] = me.xpos;
    t[0] &= -65536;
    if (me.direction == 0) {
        me.xpos += dirSign * 131072;
        me.frame = dirSign < 0 ? me.frame + 1 : me.frame - 1;
    }
    else {
        me.xpos -= dirSign * 131072;
        me.frame = dirSign < 0 ? me.frame - 1 : me.frame + 1;
    }
    me.frame &= 3;
    me.values[0]--;
    if (me.values[0] <= 0) {
        if (dirSign < 0) { // state 1
            if (me.propertyValue == 0)
                me.state = 2;
            else {
                me.state     = 0;
                me.values[0] = 64;
            }
        }
        else { // state 3
            if (me.propertyValue == 0) {
                me.state     = 0;
                me.values[0] = 64;
            }
            else
                me.state = 2;
        }
    }
    t[1] = me.xpos;
    t[1] &= -65536;
    t[1] -= t[0];
    t[2]    = me.xpos;
    me.xpos = t[0];
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6] = players.entityRefs[loop];
        BoxCollision(&PS1_OBJ(self), -64, -12, 64, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
        switch (cr) {
            case 1: PS1_OBJ(ap[6]).xpos += t[1]; break;
            case 2:
            case 3:
                if (PS1_OBJ(ap[6]).state == 24) {
                    PS1_OBJ(ap[6]).values[1] = 0;
                    PS1_OBJ(ap[6]).animation = globalVariables[86];
                    PS1_OBJ(ap[6]).state     = 22;
                }
                break;
            default: break;
        }
    }
    me.xpos = t[2];
}
static void PS1ButtonBridgeUpdate()
{
    int *ap  = scriptEng.arrayPosition;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    TypeGroupList &players = objectTypeGroupList[256];
    switch (me.state) {
        case 0:
            if (PS1_OBJ(self + 1).values[0] == 1) {
                if (me.propertyValue == 0)
                    me.state = 1;
                else
                    me.state = 3;
            }
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                BoxCollision(&PS1_OBJ(self), -64, -12, 64, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            }
            break;
        case 1: PS1ButtonBridgeSlide(-1); break;
        case 2:
            me.values[0]++;
            if (me.values[0] == 180) {
                me.values[0] = 0;
                if (me.propertyValue == 0)
                    me.state = 3;
                else
                    me.state = 1;
                me.values[0] = 64;
            }
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                BoxCollision(&PS1_OBJ(self), -64, -12, 64, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            }
            break;
        case 3: PS1ButtonBridgeSlide(1); break;
        default: break;
    }
}
#endif
#if RETRO_PLATFORM == RETRO_PS1
// The plane switches' update subs (GlobalCode objects "Plane Sw V" / "Plane Sw H": each player passing through sets
// its collision plane and draw layer by direction), natively: one statement per script instruction, in order; the
// horizontal one also skips players in state 24 and tests the y velocity. Patched in only when the subs match
// PLANESWV_SIG / PLANESWH_SIG exactly.
static void PS1PlaneSwitchUpdate(bool vertical)
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        cr        = me.values[6] == 0;
        t[0]      = cr;
        cr        = p.gravity == 0;
        t[0] |= cr;
        if (!vertical) {
            cr = p.state == 24;
            t[0] |= cr;
        }
        if (t[0] == 1) {
            if (vertical)
                TouchCollision(&PS1_OBJ(self), -12, me.values[0], 12, me.values[1], &PS1_OBJ(ap[6]), 0, 0, 0, 0);
            else
                TouchCollision(&PS1_OBJ(self), me.values[0], -12, me.values[1], 12, &PS1_OBJ(ap[6]), 0, 0, 0, 0);
            if (cr == 1) {
                if ((vertical ? p.xvel : p.yvel) > 0) {
                    t[0] = (me.propertyValue & (1 << 6)) >> 6;
                    if (t[0] == 0)
                        p.collisionPlane = me.values[3];
                    p.values[18] = me.values[5];
                }
                else {
                    t[0] = (me.propertyValue & (1 << 6)) >> 6;
                    if (t[0] == 0)
                        p.collisionPlane = me.values[2];
                    p.values[18] = me.values[4];
                }
            }
        }
    }
}

// Shared pieces of the natives below: FUNC_DIV with the PS1 zero guard, FUNC_DRAWSPRITEXY / FUNC_DRAWSPRITE for the
// running entity (the VM's scriptInfo / entity), VAR_OBJECTCOLLISIONBOTTOM's getter.
static inline int PS1ScriptDiv(int a, int b)
{
    if (!b) {
        g_ps1ScriptDivZero = g_ps1ScriptDivZero + 1;
        return 0;
    }
    return a / b;
}
static void PS1DrawSpriteXY(int frame, int x, int y)
{
    ObjectScript *o = &objectScriptList[objectEntityList[objectEntityPos].type];
    SpriteFrame *f  = &scriptFrames[o->frameListOffset + frame];
    DrawSprite((x >> 16) - xScrollOffset + f->pivotX, (y >> 16) - yScrollOffset + f->pivotY, f->width, f->height, f->sprX, f->sprY,
               o->spriteSheetID);
}
static int PS1CollisionBottom(Entity &e)
{
    AnimationFile *animFile = objectScriptList[e.type].animFile;
    if (!animFile)
        return 0;
    int h = animFrames[animationList[animFile->aniListOffset + e.animation].frameListOffset + e.frame].hitboxID;
    return hitboxList[animFile->hitboxListOffset + h].bottom[0];
}

// Hidden Palace's Rotate Platform (stage object; update 58 VM instructions + draw 30 + 12 per chain link, ~29
// hblanks a frame each on the emulator), natively: one statement per script instruction, in order. The update
// calls GlobalCode function 45 (push the player off a touching platform) inline. Patched in only when the subs
// and function 45 match ROTPLAT_SIG / ROTPLAT_DRAW_SIG / PUSHPLAYER_FN_SIG (tools/scripts/patch_bytecode.py).
static void PS1RotatePlatformUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    TypeGroupList &players = objectTypeGroupList[256];
    if (me.state == 1) {
        t[0] = G[17];
        t[0] <<= 1;
        v[11] = Sin512(t[0]);
        v[11] >>= 3;
        v[11] += 192;
    }
    else {
        v[11] = 256;
    }
    t[2] = me.xpos;
    t[3] = me.ypos;
    v[0] += v[6];
    v[0] &= 131071;
    t[0] = v[0];
    t[0] >>= 8;
    t[1] = v[5];
    t[1]++;
    t[1] <<= 4;
    me.xpos = v[1];
    me.ypos = v[2];
    v[3]    = Cos256(t[0]);
    v[3] *= t[1];
    v[3] *= v[11];
    v[3] += t[2];
    v[3] &= -65536;
    v[1] = v[3];
    v[3] -= me.xpos;
    v[4] = Sin256(t[0]);
    v[4] *= t[1];
    v[4] *= v[11];
    v[4] += t[3];
    v[4] &= -65536;
    v[2] = v[4];
    v[4] -= me.ypos;
    if (v[7] == 0) {
        for (int loop = 0; loop < players.listSize; ++loop) {
            ap[6] = players.entityRefs[loop];
            PlatformCollision(&PS1_OBJ(self), -24, -8, 24, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 1) {
                PS1_OBJ(ap[6]).xpos += v[3];
                PS1_OBJ(ap[6]).ypos += v[4];
            }
        }
    }
    else {
        for (int loop = 0; loop < players.listSize; ++loop) {
            ap[6] = players.entityRefs[loop];
            TouchCollision(&PS1_OBJ(self), -16, -16, 16, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 1) { // CallFunction 45
                Entity &p = PS1_OBJ(ap[6]);
                if (p.state != 28) {
                    ap[0] = ap[6];
                    ap[0] += ap[7];
                    if (p.values[7] == 0) {
                        if (p.values[8] == 0) {
                            p.state = 26;
                            if (p.xpos > me.xpos)
                                p.speed = 131072;
                            else
                                p.speed = -131072;
                        }
                    }
                }
            }
        }
    }
    me.xpos = t[2];
    me.ypos = t[3];
    t[0]    = v[9];
    t[0] >>= 2;
    if (t[0] >= 0 && t[0] < scriptCode[84593]) // GetTableValue v10 t0 84593
        v[10] = scriptCode[84593 + t[0] + 1];
    v[9]++;
    v[9] &= 31;
}
static void PS1RotatePlatformDraw()
{
    int *t     = scriptEng.temp;
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    t[0]       = 0;
    t[1]       = 16;
    t[4]       = v[0];
    t[4] >>= 8;
    while (t[0] < v[5]) {
        t[2] = Cos256(t[4]);
        t[2] *= t[1];
        t[2] *= v[11];
        t[2] += me.xpos;
        t[3] = Sin256(t[4]);
        t[3] *= t[1];
        t[3] *= v[11];
        t[3] += me.ypos;
        PS1DrawSpriteXY(3, t[2], t[3]);
        t[0]++;
        t[1] += 16;
    }
    v[1] = Cos256(t[4]);
    v[1] *= t[1];
    v[1] *= v[11];
    v[1] += me.xpos;
    v[1] &= -65536;
    v[2] = Sin256(t[4]);
    v[2] *= t[1];
    v[2] *= v[11];
    v[2] += me.ypos;
    v[2] &= -65536;
    PS1DrawSpriteXY(v[8], v[1], v[2]);
    PS1DrawSpriteXY(v[10], me.xpos, me.ypos); // DrawSprite v10
}

// Hidden Palace's Bridge (stage object "Bridge" in Zone08, not Emerald Hill's; update 188 VM instructions, draw 49
// + a function call per log, ~70 hblanks a frame on the emulator), natively: one statement per script instruction,
// in order; the draw's per-log function (stage function 114: the log's frame, then DrawSpriteXY) inline. Patched
// in only when both subs and function 114 match HPZBRIDGE_SIG / HPZBRIDGE_DRAW_SIG / HPZBRIDGE_LOG_FN_SIG.
static void PS1HPZBridgeUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    TypeGroupList &players = objectTypeGroupList[256];
    if (v[0] > 0) {
        if (v[1] < 128)
            v[1] += 8;
    }
    else {
        if (v[1] > 0) {
            v[1] -= 8;
            v[5] = -1;
        }
        else {
            v[3] = 0;
        }
    }
    v[4] = v[3];
    v[4] *= v[1];
    v[4] >>= 7;
    v[0]  = 0;
    v[8]  = -5;
    v[11] = -5;
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        if (p.state != 25) {
            if (p.xpos > v[6]) {
                if (p.xpos < v[7]) {
                    if (ap[6] == v[5]) {
                        v[2] = p.xpos;
                        v[2] -= v[6];
                        t[0] = v[2];
                        t[0] >>= 8;
                        t[1] = v[7];
                        t[1] -= v[6];
                        t[2] = t[1];
                        t[2] >>= 16;
                        t[0] = PS1ScriptDiv(t[0], t[2]);
                        v[3] = Sin512(t[0]);
                        t[1] >>= 13;
                        v[3] *= t[1];
                        t[0] = me.ypos;
                        t[0] -= 3145728;
                        if (p.ypos > t[0]) {
                            if (p.yvel >= 0) {
                                t[2] = PS1CollisionBottom(p);
                                t[2] = -t[2];
                                t[2] <<= 16;
                                t[2] += v[4];
                                t[2] -= 524288;
                                v[0]++;
                                p.ypos = me.ypos;
                                p.ypos += t[2];
                                p.gravity         = 0;
                                p.yvel            = 0;
                                p.floorSensors[0] = 1;
                                p.floorSensors[1] = 1;
                                p.floorSensors[2] = 1;
                                v[8]              = v[2];
                                v[8] >>= 20;
                            }
                            else {
                                v[5] = -2;
                            }
                        }
                    }
                    else {
                        if (p.yvel >= 0) {
                            t[0] = p.xpos;
                            t[0] -= v[6];
                            if (t[0] > v[2]) {
                                t[0] = v[7];
                                t[0] -= p.xpos;
                                t[3] = v[7];
                                t[3] -= v[6];
                                t[3] -= v[2];
                                t[1] = t[0];
                                t[1] <<= 7;
                                t[1] = PS1ScriptDiv(t[1], t[3]);
                            }
                            else {
                                t[1] = t[0];
                                t[1] <<= 7;
                                t[1] = PS1ScriptDiv(t[1], v[2]);
                            }
                            t[2] = Sin512(t[1]);
                            t[2] *= v[4];
                            t[2] >>= 9;
                            t[2] -= 524288;
                            if (p.yvel < 32768) {
                                t[3] = t[2];
                                t[3] >>= 16;
                                t[4] = t[3];
                                t[3] -= 8;
                            }
                            else {
                                t[3] = t[2];
                                t[3] >>= 16;
                                t[4] = t[3];
                                t[4] += 8;
                            }
                            TouchCollision(&PS1_OBJ(self), -1024, t[3], 1024, t[4], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                            if (cr == 1) {
                                v[0]++;
                                p.ypos = PS1CollisionBottom(p);
                                p.ypos = -p.ypos;
                                p.ypos <<= 16;
                                p.ypos += me.ypos;
                                p.ypos += t[2];
                                p.floorSensors[0] = 1;
                                p.floorSensors[1] = 1;
                                p.floorSensors[2] = 1;
                                v[11]             = p.xpos;
                                v[11] -= v[6];
                                v[11] >>= 20;
                                if (ap[6] == 0) {
                                    if (v[5] < 4) {
                                        if (v[5] > 0) {
                                            v[2] = p.xpos;
                                            v[2] -= v[6];
                                            t[0] = v[2];
                                            t[0] >>= 8;
                                            t[1] = v[7];
                                            t[1] -= v[6];
                                            t[2] = t[1];
                                            t[2] >>= 16;
                                            t[0] = PS1ScriptDiv(t[0], t[2]);
                                            v[3] = Sin512(t[0]);
                                            t[1] >>= 13;
                                            v[3] *= t[1];
                                            v[4] = v[3];
                                            v[4] *= v[1];
                                            v[4] >>= 7;
                                        }
                                        v[5] = 0;
                                        if (p.yvel < 256)
                                            v[1] = 128;
                                    }
                                }
                                else {
                                    if (v[5] == -1) {
                                        v[5] = ap[6];
                                        if (p.yvel < 256)
                                            v[1] = 128;
                                    }
                                    if (v[5] == -2)
                                        v[5] = ap[6];
                                }
                                p.gravity = 0;
                                p.yvel    = 0;
                            }
                        }
                    }
                }
                else {
                    if (ap[6] == v[5]) {
                        v[5] = -2;
                        v[1] = 32;
                    }
                }
            }
            else {
                if (ap[6] == v[5]) {
                    v[5] = -2;
                    v[1] = 32;
                }
            }
        }
    }
    v[9] = v[8];
    v[9]--;
    v[10] = v[8];
    v[10]++;
    v[12] = v[11];
    v[12]--;
    v[13] = v[11];
    v[13]++;
    if (v[14] != v[8]) {
        v[14] = v[8];
        v[16] = 0;
    }
    if (v[15] != v[11]) {
        v[15] = v[11];
        v[17] = 0;
    }
    auto get = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    t[0] = v[16];
    t[0] >>= 2;
    get(73160, t[0], v[18]);
    get(73169, t[0], v[19]);
    v[16]++;
    v[16] &= 31;
    t[0] = v[17];
    t[0] >>= 2;
    get(73160, t[0], v[20]);
    get(73169, t[0], v[21]);
    v[17]++;
    v[17] &= 31;
}
static void PS1HPZBridgeDraw()
{
    int *t     = scriptEng.temp;
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    auto log   = [&]() { // CallFunction 114
        me.frame = 0;
        if (v[22] == v[9])
            me.frame = v[19];
        if (v[22] == v[10])
            me.frame = v[19];
        if (v[22] == v[12])
            me.frame = v[21];
        if (v[22] == v[13])
            me.frame = v[21];
        if (v[22] == v[8])
            me.frame = v[18];
        if (v[22] == v[11])
            me.frame = v[20];
        PS1DrawSpriteXY(me.frame, t[1], t[2]);
    };
    v[22] = 0;
    t[0]  = 0;
    t[1]  = v[6];
    t[1] += 524288;
    t[4] = 524288;
    t[5] = v[2];
    t[5] >>= 20;
    while (t[0] < t[5]) {
        t[3] = t[4];
        t[3] <<= 7;
        t[3] = PS1ScriptDiv(t[3], v[2]);
        t[2] = Sin512(t[3]);
        t[2] *= v[4];
        t[2] >>= 9;
        t[2] += me.ypos;
        log();
        t[1] += 1048576;
        t[4] += 1048576;
        t[0]++;
        v[22]++;
    }
    t[2] = v[4];
    t[2] += me.ypos;
    log();
    t[1] += 1048576;
    t[0]++;
    t[5] = v[7];
    t[5] -= v[6];
    t[5] -= v[2];
    t[1] = v[7];
    t[1] -= 524288;
    t[4]  = 524288;
    v[22] = me.propertyValue;
    v[22]--;
    while (t[0] < me.propertyValue) {
        t[3] = t[4];
        t[3] <<= 7;
        t[3] = PS1ScriptDiv(t[3], t[5]);
        t[2] = Sin512(t[3]);
        t[2] *= v[4];
        t[2] >>= 9;
        t[2] += me.ypos;
        log();
        t[1] -= 1048576;
        t[4] += 1048576;
        t[0]++;
        v[22]--;
    }
}

// CallFunction from a native opcode: the function runs in a nested ProcessScript that continues the running script's
// VM stacks where they are (a native may replace a sub, with empty stacks, or a function body, whose caller still has
// its return frame on functionStack), with the foreach stack `foreachDepth` deeper (the ForEach loops the native
// transcribes around the call), and a `return` back at that function-stack depth ends the nested run (as it would
// return to the caller). The caller's stack positions are restored afterwards. Exact because the functions a native
// may call never return from inside a loop or switch and never End the event (patch_bytecode.py
// callable_from_native / returns_balanced).
static bool s_ps1CallNested = false;
static int s_ps1CallFuncPos, s_ps1CallJumpPos, s_ps1CallForeachPos;
volatile uint32_t g_ps1ScriptNestedCalls = 0; // test evidence (logic_test.sh): nested calls made by natives
static void PS1CallScriptFunction(int fn, int foreachDepth)
{
    g_ps1ScriptNestedCalls = g_ps1ScriptNestedCalls + 1;
    int f = functionStackPos, j = jumpTableStackPos, e = foreachStackPos;
    s_ps1CallNested     = true;
    s_ps1CallFuncPos    = f;
    s_ps1CallJumpPos    = j;
    s_ps1CallForeachPos = e + foreachDepth;
    ProcessScript(scriptFunctionList[fn].ptr.scriptCodePtr, scriptFunctionList[fn].ptr.jumpTablePtr, EVENT_MAIN);
    functionStackPos  = f;
    jumpTableStackPos = j;
    foreachStackPos   = e;
}
static int PS1CollisionLeft(Entity &e) // VAR_OBJECTCOLLISIONLEFT's getter
{
    AnimationFile *animFile = objectScriptList[e.type].animFile;
    if (!animFile)
        return 0;
    int h = animFrames[animationList[animFile->aniListOffset + e.animation].frameListOffset + e.frame].hitboxID;
    return hitboxList[animFile->hitboxListOffset + h].left[0];
}

// Sonic 2's Stage Setup update sub (GlobalCode object "Stage Setup", every zone; ~60 VM instructions a frame, 13
// hblanks on the emulator), natively: one statement per script instruction, in order. The script functions it
// calls run in the VM (PS1CallScriptFunction): 71 the oscillators (itself PS1Oscillate), 50 the player's death.
// Patched in only when the sub and its jump-table entries match STAGESETUP_SIG.
static void PS1StageSetupUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr = scriptEng.checkResult;
    int *G  = globalVariables;
    TypeGroupList &players = objectTypeGroupList[256];
    if (stageMode < 2 || stageMode > 3) { // switch STAGESTATE: cases 2 and 3 only `break`; the default runs
        G[18]++;
        if (G[18] == 4) {
            G[18] = 0;
            G[19]++;
            G[19] &= 7;
        }
        PS1ScriptWrite(24725, scriptCode[24725] + 1); // Inc LOCAL[24725]
        if (scriptCode[24725] > 17)
            PS1ScriptWrite(24725, 0);
        if (G[0] != 2) {
            if (G[22] >= G[24]) {
                G[25]++;
                G[24] += 50000;
                PlaySfx(24, 0);
                PauseSound();
                PS1ResetObjectEntity(25, 34, 2, 0, 0);
                PS1_OBJ(25).priority = 1;
            }
        }
        G[17]++;
        G[17] &= 511;
        PS1CallScriptFunction(71, 0);
        if (timeEnabled == 1) {
            if (stageMinutes == 10) {
                cr   = debugMode == 1;
                t[0] = cr;
                cr   = G[0] == 2;
                t[0] |= cr;
                if (t[0] == 0) {
                    G[52]           = 1;
                    ap[6]           = 0;
                    PS1_OBJ(0).type = 1;
                    PS1CallScriptFunction(50, 0);
                }
                stageMinutes      = 9;
                stageSeconds      = 59;
                stageMilliseconds = 99;
                timeEnabled       = 0;
            }
        }
        for (int loop = 0; loop < players.listSize; ++loop) {
            ap[6]     = players.entityRefs[loop];
            Entity &p = PS1_OBJ(ap[6]);
            t[0]      = PS1CollisionLeft(p);
            t[0] <<= 16;
            t[0] += p.xpos;
            t[1] = curXBoundary1;
            t[1] <<= 16;
            if (t[0] < t[1]) {
                if (p.right == 1) {
                    p.xvel  = 65536;
                    p.speed = 65536;
                }
                else {
                    p.xvel  = 0;
                    p.speed = 0;
                }
                p.xpos = t[1];
                t[0]   = PS1CollisionLeft(p);
                t[0] <<= 16;
                p.xpos -= t[0];
            }
            t[1] = curYBoundary2;
            t[1] <<= 16;
            if (t[1] < G[49]) {
                if (p.ypos > G[49])
                    PS1CallScriptFunction(50, 1);
            }
            else {
                if (p.ypos > t[1])
                    PS1CallScriptFunction(50, 1);
            }
        }
    }
    if (G[5] == 0) {
        if (PS1_OBJ(0).controlMode > -1)
            G[7] = 1;
        else
            G[7] = 0;
    }
    else {
        G[7] = 0;
    }
    auto addRef = [](int layer, int ref) { drawListEntries[layer].entityRefs[drawListEntries[layer].listSize++] = ref; };
    ap[6] = ap[7];
    ap[6]--;
    while (ap[6] > -1) {
        if (PS1_OBJ(ap[6]).visible == 1) {
            ap[6] += ap[7];
            if (PS1_OBJ(ap[6]).values[18] == 0) {
                ap[6] -= ap[7];
                ap[0] = PS1_OBJ(ap[6]).values[18];
                addRef(ap[0], ap[6]);
                ap[6] += ap[7];
                addRef(ap[0], ap[6]);
                ap[6] -= ap[7];
            }
            else {
                ap[6] -= ap[7];
                ap[0] = PS1_OBJ(ap[6]).values[18];
                ap[6] += ap[7];
                addRef(ap[0], ap[6]);
                ap[6] -= ap[7];
                ap[0] = PS1_OBJ(ap[6]).values[18];
                addRef(ap[0], ap[6]);
            }
        }
        ap[6]--;
    }
}
#if PS1_GAME == 1
// Sonic 1's Stage Setup update sub (docs/37 phase 2b), like PS1StageSetupUpdate: its main part runs only in stage state 1
// (Sonic 2's: every state but 2 and 3), with Sonic 1's globals, timer local (22826), extra-life object (38) and death
// function (51); the time-over path doesn't set a global first. Patched in when STAGESETUP_SIG_S1 matches.
static void PS1StageSetupUpdateS1()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr = scriptEng.checkResult;
    int *G  = globalVariables;
    TypeGroupList &players = objectTypeGroupList[256];
    if (stageMode == 1) {
        G[17]++;
        if (G[17] == 4) {
            G[17] = 0;
            G[18]++;
            G[18] &= 7;
        }
        PS1ScriptWrite(22826, scriptCode[22826] + 1); // Inc LOCAL[22826]
        if (scriptCode[22826] > 17)
            PS1ScriptWrite(22826, 0);
        if (G[0] != 2) {
            if (G[21] >= G[22]) {
                G[23]++;
                G[22] += 50000;
                PlaySfx(24, 0);
                PauseSound();
                PS1ResetObjectEntity(25, 38, 2, 0, 0);
                PS1_OBJ(25).priority = 1;
            }
        }
        G[16]++;
        G[16] &= 511;
        PS1CallScriptFunction(71, 0);
        if (timeEnabled == 1) {
            if (stageMinutes == 10) {
                cr   = debugMode == 1;
                t[0] = cr;
                cr   = G[0] == 2;
                t[0] |= cr;
                if (t[0] == 0) {
                    ap[6]           = 0;
                    PS1_OBJ(0).type = 1;
                    PS1CallScriptFunction(51, 0);
                }
                stageMinutes      = 9;
                stageSeconds      = 59;
                stageMilliseconds = 99;
                timeEnabled       = 0;
            }
        }
        for (int loop = 0; loop < players.listSize; ++loop) {
            ap[6]     = players.entityRefs[loop];
            Entity &p = PS1_OBJ(ap[6]);
            t[0]      = PS1CollisionLeft(p);
            t[0] <<= 16;
            t[0] += p.xpos;
            t[1] = curXBoundary1;
            t[1] <<= 16;
            if (t[0] < t[1]) {
                if (p.right == 1) {
                    p.xvel  = 65536;
                    p.speed = 65536;
                }
                else {
                    p.xvel  = 0;
                    p.speed = 0;
                }
                p.xpos = t[1];
                t[0]   = PS1CollisionLeft(p);
                t[0] <<= 16;
                p.xpos -= t[0];
            }
            t[1] = curYBoundary2;
            t[1] <<= 16;
            if (t[1] < G[47]) {
                if (p.ypos > G[47])
                    PS1CallScriptFunction(51, 1);
            }
            else {
                if (p.ypos > t[1])
                    PS1CallScriptFunction(51, 1);
            }
        }
    }
    if (G[5] == 0) {
        if (PS1_OBJ(0).controlMode > -1)
            G[7] = 1;
        else
            G[7] = 0;
    }
    else {
        G[7] = 0;
    }
    auto addRef = [](int layer, int ref) { drawListEntries[layer].entityRefs[drawListEntries[layer].listSize++] = ref; };
    ap[6] = ap[7];
    ap[6]--;
    while (ap[6] > -1) {
        if (PS1_OBJ(ap[6]).visible == 1) {
            ap[6] += ap[7];
            if (PS1_OBJ(ap[6]).values[18] == 0) {
                ap[6] -= ap[7];
                ap[0] = PS1_OBJ(ap[6]).values[18];
                addRef(ap[0], ap[6]);
                ap[6] += ap[7];
                addRef(ap[0], ap[6]);
                ap[6] -= ap[7];
            }
            else {
                ap[6] -= ap[7];
                ap[0] = PS1_OBJ(ap[6]).values[18];
                ap[6] += ap[7];
                addRef(ap[0], ap[6]);
                ap[6] -= ap[7];
                ap[0] = PS1_OBJ(ap[6]).values[18];
                addRef(ap[0], ap[6]);
            }
        }
        ap[6]--;
    }
}
#endif
static int PS1CollisionRight(Entity &e) // VAR_OBJECTCOLLISIONRIGHT's getter
{
    AnimationFile *animFile = objectScriptList[e.type].animFile;
    if (!animFile)
        return 0;
    int h = animFrames[animationList[animFile->aniListOffset + e.animation].frameListOffset + e.frame].hitboxID;
    return hitboxList[animFile->hitboxListOffset + h].right[0];
}

// FUNC_DRAWSPRITESCREENXY / FUNC_DRAWNUMBERS for the running entity (the VM's scriptInfo).
static void PS1DrawSpriteScreenXY(int frame, int x, int y)
{
    ObjectScript *o = &objectScriptList[objectEntityList[objectEntityPos].type];
    SpriteFrame *f  = &scriptFrames[o->frameListOffset + frame];
    DrawSprite(x + f->pivotX, y + f->pivotY, f->width, f->height, f->sprX, f->sprY, o->spriteSheetID);
}
static void PS1DrawNumbers(int base, int x, int y, int value, int digits, int spacing, int showAll)
{
    ObjectScript *o = &objectScriptList[objectEntityList[objectEntityPos].type];
    int i           = 10;
    if (showAll) {
        while (digits > 0) {
            SpriteFrame *f = &scriptFrames[o->frameListOffset + value % i / (i / 10) + base];
            DrawSprite(f->pivotX + x, f->pivotY + y, f->width, f->height, f->sprX, f->sprY, o->spriteSheetID);
            x -= spacing;
            i *= 10;
            --digits;
        }
    }
    else {
        int extra = 10;
        if (value)
            extra = 10 * value;
        while (digits > 0) {
            if (extra >= i) {
                SpriteFrame *f = &scriptFrames[o->frameListOffset + value % i / (i / 10) + base];
                DrawSprite(f->pivotX + x, f->pivotY + y, f->width, f->height, f->sprX, f->sprY, o->spriteSheetID);
            }
            x -= spacing;
            i *= 10;
            --digits;
        }
    }
}

// Sonic 2's HUD drawing (GlobalCode function 72, called by the HUD's draw sub every frame in every zone; ~25 VM
// instructions + DrawNumbers), natively: one statement per script instruction, in order. The function's body is
// patched to `PS1HUDDraw / return` only when it matches HUD_FN_SIG. Sonic 1's is the same function with its own
// globals for the score and the lives (docs/37 phase 2b, HUD_FN_SIG_S1).
#if PS1_GAME == 1
#define PS1_G_SCORE 21
#define PS1_G_LIVES 23
#else
#define PS1_G_SCORE 22
#define PS1_G_LIVES 25
#endif
static void PS1HUDDraw()
{
    int *t     = scriptEng.temp;
    int *G     = globalVariables;
    Entity &me = objectEntityList[objectEntityPos];
    Entity &p  = PS1_OBJ(0);
    PS1DrawSpriteScreenXY(10, 17, 13);
    if (stageMinutes == 9) {
        if (me.values[1] > 7)
            PS1DrawSpriteScreenXY(11, 17, 29);
    }
    if (p.values[0] == 0) {
        if (me.values[1] > 7)
            PS1DrawSpriteScreenXY(12, 17, 45);
    }
    PS1DrawNumbers(0, 104, 13, G[PS1_G_SCORE], 6, 8, 0);
    if (G[0] < 2) {
        PS1DrawSpriteScreenXY(14, 67, 29);
    }
    else {
        PS1DrawSpriteScreenXY(13, 67, 29);
        PS1DrawNumbers(0, 104, 29, stageMilliseconds, 2, 8, 1);
    }
    PS1DrawNumbers(0, 80, 29, stageSeconds, 2, 8, 1);
    PS1DrawNumbers(0, 56, 29, stageMinutes, 1, 8, 1);
    PS1DrawNumbers(0, 80, 45, p.values[0], 3, 8, 0);
    if (debugMode == 1) {
        t[0] = p.xpos;
        t[0] >>= 16;
        t[0] = abs(t[0]);
        t[1] = SCREEN_XSIZE;
        t[1] -= 24;
        PS1DrawNumbers(0, t[1], 13, t[0], 5, 8, 1);
        t[0] = p.ypos;
        t[0] >>= 16;
        t[0] = abs(t[0]);
        PS1DrawNumbers(0, t[1], 29, t[0], 5, 8, 1);
        t[1] -= 42;
        if (p.xpos >= 0)
            PS1DrawSpriteScreenXY(18, t[1], 15);
        else
            PS1DrawSpriteScreenXY(36, t[1], 15);
        if (p.ypos >= 0)
            PS1DrawSpriteScreenXY(19, t[1], 31);
        else
            PS1DrawSpriteScreenXY(37, t[1], 31);
    }
    t[0] = playerListPos;
    t[0] += 15;
    PS1DrawSpriteScreenXY(t[0], 16, 212);
    t[0] += 6;
    PS1DrawSpriteScreenXY(t[0], 33, 213);
    PS1DrawSpriteScreenXY(20, 38, 222);
    PS1DrawNumbers(24, 56, 220, G[PS1_G_LIVES], 2, 8, 0);
}

// VAR_OBJECTOUTOFBOUNDS's getter (the !RETRO_REV00 branch), for natives.
static int PS1ObjectOutOfBounds(Entity *entPtr)
{
    int boundX1_2P = -(0x200 << 16);
    int boundX2_2P = (0x200 << 16);
    int boundX3_2P = -(0x180 << 16);
    int boundX4_2P = (0x180 << 16);
    int boundY1_2P = -(0x180 << 16);
    int boundY2_2P = (0x180 << 16);
    int boundY3_2P = -(0x100 << 16);
    int boundY4_2P = (0x100 << 16);
    int x          = entPtr->xpos >> 16;
    int y          = entPtr->ypos >> 16;
    if (entPtr->priority == PRIORITY_BOUNDS_SMALL || entPtr->priority == PRIORITY_ACTIVE_SMALL) {
        if (stageMode == STAGEMODE_2P) {
            x         = entPtr->xpos;
            y         = entPtr->ypos;
            bool oob1 = x <= objectEntityList[0].xpos + boundX3_2P || x >= objectEntityList[0].xpos + boundX4_2P
                        || y <= objectEntityList[0].ypos + boundY3_2P || y >= objectEntityList[0].ypos + boundY4_2P;
            bool oob2 = x <= objectEntityList[1].xpos + boundX3_2P || x >= objectEntityList[1].xpos + boundX4_2P
                        || y <= objectEntityList[1].ypos + boundY3_2P || y >= objectEntityList[1].ypos + boundY4_2P;
            return oob1 && oob2;
        }
        return x <= xScrollOffset - OBJECT_BORDER_X3 || x >= xScrollOffset + OBJECT_BORDER_X4 || y <= yScrollOffset - OBJECT_BORDER_Y3
               || y >= yScrollOffset + OBJECT_BORDER_Y4;
    }
    if (stageMode == STAGEMODE_2P) {
        x         = entPtr->xpos;
        y         = entPtr->ypos;
        bool oob1 = x <= objectEntityList[0].xpos + boundX1_2P || x >= objectEntityList[0].xpos + boundX2_2P
                    || y <= objectEntityList[0].ypos + boundY1_2P || y >= objectEntityList[0].ypos + boundY2_2P;
        bool oob2 = x <= objectEntityList[1].xpos + boundX1_2P || x >= objectEntityList[1].xpos + boundX2_2P
                    || y <= objectEntityList[1].ypos + boundY1_2P || y >= objectEntityList[1].ypos + boundY2_2P;
        return oob1 && oob2;
    }
    return x <= xScrollOffset - OBJECT_BORDER_X1 || x >= xScrollOffset + OBJECT_BORDER_X2 || y <= yScrollOffset - OBJECT_BORDER_Y1
           || y >= yScrollOffset + OBJECT_BORDER_Y2;
}

// Wing Fortress's Turret Platform (stage object in Zone11; update 40 VM instructions, ~3.5 hblanks a frame each,
// 7 on screen), natively: one statement per script instruction, in order; its reset (stage function 121) inline.
// Patched in only when the sub (with its jump-table entries) and function 121 match TURRET_SIG / TURRET_FN_SIG.
static void PS1TurretPlatformUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    switch (me.state) {
        case 0:
            t[0] = G[17];
            t[0] &= 240;
            if (t[0] == me.propertyValue) {
                me.priority       = 1;
                me.values[0]      = 160;
                me.animationTimer = 6;
                me.state          = 1;
            }
            break;
        case 1:
            me.animationTimer++;
            me.frame = me.animationTimer;
            me.frame = me.frame / 6;
            if (me.frame == 4)
                me.state = 2;
            break;
        case 2:
            me.values[0]--;
            if (me.values[0] == 0)
                me.state = 3;
            break;
        case 3:
            me.animationTimer--;
            me.frame = me.animationTimer;
            me.frame = me.frame / 6;
            if (me.animationTimer == 0)
                me.state = 0;
            break;
    }
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6] = players.entityRefs[loop];
        if (me.frame >= 3)
            PlatformCollision(&PS1_OBJ(self), -32, -24, 32, 0, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
    }
    if (PS1ObjectOutOfBounds(&PS1_OBJ(self)) == 1) {
        ap[0]     = self;
        Entity &o = PS1_OBJ(ap[0]); // CallFunction 121
        o.values[0]      = 0;
        o.animationTimer = 0;
        o.frame          = 0;
        o.state          = 0;
        o.priority       = 1;
    }
}

// Wing Fortress's Belt Platform (stage object in Zone11; update 50 VM instructions, ~2.8 hblanks a frame each, 8 on
// screen), natively: one statement per script instruction, in order. Patched in only when the sub and its
// jump-table entries match BELTPLAT_SIG.
static void PS1BeltPlatformUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    switch (me.state) {
        case 0: break;
        case 1:
            me.animationTimer++;
            if (me.animationTimer == 4) {
                me.animationTimer = 0;
                me.frame--;
                if (me.frame == 0)
                    me.state = 2;
            }
            break;
        case 2: {
            me.ypos += me.yvel;
            v[0]--;
            if (v[0] == 0)
                me.state = 3;
            t[2]                   = 0;
            TypeGroupList &players = objectTypeGroupList[256];
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                t[0]  = (v[1] & (1 << t[2])) >> t[2];
                if (t[0] == 1)
                    PS1_OBJ(ap[6]).ypos += me.yvel;
                v[1] &= ~(1 << t[2]);
                PlatformCollision(&PS1_OBJ(self), -24, -4, 24, 4, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                if (cr == 1)
                    v[1] |= 1 << t[2];
                t[2]++;
            }
            break;
        }
        case 3:
            me.animationTimer++;
            if (me.animationTimer == 4) {
                me.animationTimer = 0;
                me.frame++;
                if (me.frame == 3)
                    me.state = 0;
            }
            break;
    }
    t[0] = me.xpos;
    t[0] >>= 16;
    t[0] -= cameraXPos;
    t[0] = abs(t[0]);
    t[0] -= 128;
    if (t[0] > SCREEN_CENTERX) {
        me.state    = 0;
        me.priority = 0;
    }
}

// Casino Night's H Flipper (stage object in Zone04; update 170 VM instructions, ~15 hblanks a frame each on the
// emulator), natively: one statement per script instruction, in order; its launch (stage function 122) inline.
// OBJECTIXPOS / OBJECTIYPOS reads keep the VM's write-back: the position is truncated to whole pixels.
// Patched in only when the sub (with its jump-table entries) and function 122 match HFLIPPER_SIG / HFLIPPER_FN_SIG.
static void PS1HFlipperUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    auto get   = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    auto notLocked = [&](Entity &p) { // CheckEqual state 31 / Or state 13 -> TEMP0; true when TEMP0 == 0
        cr   = p.state == 31;
        t[0] = cr;
        cr   = p.state == 13;
        t[0] |= cr;
        return t[0] == 0;
    };
    auto launch = [&](Entity &p) { // CallFunction 122
        t[0]   = p.xpos >> 16;
        p.xpos = t[0] << 16; // Equal TEMP0 OBJECTIXPOS[1,1,6]: write-back
        int ix = me.xpos >> 16;
        t[0] -= ix;
        me.xpos = ix << 16; // Sub TEMP0 OBJECTIXPOS: write-back
        if (me.direction == 1)
            t[0] = -t[0];
        t[0] += 35;
        t[2] = t[0];
        if (t[2] > 64)
            t[2] = 64;
        t[2] <<= 5;
        t[2] += 2048;
        t[2] = -t[2];
        t[3] = t[0];
        t[3] >>= 2;
        t[3] += 64;
        t[0] = Sin256(t[3]);
        t[1] = Cos256(t[3]);
        t[0] *= t[2];
        t[1] *= t[2];
        p.yvel = t[0];
        if (me.direction == 1)
            t[1] = -t[1];
        p.speed = t[1];
        p.xvel  = t[1];
    };
    if (me.state == 1) { // switch OBJECTSTATE: case 1 only
        me.frame = v[0];
        me.frame = me.frame >> 2;
        me.frame = me.frame + 1;
        if (v[0] < 12) {
            v[0]++;
        }
        else {
            v[0]     = 0;
            me.frame = 0;
            me.state = 0;
        }
    }
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        cr        = p.state == 28;
        t[0]      = cr;
        cr        = p.state == 26;
        t[0] |= cr;
        if (t[0] != 0)
            continue;
        v[4] = p.xpos;
        v[4] -= me.xpos;
        v[4] >>= 16;
        v[3] = v[4];
        v[3] += PS1CollisionLeft(p);
        v[5] = v[4];
        v[5] += PS1CollisionRight(p);
        v[3] += 24;
        if (v[3] < 0)
            v[3] = 0;
        if (v[3] > 47)
            v[3] = 47;
        v[4] += 24;
        if (v[4] < 0)
            v[4] = 0;
        if (v[4] > 47)
            v[4] = 47;
        v[5] += 24;
        if (v[5] < 0)
            v[5] = 0;
        if (v[5] > 47)
            v[5] = 47;
        get(v[1], v[3], v[6]);
        get(v[1], v[4], v[7]);
        get(v[1], v[5], v[8]);
        v[9] = v[6];
        if (v[7] < v[9])
            v[9] = v[7];
        if (v[8] < v[9])
            v[9] = v[8];
        get(v[2], v[3], v[6]);
        get(v[2], v[4], v[7]);
        get(v[2], v[5], v[8]);
        v[10] = v[6];
        if (v[7] > v[10])
            v[10] = v[7];
        if (v[8] > v[10])
            v[10] = v[8];
        if (p.state != 1) {
            BoxCollision(&PS1_OBJ(self), -24, v[9], 24, v[10], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 4) {
                cr   = p.state == 24;
                t[0] = cr;
                cr   = p.state == 20;
                t[0] |= cr;
                cr = p.state == 21;
                t[0] |= cr;
                cr = p.state == 19;
                t[0] |= cr;
                cr = p.state == 31;
                t[0] |= cr;
                cr = p.state == 13;
                t[0] |= cr;
                if (t[0] == 0) {
                    p.state   = 12;
                    p.gravity = 1;
                }
            }
            t[0] = 4;
        }
        else {
            t[0] = 12;
        }
        v[9] -= t[0];
        v[10] = v[9];
        v[10] += 8;
        TouchCollision(&PS1_OBJ(self), -24, v[9], 24, v[10], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
        if (cr != 1)
            continue;
        p.ypos = v[9];
        p.ypos += t[0];
        p.ypos -= PS1CollisionBottom(p);
        p.ypos <<= 16;
        p.ypos += me.ypos;
        if (me.state == 0) {
            if (me.direction == 0)
                TouchCollision(&PS1_OBJ(self), -26, -12, 20, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            else
                TouchCollision(&PS1_OBJ(self), -20, -12, 26, 8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 1) {
                if (notLocked(p)) {
                    p.state     = 14;
                    p.animation = G[67];
                }
                p.scrollTracking = 0;
                p.gravity        = 0;
                p.controlLock    = 8;
                if (p.jumpPress == 1) {
                    if (notLocked(p))
                        p.state = 12;
                    p.gravity     = 1;
                    p.values[1]   = 0;
                    p.controlLock = 0;
                    launch(p);
                    me.state = 1;
                    StopSfx(0);
                    PlaySfx(48, 0);
                }
                else {
                    p.speed = me.xvel;
                    p.xvel  = me.xvel;
                    p.yvel  = 0;
                }
            }
            else {
                if (notLocked(p))
                    p.state = 12;
                p.gravity   = 1;
                p.values[1] = 0;
            }
            p.animation = G[67];
            if (p.prevAnimation != G[67])
                p.ypos = ((p.ypos >> 16) - p.values[30]) << 16; // Sub OBJECTIYPOS OBJECTVALUE30 (write-back)
        }
        else {
            if (notLocked(p))
                p.state = 12;
            launch(p);
            p.gravity   = 1;
            p.values[1] = 0;
        }
    }
}

// Hill Top's Earthquake (stage object in Zone05: the ceiling / floor sections that rise and fall; update 99 VM
// instructions, ~5 hblanks a frame each on the emulator), natively: one statement per script instruction, in order.
// Crushing (function 46) and falling out (function 50) run in the VM from inside the player loop (foreach depth 1).
// Patched in only when the sub and its jump-table entries match EARTHQUAKE_SIG and the functions are callable.
static void PS1EarthquakeUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    auto L     = [](int pos) { return (int)scriptCode[pos]; };
    TouchCollision(&PS1_OBJ(self), v[7], v[8], v[9], v[10], &PS1_OBJ(0), 0, 0, 0, 0);
    if (cr == 1) {
        if (L(63679) == 0) {
            PS1ScriptWrite(63679, 2);
            PS1ScriptWrite(63680, v[0]);
            PS1ScriptWrite(63681, v[1]);
            PS1ScriptWrite(63682, v[2]);
            hParallax.scrollPos[20] = v[16];
        }
    }
    TouchCollision(&PS1_OBJ(self), v[3], v[4], v[5], v[6], &PS1_OBJ(0), 0, 0, 0, 0);
    if (cr == 1) {
        if (L(63679) == 0) {
            PS1ScriptWrite(63679, 1);
            PS1ScriptWrite(63680, v[0]);
            PS1ScriptWrite(63681, v[1]);
            PS1ScriptWrite(63682, v[2]);
            hParallax.scrollPos[20] = v[16];
        }
    }
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        t[0]      = v[12];
        t[0] -= v[15];
        t[1] = v[14];
        t[1] -= v[15];
        if (me.propertyValue == 3) {
            t[2] = p.xpos;
            t[2] -= me.xpos;
            t[2] += 25165824;
            if (t[2] > 0) {
                t[2] >>= 18;
                t[0] += t[2];
            }
            t[0]++;
            if (p.values[42] == 0) {
                if (p.gravity == 1) {
                    if (p.xvel > 0) {
                        t[2] = p.xvel;
                        t[2] >>= 2;
                        p.ypos += t[2];
                        p.ypos += 131072;
                    }
                }
            }
        }
        BoxCollision(&PS1_OBJ(self), v[11], t[0], v[13], t[1], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
        switch (cr) { // the script's switch: case 1 falls into cases 2 / 3
            case 1:
                t[0] = L(63683);
                t[0] -= v[15];
                t[0] <<= 16;
                p.ypos -= t[0];
                // fall through
            case 2:
            case 3:
                if (me.propertyValue == 2)
                    PS1CallScriptFunction(46, 1);
                if (me.propertyValue == 4)
                    PS1CallScriptFunction(46, 1);
                if (me.propertyValue == 5)
                    PS1CallScriptFunction(46, 1);
                break;
            case 4:
                if (p.gravity == 0)
                    PS1CallScriptFunction(50, 1);
                break;
        }
    }
    TypeGroupList &group = objectTypeGroupList[11];
    for (int loop = 0; loop < group.listSize; ++loop) {
        ap[0]     = group.entityRefs[loop];
        Entity &o = PS1_OBJ(ap[0]);
        if (o.yvel >= 0) {
            t[0] = v[12];
            t[0] -= v[15];
            t[1] = v[14];
            t[1] -= v[15];
            if (me.propertyValue == 3) {
                t[2] = o.xpos;
                t[2] -= me.xpos;
                t[2] += 25165824;
                if (t[2] > 0) {
                    t[2] >>= 18;
                    t[0] += t[2];
                }
                t[0]++;
            }
            t[2] = o.yvel;
            PlatformCollision(&PS1_OBJ(self), v[11], t[0], v[13], t[1], &PS1_OBJ(ap[0]), -8, -8, 8, 8);
            if (cr == 1) {
                o.yvel = t[2];
                t[2] >>= 2;
                o.yvel -= t[2];
                o.yvel = -o.yvel;
                if (o.yvel > -65536)
                    o.yvel = -65536;
                o.gravity = 1;
            }
        }
    }
    v[15] = L(63683);
}

// Sonic 2's player input / timers (GlobalCode function 0, called by the Player Object's update every frame in every
// zone; ~60 VM instructions a frame), natively: one statement per script instruction, in order. The mobile touch
// rectangles (CheckTouchRect: no touches on the PS1, but its debug hitbox is kept), the key variables with the VM's
// inputCheck (their array index is the running entity), ProcessObjectControl, the shield / invincibility / blink
// timers. The functions it calls (37, 41, the variable GLOBAL[100]) run in the VM (PS1CallScriptFunction); the
// running entity is re-read after each call, as the VM re-reads objectEntityPos. The body is patched to
// `PS1PlayerInput / return` only when it matches PLAYERINPUT_FN_SIG and no function of the file returns inside a loop.
static void PS1PlayerInput()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr = scriptEng.checkResult;
    int *G  = globalVariables;
    auto E  = []() -> Entity & { return objectEntityList[objectEntityPos]; };
    auto ic = []() { return !(forceUseScripts || Engine.usingOrigins) || objectEntityPos <= 1; }; // inputCheck
    auto L  = [](int pos) { return (int)scriptCode[pos]; };
    auto touch = [&](int l, int tp, int r, int b) { // FUNC_CHECKTOUCHRECT
        cr = -1;
#if !RETRO_USE_ORIGINAL_CODE
        AddDebugHitbox(H_TYPE_FINGER, NULL, l, tp, r, b);
#endif
        for (int f = 0; f < touches; ++f) {
            if (touchDown[f] && touchX[f] > l && touchX[f] < r && touchY[f] > tp && touchY[f] < b)
                cr = f;
        }
    };
    auto pause = [&]() {
        if (G[13] == 0) {
            PlaySfx(23, 0);
            StopSfx(19);
            StopSfx(20);
            Engine.gameMode = 5;
        }
    };
    if (G[5] == 0) {
        if (E().controlMode == 0) {
            touch(0, 96, SCREEN_CENTERX, SCREEN_YSIZE);
            if (cr > -1) {
                ap[0] = cr;
                t[0]  = touchX[ap[0]];
                t[0] -= saveRAM[39];
                t[1] = touchY[ap[0]];
                t[1] -= saveRAM[40];
                t[2] = ArcTanLookup(t[0], t[1]);
                t[2] += 32;
                t[2] &= 255;
                t[2] >>= 6;
                switch (t[2]) {
                    case 0:
                        if (ic())
                            keyDown.right = 1;
                        break;
                    case 1:
                        if (ic())
                            keyDown.down = 1;
                        break;
                    case 2:
                        if (ic())
                            keyDown.left = 1;
                        break;
                    case 3:
                        if (ic())
                            keyDown.up = 1;
                        break;
                }
            }
            touch(SCREEN_CENTERX, 96, SCREEN_XSIZE, 240);
            if (cr > -1) {
                if (ic())
                    keyDown.A = 1;
            }
            if (G[102] == 0) { // Or KEYPRESSBUTTONA KEYDOWNBUTTONA
                int a = keyPress.A && ic();
                a |= keyDown.A && ic();
                if (ic())
                    keyPress.A = a;
            }
            G[102] = keyDown.A && ic();
            if (debugMode == 1) {
                touch(0, 0, 112, 56);
                if (cr > -1) {
                    if (ic())
                        keyDown.B = 1;
                }
                if (G[103] == 0) {
                    int b = keyPress.B && ic();
                    b |= keyDown.B && ic();
                    if (ic())
                        keyPress.B = b;
                }
                G[103] = keyDown.B && ic();
            }
            touch(240, 0, SCREEN_XSIZE, 40);
            if (cr > -1)
                pause();
            if ((keyPress.start && ic()) == 1)
                pause();
            // IfEqual KEYDOWNBUTTONB 2: the mobile back key's pause, its 1 patched to 2 (never true; Circle jumps on
            // the PS1, tools/scripts/patch_bytecode.py patch_back_pause)
        }
        ProcessObjectControl(&E());
        if (G[13] == 1) { // CallNativeFunction2 GLOBAL[137] 0 0
            int op0 = G[137], op1 = 0, op2 = 0;
            if (op0 >= 0 && op0 < NATIIVEFUNCTION_COUNT) {
                void (*func)(int *, int *) = (void (*)(int *, int *))nativeFunction[op0];
                if (func)
                    func(&op1, &op2);
            }
        }
    }
    else {
        if (G[106] == 0) {
            touch(0, 0, SCREEN_XSIZE, SCREEN_YSIZE);
            if ((keyPress.start && ic()) == 1)
                cr = 0;
            if (cr > -1) {
                if (L(12) > 1)
                    PS1ScriptWrite(12, 1);
            }
            if ((keyPress.start && ic()) == 1) {
                if (L(12) > 1)
                    PS1ScriptWrite(12, 1);
            }
        }
        if (E().controlMode == 0) {
            PS1ScriptWrite(11, L(11) - 1);
            if (L(11) < 1) {
                if (L(9) < L(10)) {
                    if (L(9) >= 0 && L(9) < scriptCode[L(8)]) // GetTableValue TEMP0 LOCAL9 LOCAL8
                        t[0] = scriptCode[L(8) + L(9) + 1];
                    E().up        = (t[0] & (1 << 0)) >> 0;
                    E().down      = (t[0] & (1 << 1)) >> 1;
                    E().left      = (t[0] & (1 << 2)) >> 2;
                    E().right     = (t[0] & (1 << 3)) >> 3;
                    E().jumpPress = (t[0] & (1 << 4)) >> 4;
                    E().jumpHold  = (t[0] & (1 << 5)) >> 5;
                    PS1ScriptWrite(9, L(9) + 1);
                    if (L(9) >= 0 && L(9) < scriptCode[L(8)]) // GetTableValue LOCAL11 LOCAL9 LOCAL8
                        PS1ScriptWrite(11, scriptCode[L(8) + L(9) + 1]);
                    PS1ScriptWrite(9, L(9) + 1);
                }
            }
            else {
                if (E().jumpPress == 1)
                    E().jumpPress = 0;
            }
            if (L(12) > 0) {
                PS1ScriptWrite(12, L(12) - 1);
                if (L(12) < 1) {
                    PS1ResetObjectEntity(11, 6, 0, 0, 0);
                    PS1_OBJ(11).state     = 8;
                    PS1_OBJ(11).priority  = 1;
                    PS1_OBJ(11).drawOrder = 6;
                    E().values[7]         = 80;
                    cameraEnabled         = 0;
                }
            }
        }
    }
    if (E().values[6] > 0) {
        E().values[6]--;
        if (E().values[6] < 1) {
            ap[6] = objectEntityPos;
            PS1CallScriptFunction(41, 0);
            t[0]                   = 0;
            TypeGroupList &players = objectTypeGroupList[256];
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                t[0] += PS1_OBJ(ap[6]).values[6];
            }
            if (t[0] == 0) {
                if (G[100] != 0)
                    PS1CallScriptFunction(G[100], 0);
            }
            E().values[6] = 0;
        }
    }
    if (E().state != 27) {
        if (E().values[8] > 0) {
            E().values[8]--;
            t[0] = (E().values[8] & (1 << 2)) >> 2;
            if (t[0] == 1)
                E().visible = 0;
            else
                E().visible = 1;
        }
    }
    if (E().values[7] > 0) {
        E().values[7]--;
        if (E().values[7] == 0) {
            t[0]                   = 0;
            TypeGroupList &players = objectTypeGroupList[256];
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                t[0] += PS1_OBJ(ap[6]).values[7];
            }
            if (t[0] == 0) {
                if (trackID == 2)
                    PlayMusic(0, 0);
            }
            if (PS1_OBJ(objectEntityPos + ap[7]).type == G[42]) { // OBJECTTYPE[2,1,7]: entity (this + arrayPos7)
                ap[6] = objectEntityPos;
                ap[0] = ap[6];
                ap[0] += ap[7];
                PS1CallScriptFunction(37, 0);
            }
        }
    }
    if (E().state != 16) {
        if (E().state != 17) {
            if (E().lookPosY > 0)
                E().lookPosY -= 2;
            if (E().lookPosY < 0)
                E().lookPosY += 2;
        }
    }
    if (E().values[11] > 0) {
        E().values[11]--;
        if (E().values[11] == 0) {
            if (objectEntityPos == cameraTarget)
                cameraStyle = 0;
        }
    }
    if (E().state != 19) {
        if (E().values[26] != 0) {
            StopSfx(19);
            StopSfx(20);
            E().values[26] = 0;
        }
    }
}

#if PS1_GAME == 1
// Sonic 1's special stage, function 9 (docs/37 phase 7): an object's place in the rotating maze -- its offset from
// the player (entity 0) rotated by the maze angle (LOCAL[0]) into TEMP0 / TEMP1, and its rotation 512 - angle. Every
// block's and ring's draw sub calls it (~200 a frame: 25 VM instructions each). One statement per instruction, the
// temps left as the script leaves them. Patched to `PS1SSRotPos / return` when SSROTPOS_FN_SIG_S1 matches.
static void PS1SSRotPos()
{
    int *t     = scriptEng.temp;
    Entity &me = objectEntityList[objectEntityPos];
    Entity &p  = PS1_OBJ(0);
    // the script's 24 statements with LOCAL[0] and its sine / cosine read once; the temps end as the script leaves them
    int ang = scriptCode[0], sn = Sin512(ang), cs = Cos512(ang);
    int dx = (me.xpos - p.xpos) >> 8, dy = (me.ypos - p.ypos) >> 8;
    t[2] = dx;
    t[3] = dy;
    t[0] = ((sn * dy + cs * dx) >> 1) + p.xpos;
    t[4] = cs * dy;
    t[5] = sn * dx;
    t[1] = ((t[4] - t[5]) >> 1) + p.ypos;
    me.rotation = 512 - ang;
}
#endif

#if PS1_GAME == 1
// Sonic 1's special stage, function 10 (docs/37 phase 7): a maze block against the player (entity 0), called by the
// blocks' update subs from their player loop -- the solid box (bits of the player's value 11) or the touch box that
// pushes the player out along the faster axis. One statement per instruction; SSBLOCKCOLLIDE_FN_SIG_S1.
// The block's two tests against the player when they cannot hit (~70 blocks a frame, most far from the player):
// BoxCollision2(me, -12, -12, 12, 12, o, C_BOX x4) misses in every one of its four sensor steps when the player's
// hitbox lies wholly beyond the block's box (to the left / right / above / below; with the box offsets' signs and a
// player hitbox around its origin at least 4 px wide, each step's test needs the boxes to reach each other), and
// TouchCollision's -10..10 box is inside it. What BoxCollision2 leaves then: checkResult 0 and the sensors of its last
// step (the steps' writes worked through for both of its branches: floor / roof / walls when xDif <= yDif, walls /
// floor / roof otherwise). Anything else, and the debug hitboxes, take the real calls.
static bool PS1SSFarBlock(Entity *me, Entity *o)
{
#if !RETRO_USE_ORIGINAL_CODE
    if (showHitboxes)
        return false;
#endif
    AnimationFile *a = objectScriptList[o->type].animFile;
    Hitbox *h        = &hitboxList[a->hitboxListOffset + animFrames[animationList[a->aniListOffset + o->animation].frameListOffset + o->frame].hitboxID];
    int oL = h->left[0], oT = h->top[0], oR = h->right[0], oB = h->bottom[0];
    if (oL > 0 || oR < 0 || oT > 0 || oB < 0 || oR - oL < 4)
        return false;
    int thisLeft = (-12 + (me->xpos >> 16)) << 16, thisRight = (12 + (me->xpos >> 16)) << 16;
    int thisTop = (-12 + (me->ypos >> 16)) << 16, thisBottom = (12 + (me->ypos >> 16)) << 16;
    oL <<= 16, oT <<= 16, oR <<= 16, oB <<= 16;
    int rx = o->xpos >> 16 << 16, ry = o->ypos >> 16 << 16;
    if (!(rx + oR < thisLeft || rx + oL > thisRight || ry + oB < thisTop || ry + oT > thisBottom))
        return false;
    int xDif = me->xpos <= rx ? rx - thisRight : thisLeft - rx;
    int yDif = me->ypos <= ry ? ry - thisBottom : thisTop - ry;
    sensors[0].collided = false;
    sensors[1].collided = false;
    sensors[2].collided = false;
    if (xDif <= yDif) { // ... right wall last
        sensors[0].xpos = rx + oL;
        sensors[0].ypos = ry + oT + 0x20000;
    }
    else { // ... roof last
        sensors[0].xpos = rx + oL + 0x20000;
        sensors[0].ypos = ry + oT;
    }
    sensors[1].xpos       = rx + oR - 0x20000;
    sensors[1].ypos       = ry + oB - 0x20000;
    sensors[2].xpos       = rx + oR - 0x20000;
    scriptEng.checkResult = 0;
    return true;
}
static int PS1CollisionTop(Entity &e); // below (VAR_OBJECTCOLLISIONTOP's getter)
static void PS1SSBlockCollide()
{
    int *t   = scriptEng.temp;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    auto setBit = [](int &v, int bit, int on) {
        if (on <= 0)
            v &= ~(1 << bit);
        else
            v |= 1 << bit;
    };
    if (PS1SSFarBlock(&PS1_OBJ(self), &PS1_OBJ(0)))
        return; // both tests miss: cr = 0, sensors as BoxCollision2 leaves them
    BoxCollision2(&PS1_OBJ(self), -12, -12, 12, 12, &PS1_OBJ(0), 65536, 65536, 65536, 65536);
    if (cr != 0) {
        setBit(PS1_OBJ(0).values[11], cr, 1);
    }
    else {
        PS1TouchS1(&PS1_OBJ(self), -10, -10, 10, 10, &PS1_OBJ(0));
        if (cr == 1) {
            Entity &p = PS1_OBJ(0);
            t[0]      = p.xvel;
            t[1]      = p.yvel;
            t[0]      = abs(t[0]);
            t[1]      = abs(t[1]);
            if (t[0] > t[1]) {
                if (p.xvel > 0) {
                    setBit(p.values[11], 2, 1);
                    p.xpos = PS1CollisionLeft(p);
                    p.xpos -= 12;
                    p.xpos <<= 16;
                }
                else {
                    setBit(p.values[11], 3, 1);
                    p.xpos = PS1CollisionRight(p);
                    p.xpos += 12;
                    p.xpos <<= 16;
                }
                p.xpos += PS1_OBJ(self).xpos;
            }
            else {
                if (p.yvel > 0) {
                    setBit(p.values[11], 1, 1);
                    p.ypos = PS1CollisionTop(p);
                    p.ypos -= 12;
                    p.ypos <<= 16;
                }
                else {
                    setBit(p.values[11], 4, 1);
                    p.ypos = PS1CollisionBottom(p);
                    p.ypos += 12;
                    p.ypos <<= 16;
                }
                p.ypos += PS1_OBJ(self).ypos;
            }
        }
    }
}

// Sonic 1's special stage, the coloured blocks' draw sub (Blue / Yellow / Pink / Green: the same sub with two type
// numbers, the opcode's operands a and b): on the flashing steps (LOCAL[2027] / 8 against the property value) the
// block takes type a -- whose sprites the draws then use, as the VM reads the script info at every instruction --, it
// is placed in the rotating maze (function 9, natively), drawn (frame 0 rotated, frame 1, frame 2 + rotation / 8
// rotated), and gets type b back. SSBLOCKDRAW_SIG_S1.
static void PS1SSBlockDraw(int a, int b)
{
    int *t     = scriptEng.temp;
    Entity &me = objectEntityList[objectEntityPos];
    auto rotated = [&](int frame) { // DrawSpriteFX frame FX_ROTATE TEMP0 TEMP1
        ObjectScript *info = &objectScriptList[me.type];
        SpriteFrame *f     = &scriptFrames[info->frameListOffset + frame];
        DrawSpriteRotated(me.direction, (t[0] >> 16) - xScrollOffset, (t[1] >> 16) - yScrollOffset, -f->pivotX, -f->pivotY, f->sprX,
                          f->sprY, f->width, f->height, me.rotation, info->spriteSheetID);
    };
    if (me.propertyValue > 0) {
        t[0] = scriptCode[2027];
        t[0] >>= 3;
        t[1] = me.propertyValue;
        t[1] -= t[0];
        if (t[1] == 1 || t[1] == 3) // switch 1..3: cases 1 and 3
            me.type = a;
    }
    PS1SSRotPos(); // CallFunction 9
    rotated(0);
    {
        ObjectScript *info = &objectScriptList[me.type]; // DrawSpriteXY 1 TEMP0 TEMP1
        SpriteFrame *f     = &scriptFrames[info->frameListOffset + 1];
        DrawSprite((t[0] >> 16) - xScrollOffset + f->pivotX, (t[1] >> 16) - yScrollOffset + f->pivotY, f->width, f->height, f->sprX, f->sprY,
                   info->spriteSheetID);
    }
    t[2] = me.rotation;
    t[2] >>= 3;
    t[2] += 2;
    rotated(t[2]);
    me.type = b;
}
#endif

#if PS1_GAME == 1
// Sonic 1's special stage Ring update sub (stage object "Ring" in Special; 72 on screen at a time): the pickup for
// each player (group 256, ARRAYPOS6), the extra life at 100 / 200 rings, the alternating ring sound, the continue at
// 50. One statement per instruction; SSRING_SIG_S1.
static void PS1SSRingUpdate()
{
    int *ap  = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int *G   = globalVariables;
    int self = objectEntityPos;
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) { // ForEachActive 256, ARRAYPOS6
        ap[6] = players.entityRefs[loop];
        PS1TouchS1(&PS1_OBJ(self), -8, -8, 8, 8, &PS1_OBJ(ap[6]));
        if (cr == 1) {
            PS1_OBJ(self).type = 19;
            Entity &p = PS1_OBJ(ap[6]);
            p.values[0]++;
            if (p.values[0] > 999)
                p.values[0] = 999;
            if (p.values[0] >= G[20]) {
                if (G[0] != 2) {
                    G[23]++;
                    PlaySfx(24, 0);
                    PauseSound();
                    PS1ResetObjectEntity(25, 23, 2, 0, 0);
                    PS1_OBJ(25).priority = 1;
                }
                G[20] += 100;
                if (G[20] >= 300)
                    G[20] = 1000;
            }
            if (scriptCode[3] == 0) { // LOCAL[3]
                if (G[19] == 0) {
                    PlaySfx(1, 0);
                    SetSfxAttributes(1, -1, -100);
                    G[19] = 1;
                }
                else {
                    PlaySfx(2, 0);
                    SetSfxAttributes(2, -1, 100);
                    G[19] = 0;
                }
            }
            if (PS1_OBJ(ap[6]).values[0] == 50) {
                G[24]++;
                PlaySfx(46, 0);
            }
        }
    }
}

// The special stage's coloured blocks' update sub: function 10 (PS1SSBlockCollide) once per player. SSBLOCKUPDATE_SIG_S1.
static void PS1SSBlockUpdate()
{
    int *ap                = scriptEng.arrayPosition;
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) { // ForEachActive 256, ARRAYPOS6
        ap[6] = players.entityRefs[loop];
        PS1SSBlockCollide(); // CallFunction 10
    }
}
#endif

#if PS1_GAME == 1
// FUNC_DRAWSPRITEXY / FUNC_DRAWSPRITEFX for the running entity, for Sonic 1's natives: the VM's bodies with the frame,
// the effect and the position as arguments (the script info read at the call, as the VM does per instruction).
static void PS1S1DrawXY(int frame, int x, int y)
{
    ObjectScript *info = &objectScriptList[objectEntityList[objectEntityPos].type];
    SpriteFrame *f     = &scriptFrames[info->frameListOffset + frame];
    DrawSprite((x >> 16) - xScrollOffset + f->pivotX, (y >> 16) - yScrollOffset + f->pivotY, f->width, f->height, f->sprX, f->sprY,
               info->spriteSheetID);
}
static void PS1S1DrawFX(int frame, int fx, int x, int y)
{
    Entity *e          = &objectEntityList[objectEntityPos];
    ObjectScript *info = &objectScriptList[e->type];
    SpriteFrame *f     = &scriptFrames[info->frameListOffset + frame];
    int sx = (x >> 16) - xScrollOffset, sy = (y >> 16) - yScrollOffset;
    switch (fx) {
        default: break;
        case FX_SCALE:
            DrawSpriteScaled(e->direction, sx, sy, -f->pivotX, -f->pivotY, e->scale, e->scale, f->width, f->height, f->sprX, f->sprY,
                             info->spriteSheetID);
            break;
        case FX_ROTATE:
            DrawSpriteRotated(e->direction, sx, sy, -f->pivotX, -f->pivotY, f->sprX, f->sprY, f->width, f->height, e->rotation,
                              info->spriteSheetID);
            break;
        case FX_ROTOZOOM:
            DrawSpriteRotozoom(e->direction, sx, sy, -f->pivotX, -f->pivotY, f->sprX, f->sprY, f->width, f->height, e->rotation, e->scale,
                               info->spriteSheetID);
            break;
        case FX_INK:
            switch (e->inkEffect) {
                case INK_NONE:
                    DrawSprite(sx + f->pivotX, sy + f->pivotY, f->width, f->height, f->sprX, f->sprY, info->spriteSheetID);
                    break;
                case INK_BLEND:
                    DrawBlendedSprite(sx + f->pivotX, sy + f->pivotY, f->width, f->height, f->sprX, f->sprY, info->spriteSheetID);
                    break;
                case INK_ALPHA:
                    DrawAlphaBlendedSprite(sx + f->pivotX, sy + f->pivotY, f->width, f->height, f->sprX, f->sprY, e->alpha,
                                           info->spriteSheetID);
                    break;
                case INK_ADD:
                    DrawAdditiveBlendedSprite(sx + f->pivotX, sy + f->pivotY, f->width, f->height, f->sprX, f->sprY, e->alpha,
                                              info->spriteSheetID);
                    break;
                case INK_SUB:
                    DrawSubtractiveBlendedSprite(sx + f->pivotX, sy + f->pivotY, f->width, f->height, f->sprX, f->sprY, e->alpha,
                                                 info->spriteSheetID);
                    break;
            }
            break;
        case FX_TINT:
            if (e->inkEffect == INK_ALPHA)
                DrawScaledTintMask(e->direction, sx, sy, -f->pivotX, -f->pivotY, e->scale, e->scale, f->width, f->height, f->sprX, f->sprY,
                                   info->spriteSheetID);
            else
                DrawSpriteScaled(e->direction, sx, sy, -f->pivotX, -f->pivotY, e->scale, e->scale, f->width, f->height, f->sprX, f->sprY,
                                 info->spriteSheetID);
            break;
        case FX_FLIP:
            switch (e->direction) {
                default:
                case FLIP_NONE:
                    DrawSpriteFlipped(sx + f->pivotX, sy + f->pivotY, f->width, f->height, f->sprX, f->sprY, FLIP_NONE, info->spriteSheetID);
                    break;
                case FLIP_X:
                    DrawSpriteFlipped(sx - f->width - f->pivotX, sy + f->pivotY, f->width, f->height, f->sprX, f->sprY, FLIP_X,
                                      info->spriteSheetID);
                    break;
                case FLIP_Y:
                    DrawSpriteFlipped(sx + f->pivotX, sy - f->height - f->pivotY, f->width, f->height, f->sprX, f->sprY, FLIP_Y,
                                      info->spriteSheetID);
                    break;
                case FLIP_XY:
                    DrawSpriteFlipped(sx - f->width - f->pivotX, sy - f->height - f->pivotY, f->width, f->height, f->sprX, f->sprY, FLIP_XY,
                                      info->spriteSheetID);
                    break;
            }
            break;
    }
}

// The special stage's plain draw subs, `CallFunction 9 / DrawSpriteXY <frame> TEMP0 TEMP1`: mode 0 the object's frame
// (Rotate Block, Bumper), mode 1 GLOBAL[18] (Ring). SSPLACEDRAW_SIG_S1 (mode from the frame operand).
static void PS1SSPlaceDraw(int mode)
{
    int *t = scriptEng.temp;
    PS1SSRotPos(); // CallFunction 9
    PS1S1DrawXY(mode ? globalVariables[18] : objectEntityList[objectEntityPos].frame, t[0], t[1]);
}

// Draw subs with a 2-frame animation step (GLOBAL[16] & 15 >> 3 into TEMP2): kind 0 Goal Block (DrawSpriteXY TEMP2),
// 1 Up Down Block (frame 2 on the second step, else its property value), 2 Red White Block (DrawSpriteFX TEMP2 FX_INK).
static void PS1SSAnimDraw(int kind)
{
    int *t = scriptEng.temp;
    PS1SSRotPos(); // CallFunction 9
    t[2] = globalVariables[16];
    t[2] &= 15;
    t[2] >>= 3;
    Entity &me = objectEntityList[objectEntityPos];
    if (kind == 0)
        PS1S1DrawXY(t[2], t[0], t[1]);
    else if (kind == 1) {
        if (t[2] == 0)
            PS1S1DrawXY(me.propertyValue, t[0], t[1]);
        else
            PS1S1DrawXY(2, t[0], t[1]);
    }
    else
        PS1S1DrawFX(t[2], FX_INK, t[0], t[1]);
}

// Gem Block's draw sub: placed (function 9), its direction from LOCAL[2025] / 5 (0 / 1 / 3 / 2 for 0-3), drawn flipped
// (DrawSpriteFX property value FX_FLIP). SSGEMDRAW_SIG_S1.
static void PS1SSGemDraw()
{
    int *t     = scriptEng.temp;
    Entity &me = objectEntityList[objectEntityPos];
    PS1SSRotPos(); // CallFunction 9
    t[2] = scriptCode[2025];
    t[2] /= 5; // Div TEMP2 5 (the divisor is never 0)
    switch (t[2]) {
        case 0: me.direction = 0; break;
        case 1: me.direction = 1; break;
        case 2: me.direction = 3; break;
        case 3: me.direction = 2; break;
        default: break;
    }
    PS1S1DrawFX(me.propertyValue, FX_FLIP, t[0], t[1]);
}
#endif

#if PS1_GAME == 1
// The special stage's other update subs (SSOBJUPDATE_SIGS_S1; the operand says which), all around function 10 (a block
// against the player: PS1SSBlockCollide) in the players' loop (group 256, ARRAYPOS6). One statement per instruction.
// 0 Red White Block: property 0 = solid; 1-5 = a trigger box (its side) that turns it solid (property 0, ink 0).
// 1 Gem Block: its fade-out steps (state 1) and the touch that starts them (the "gem" sound).
// 2 Up Down Block: toggles LOCAL[1] (the maze's speed step) with a 30-frame cool-down on the player.
// 3 Rotate Block: reverses LOCAL[2] (the rotation's direction) and flashes; the same cool-down (value 14).
// 4 Goal Block: stops the player and starts the stage's end (entity 20 = object 22).
// 5 Bumper: bounces the player away from its centre (ATan2), with its 3-frame animation.
static void PS1SSObjUpdate(int kind)
{
    int *t   = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int *G   = globalVariables;
    int self = objectEntityPos;
    TypeGroupList &players = objectTypeGroupList[256];
    auto me = [&]() -> Entity & { return PS1_OBJ(self); };
    auto L  = [](int pos) { return (int)scriptCode[pos]; };
    switch (kind) {
        case 0: // Red White Block
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                if (me().propertyValue == 0) {
                    PS1SSBlockCollide(); // CallFunction 10
                }
                else {
                    // switch OBJECTPROPERTYVALUE 1-5: BoxCollisionTest C_TOUCH with that case's box (else nothing)
                    static const int16_t box[5][4] = { { -160, 36, 160, 60 }, { 36, -160, 60, 160 }, { -60, -160, -36, 160 },
                                                       { -160, -60, 160, -36 }, { 36, -24, 60, 24 } };
                    int k = me().propertyValue;
                    if (k >= 1 && k <= 5) {
                        const int16_t *b = box[k - 1];
                        PS1TouchS1(&PS1_OBJ(self), b[0], b[1], b[2], b[3], &PS1_OBJ(ap[6]));
                        if (cr == 1) {
                            me().propertyValue = 0;
                            me().inkEffect     = 0;
                        }
                    }
                }
            }
            break;
        case 1: // Gem Block
            if (me().state == 1) {
                me().propertyValue = me().values[0];
                me().propertyValue = me().propertyValue >> 1;
                me().propertyValue &= 3;
                me().values[0]++;
                if (me().values[0] == 16) {
                    me().priority      = 0;
                    me().values[0]     = 0;
                    me().propertyValue = me().values[1];
                    me().propertyValue++;
                    if (me().propertyValue == 4)
                        me().type = 0;
                    else
                        me().state = 0;
                    PS1ScriptWrite(2026, 0); // LOCAL[2026]
                }
                for (int loop = 0; loop < players.listSize; ++loop) {
                    ap[6] = players.entityRefs[loop];
                    PS1SSBlockCollide();
                }
            }
            else {
                for (int loop = 0; loop < players.listSize; ++loop) {
                    ap[6] = players.entityRefs[loop];
                    PS1SSBlockCollide();
                    if (L(2026) == 0) {
                        if (cr > 0) {
                            me().state     = 1;
                            me().priority  = 1;
                            PS1ScriptWrite(2026, 1);
                            me().values[1] = me().propertyValue;
                            if (L(3) == 0)
                                PlaySfx(43, 0);
                        }
                    }
                }
            }
            break;
        case 2: // Up Down Block
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                PS1SSBlockCollide();
                if (cr != 0) {
                    Entity &p = PS1_OBJ(ap[6]);
                    if (p.values[15] == 0) {
                        p.values[15] = 30;
                        if (me().propertyValue == 0) {
                            if (L(1) < 1) {
                                PS1ScriptWrite(1, L(1) + 1);
                                me().propertyValue = 1;
                                PlaySfx(42, 0);
                            }
                        }
                        else {
                            if (L(1) > 0) {
                                PS1ScriptWrite(1, L(1) - 1);
                                me().propertyValue = 0;
                                PlaySfx(42, 0);
                            }
                        }
                    }
                }
            }
            break;
        case 3: // Rotate Block
            if (me().state == 1) {
                me().frame = me().values[0];
                me().frame = me().frame >> 3;
                me().values[0]++;
                if (me().values[0] == 32) {
                    me().values[0] = 0;
                    me().state     = 0;
                    me().frame     = 0;
                }
            }
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                PS1SSBlockCollide();
                if (cr != 0) {
                    Entity &p = PS1_OBJ(ap[6]);
                    if (p.values[14] == 0) {
                        p.values[14] = 30;
                        me().state   = 1;
                        PS1ScriptWrite(2, L(2) ^ 1);
                        PlaySfx(42, 0);
                    }
                }
            }
            break;
        case 4: // Goal Block
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                PS1SSBlockCollide();
                if (cr > 0) {
                    Entity &p            = PS1_OBJ(ap[6]);
                    p.state              = 1;
                    p.xvel               = 0;
                    p.yvel               = 0;
                    p.speed              = 0;
                    me().values[0]       = 0;
                    p.objectInteractions = 0;
                    PS1ResetObjectEntity(20, 22, 0, 0, 0);
                    PS1_OBJ(20).priority = 1;
                    PlaySfx(45, 0);
                    timeEnabled = 0;
                    G[7]        = 0;
                }
            }
            break;
        case 5: // Bumper
            if (me().state > 0) {
                me().frame = me().values[0];
                me().frame = PS1ScriptDiv(me().frame, 5);
                me().frame++;
                me().values[0]++;
                if (me().values[0] > 22) {
                    me().values[0] = 0;
                    me().state     = 0;
                    me().frame     = 0;
                }
            }
            if (PS1ObjectOutOfBounds(&me()) == 1)
                me().priority = 0;
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                PS1SSBlockCollide();
                PS1TouchS1(&PS1_OBJ(self), -14, -14, 14, 14, &PS1_OBJ(ap[6]));
                if (cr == 1) {
                    if (me().state == 0)
                        PlaySfx(41, 0);
                    if (me().values[0] > 5)
                        PlaySfx(41, 0);
                    me().state    = 1;
                    me().priority = 1;
                    Entity &p     = PS1_OBJ(ap[6]);
                    t[0]          = p.xpos;
                    t[0] -= me().xpos;
                    t[1] = p.ypos;
                    t[1] -= me().ypos;
                    t[2] = ArcTanLookup(t[0], t[1]);
                    t[0] = Cos256(t[2]);
                    t[1] = Sin256(t[2]);
                    t[0] *= 1792;
                    t[1] *= 1792;
                    p.values[1]  = 0;
                    p.values[12] = t[0];
                    p.values[13] = t[1];
                    p.speed      = me().xvel;
                    p.gravity    = 1;
                }
            }
            break;
    }
}
#endif

#if PS1_GAME == 1
// Sonic 1's zone objects' update subs (docs/37 phase 2b; S1ZONEOBJ_SIGS, the operand says which), one statement per
// instruction, the players' loops over group 256 (ARRAYPOS6):
// 0 Labyrinth's Door: its state switch (0 wait for the switch -- entity + 1's value 0 -- , 1 rise, 2 open, 3 wait for
//   the player past x 4384, 4 close), then solid for each player.
// 1 Labyrinth's Door Horizontal: 3 falls into 0 into 2 (LOCAL[56375] opens it, the switch at entity + 1), 1 slides (the
//   players standing on it move with it), each state solid for the players.
// 2 Invisible Block (Zones 02-06): solid from above / pushing from the sides by its state; crushed from below runs
//   GlobalCode function 51 (the death) in the VM from the players' loop (foreach depth 1).
static void PS1S1ZoneObj(int kind)
{
    int *t   = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    TypeGroupList &players = objectTypeGroupList[256];
    auto me = [&]() -> Entity & { return PS1_OBJ(self); };
    auto L  = [](int pos) { return (int)scriptCode[pos]; };
    switch (kind) {
        case 0: // Door
            switch (me().state) {
                case 0:
                    if (me().propertyValue == 2) {
                        if (PS1_OBJ(0).xpos < me().xpos)
                            PS1ScriptWrite(59874, 4); // LOCAL[59874]
                    }
                    if (PS1_OBJ(self + 1).values[0] == 1) // OBJECTVALUE0[2,0,1]: entity + 1
                        me().state++;
                    break;
                case 1:
                    me().ypos -= 131072;
                    me().values[0]--;
                    if (me().values[0] < 0) {
                        me().state++;
                        if (me().propertyValue == 1)
                            me().state++;
                    }
                    break;
                case 2: break;
                case 3:
                    if (PS1_OBJ(0).xpos > 287309824)
                        me().state++;
                    break;
                case 4:
                    me().ypos += 131072;
                    me().values[0]++;
                    if (me().values[0] >= 32)
                        me().state = 0;
                    break;
                default: break;
            }
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                BoxCollision(&PS1_OBJ(self), -8, -32, 8, 32, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536); // C_SOLID
            }
            break;
        case 1: { // Door Horizontal
            int st = me().state;
            if (st == 3) {
                if (L(56375) == 1)
                    me().state = 1;
            }
            if (st == 3 || st == 0) {
                if (PS1_OBJ(self + 1).values[0] == 1)
                    me().state++;
            }
            if (st == 3 || st == 0 || st == 2) {
                for (int loop = 0; loop < players.listSize; ++loop) {
                    ap[6] = players.entityRefs[loop];
                    BoxCollision(&PS1_OBJ(self), -64, -16, 64, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                }
            }
            else if (st == 1) {
                t[0] = me().xpos;
                t[0] &= -65536;
                if (me().direction == 0)
                    me().xpos -= 131072;
                else
                    me().xpos += 131072;
                me().values[0]--;
                if (me().values[0] < 0)
                    me().state++;
                me().values[1] = me().xpos;
                me().values[1] &= -65536;
                me().values[1] -= t[0];
                t[1]      = me().xpos;
                me().xpos = t[0];
                for (int loop = 0; loop < players.listSize; ++loop) {
                    ap[6] = players.entityRefs[loop];
                    BoxCollision(&PS1_OBJ(self), -64, -16, 64, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                    if (cr == 1)
                        PS1_OBJ(ap[6]).xpos += me().values[1];
                }
                me().xpos = t[1];
            }
            break;
        }
        case 2: // Invisible Block
            t[0] = me().values[0];
            t[0] = -t[0];
            t[1] = me().values[1];
            t[1] = -t[1];
            switch (me().state) {
                case 0:
                    for (int loop = 0; loop < players.listSize; ++loop) {
                        ap[6] = players.entityRefs[loop];
                        if (PS1_OBJ(ap[6]).state != 24) {
                            BoxCollision(&PS1_OBJ(self), t[0], t[1], me().values[0], me().values[1], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                            switch (cr) {
                                case 0:
                                    t[0] += 2;
                                    t[1] += 2;
                                    t[2] = me().values[0];
                                    t[3] = me().values[1];
                                    t[2] -= 2;
                                    t[3] -= 2;
                                    TouchCollision(&PS1_OBJ(self), t[0], t[1], t[2], t[3], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                                    if (cr == 1)
                                        PS1_OBJ(ap[6]).gravity = 0;
                                    break;
                                case 4:
                                    if (PS1_OBJ(ap[6]).gravity == 0)
                                        PS1CallScriptFunction(51, 1); // CallFunction 51 (the player's death)
                                    break;
                                default: break;
                            }
                        }
                    }
                    break;
                case 1:
                    for (int loop = 0; loop < players.listSize; ++loop) {
                        ap[6] = players.entityRefs[loop];
                        if (PS1_OBJ(ap[6]).state != 24) {
                            TouchCollision(&PS1_OBJ(self), t[0], t[1], me().values[0], me().values[1], &PS1_OBJ(ap[6]), 65536, 65536, 65536,
                                           65536);
                            if (cr == 1) {
                                Entity &p = PS1_OBJ(ap[6]);
                                if (p.gravity == 0) {
                                    p.xpos = PS1CollisionRight(p);
                                    p.xpos = -p.xpos;
                                    p.xpos -= me().values[0];
                                    p.xpos <<= 16;
                                    p.xpos += me().xpos;
                                    if (p.speed > 0)
                                        p.speed = 0;
                                }
                            }
                        }
                    }
                    break;
                case 2:
                    for (int loop = 0; loop < players.listSize; ++loop) {
                        ap[6] = players.entityRefs[loop];
                        if (PS1_OBJ(ap[6]).state != 24) {
                            TouchCollision(&PS1_OBJ(self), t[0], t[1], me().values[0], me().values[1], &PS1_OBJ(ap[6]), 65536, 65536, 65536,
                                           65536);
                            if (cr == 1) {
                                Entity &p = PS1_OBJ(ap[6]);
                                if (p.gravity == 0) {
                                    p.xpos = PS1CollisionLeft(p);
                                    p.xpos = -p.xpos;
                                    p.xpos += me().values[0];
                                    p.xpos <<= 16;
                                    p.xpos += me().xpos;
                                    if (p.speed < 0)
                                        p.speed = 0;
                                }
                            }
                        }
                    }
                    break;
                default: break;
            }
            break;
    }
}
#endif

#if PS1_GAME == 1
// Sonic 1's player input / timers (GlobalCode function 0, docs/37 phase 2b), like PS1PlayerInput: Sonic 2's function
// without the 2P VS parts (no GLOBAL[13] checks, no sums over both players), with Sonic 1's globals (97 / 98 / 102 / 95
// / 39) and functions (42, 38); its pause blocks stop SFX 0. The mobile back key's pause is patched off before this
// (patch_back_pause s1: its `IfEqual KEYDOWNBUTTONB 2` is never true). Patched in when PLAYERINPUT_FN_SIG_S1 matches.
static void PS1PlayerInputS1()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr = scriptEng.checkResult;
    int *G  = globalVariables;
    auto E  = []() -> Entity & { return objectEntityList[objectEntityPos]; };
    auto ic = []() { return !(forceUseScripts || Engine.usingOrigins) || objectEntityPos <= 1; }; // inputCheck
    auto L  = [](int pos) { return (int)scriptCode[pos]; };
    auto touch = [&](int l, int tp, int r, int b) { // FUNC_CHECKTOUCHRECT
        cr = -1;
#if !RETRO_USE_ORIGINAL_CODE
        AddDebugHitbox(H_TYPE_FINGER, NULL, l, tp, r, b);
#endif
        for (int f = 0; f < touches; ++f) {
            if (touchDown[f] && touchX[f] > l && touchX[f] < r && touchY[f] > tp && touchY[f] < b)
                cr = f;
        }
    };
    if (G[5] == 0) {
        if (E().controlMode == 0) {
            touch(0, 96, SCREEN_CENTERX, SCREEN_YSIZE);
            if (cr > -1) {
                ap[0] = cr;
                t[0]  = touchX[ap[0]];
                t[0] -= saveRAM[39];
                t[1] = touchY[ap[0]];
                t[1] -= saveRAM[40];
                t[2] = ArcTanLookup(t[0], t[1]);
                t[2] += 32;
                t[2] &= 255;
                t[2] >>= 6;
                switch (t[2]) {
                    case 0:
                        if (ic())
                            keyDown.right = 1;
                        break;
                    case 1:
                        if (ic())
                            keyDown.down = 1;
                        break;
                    case 2:
                        if (ic())
                            keyDown.left = 1;
                        break;
                    case 3:
                        if (ic())
                            keyDown.up = 1;
                        break;
                }
            }
            touch(SCREEN_CENTERX, 96, SCREEN_XSIZE, 240);
            if (cr > -1) {
                if (ic())
                    keyDown.A = 1;
            }
            if (G[97] == 0) { // Or KEYPRESSBUTTONA KEYDOWNBUTTONA
                int a = keyPress.A && ic();
                a |= keyDown.A && ic();
                if (ic())
                    keyPress.A = a;
            }
            G[97] = keyDown.A && ic();
            if (debugMode == 1) {
                touch(0, 0, 112, 56);
                if (cr > -1) {
                    if (ic())
                        keyDown.B = 1;
                }
                if (G[98] == 0) {
                    int b = keyPress.B && ic();
                    b |= keyDown.B && ic();
                    if (ic())
                        keyPress.B = b;
                }
                G[98] = keyDown.B && ic();
            }
            touch(240, 0, SCREEN_XSIZE, 40);
            if (cr > -1) {
                PlaySfx(23, 0);
                StopSfx(19);
                StopSfx(0);
                Engine.gameMode = 5;
            }
            if ((keyPress.start && ic()) == 1) {
                PlaySfx(23, 0);
                StopSfx(19);
                StopSfx(0);
                Engine.gameMode = 5;
            }
            // IfEqual KEYDOWNBUTTONB 2: the mobile back key's pause, its 1 patched to 2 (never true)
        }
        ProcessObjectControl(&E());
    }
    else {
        if (G[102] == 0) {
            touch(0, 0, SCREEN_XSIZE, SCREEN_YSIZE);
            if ((keyPress.start && ic()) == 1)
                cr = 0;
            if (cr > -1) {
                if (L(12) > 1)
                    PS1ScriptWrite(12, 1);
            }
            if ((keyPress.start && ic()) == 1) {
                if (L(12) > 1)
                    PS1ScriptWrite(12, 1);
            }
        }
        if (E().controlMode == 0) {
            PS1ScriptWrite(11, L(11) - 1);
            if (L(11) < 1) {
                if (L(9) < L(10)) {
                    if (L(9) >= 0 && L(9) < scriptCode[L(8)]) // GetTableValue TEMP0 LOCAL9 LOCAL8
                        t[0] = scriptCode[L(8) + L(9) + 1];
                    E().up        = (t[0] & (1 << 0)) >> 0;
                    E().down      = (t[0] & (1 << 1)) >> 1;
                    E().left      = (t[0] & (1 << 2)) >> 2;
                    E().right     = (t[0] & (1 << 3)) >> 3;
                    E().jumpPress = (t[0] & (1 << 4)) >> 4;
                    E().jumpHold  = (t[0] & (1 << 5)) >> 5;
                    PS1ScriptWrite(9, L(9) + 1);
                    if (L(9) >= 0 && L(9) < scriptCode[L(8)]) // GetTableValue LOCAL11 LOCAL9 LOCAL8
                        PS1ScriptWrite(11, scriptCode[L(8) + L(9) + 1]);
                    PS1ScriptWrite(9, L(9) + 1);
                }
            }
            else {
                if (E().jumpPress == 1)
                    E().jumpPress = 0;
            }
            if (L(12) > 0) {
                PS1ScriptWrite(12, L(12) - 1);
                if (L(12) < 1) {
                    PS1ResetObjectEntity(11, 6, 0, 0, 0);
                    PS1_OBJ(11).state     = 8;
                    PS1_OBJ(11).priority  = 1;
                    PS1_OBJ(11).drawOrder = 6;
                    E().values[7]         = 80;
                    cameraEnabled         = 0;
                }
            }
        }
    }
    if (E().values[6] > 0) {
        E().values[6]--;
        if (E().values[6] < 1) {
            ap[6] = objectEntityPos;
            PS1CallScriptFunction(42, 0);
            if (G[95] != 0)
                PS1CallScriptFunction(G[95], 0);
            E().values[6] = 0;
        }
    }
    if (E().state != 26) {
        if (E().values[8] > 0) {
            E().values[8]--;
            t[0] = (E().values[8] & (1 << 2)) >> 2;
            if (t[0] == 1)
                E().visible = 0;
            else
                E().visible = 1;
        }
    }
    if (E().values[7] > 0) {
        E().values[7]--;
        if (E().values[7] == 0) {
            if (trackID == 2)
                PlayMusic(0, 0);
            if (PS1_OBJ(objectEntityPos + ap[7]).type == G[39]) { // OBJECTTYPE[2,1,7]: entity (this + arrayPos7)
                ap[6] = objectEntityPos;
                ap[0] = ap[6];
                ap[0] += ap[7];
                PS1CallScriptFunction(38, 0);
            }
        }
    }
    if (E().state != 15) {
        if (E().state != 16) {
            if (E().lookPosY > 0)
                E().lookPosY -= 2;
            if (E().lookPosY < 0)
                E().lookPosY += 2;
        }
    }
    if (E().values[11] > 0) {
        E().values[11]--;
        if (E().values[11] == 0) {
            if (objectEntityPos == cameraTarget)
                cameraStyle = 0;
        }
    }
    if (E().state != 18) {
        if (E().values[26] != 0) {
            StopSfx(19);
            StopSfx(20);
            E().values[26] = 0;
        }
    }
}
#endif

// Sonic 2's Spikes (GlobalCode object "Spikes"; update 121 VM instructions, ~3 hblanks a frame each), natively: one
// statement per script instruction, in order. The moving spikes' states (switch OBJECTSTATE 1-5), then the player
// loop of their orientation (switch OBJECTPROPERTYVALUE 0-3); hurting (function 49) and killing (function 50) run in
// the VM from inside the loop (foreach depth 1). Patched in only when the sub and its jump-table entries match
// SPIKES_SIG.
static void PS1SpikesUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    auto rise  = [&]() { // states 1 and 3 (the same instructions)
        if (v[0] < 60) {
            v[0]++;
        }
        else {
            me.state++;
            v[0] = 0;
            t[0] = SCREEN_CENTERX;
            t[0] += 64;
            t[1] = t[0];
            t[0] = -t[0];
            TouchCollision(&PS1_OBJ(self), t[0], -128, t[1], 128, &PS1_OBJ(0), 0, 0, 0, 0);
            if (cr == 1)
                PlaySfx(29, 0);
        }
    };
    switch (me.state) {
        case 1: rise(); break;
        case 2:
            if (v[1] < 2097152) {
                v[1] += 524288;
                me.xpos += me.xvel;
                me.ypos += me.yvel;
            }
            else {
                me.state++;
            }
            break;
        case 3: rise(); break;
        case 4:
            if (v[1] > 0) {
                v[1] -= 524288;
                me.xpos -= me.xvel;
                me.ypos -= me.yvel;
            }
            else {
                me.state = 1;
            }
            break;
        case 5:
            ap[0]   = v[2];
            me.ypos = PS1_OBJ(self - ap[0]).ypos; // OBJECTYPOS[3,1,0]: entity (this - arrayPos0)
            break;
    }
    TypeGroupList &players = objectTypeGroupList[256];
    auto solidKill         = [&](int l, int tp, int r, int b) {
        BoxCollision(&PS1_OBJ(self), l, tp, r, b, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
        if (cr == 4) {
            if (PS1_OBJ(ap[6]).gravity == 0)
                PS1CallScriptFunction(50, 1);
        }
    };
    auto touchHurt = [&](int l, int tp, int r, int b) {
        TouchCollision(&PS1_OBJ(self), l, tp, r, b, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
        if (cr == 1)
            PS1CallScriptFunction(49, 1);
    };
    switch (me.propertyValue) {
        case 0:
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                solidKill(-16, -16, 16, 16);
                if (PS1_OBJ(ap[6]).yvel > -1)
                    touchHurt(-15, -17, 15, -12);
            }
            break;
        case 1:
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                solidKill(-16, -16, 15, 16);
                if (PS1_OBJ(ap[6]).xvel <= 0)
                    touchHurt(12, -15, 16, 15);
            }
            break;
        case 2:
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                solidKill(-15, -16, 16, 16);
                touchHurt(-16, -15, -12, 15);
            }
            break;
        case 3:
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                t[0]  = PS1_OBJ(ap[6]).yvel;
                BoxCollision(&PS1_OBJ(self), -16, -16, 16, 15, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                if (cr == 4) {
                    if (PS1_OBJ(ap[6]).gravity == 0) {
                        if (PS1_OBJ(ap[6]).collisionMode == 0) {
                            PS1CallScriptFunction(50, 1);
                            PS1_OBJ(ap[6]).values[7] = 0;
                        }
                        else {
                            PS1CallScriptFunction(49, 1);
                        }
                    }
                    else {
                        PS1CallScriptFunction(49, 1);
                    }
                    t[0] = 1;
                }
                if (t[0] < 1)
                    touchHurt(-15, 12, 15, 14);
            }
            break;
    }
}

// FUNC_CREATETEMPOBJECT's body (the new entity in arrayPos8).
static void PS1CreateTempObject(int type, int prop, int x, int y)
{
    int *ap = scriptEng.arrayPosition;
    if (objectEntityList[ap[8]].type > OBJ_TYPE_BLANKOBJECT && ++ap[8] == ENTITY_COUNT)
        ap[8] = TEMPENTITY_START;
    Entity *temp = &objectEntityList[ap[8]];
    memset(temp, 0, sizeof(Entity));
    temp->type               = type;
    temp->propertyValue      = prop;
    temp->xpos               = x;
    temp->ypos               = y;
    temp->direction          = FLIP_NONE;
    temp->priority           = PRIORITY_ACTIVE;
    temp->drawOrder          = 3;
    temp->scale              = 512;
    temp->inkEffect          = INK_NONE;
    temp->objectInteractions = true;
    temp->visible            = true;
    temp->tileCollisions     = true;
}
// GlobalCode function 45 (push the player in arrayPos6 off the running object), as its instructions.
static void PS1PushPlayerOff(Entity &me)
{
    int *ap   = scriptEng.arrayPosition;
    Entity &p = PS1_OBJ(ap[6]);
    if (p.state != 28) {
        ap[0] = ap[6];
        ap[0] += ap[7];
        if (p.values[7] == 0) {
            if (p.values[8] == 0) {
                p.state = 26;
                if (p.xpos > me.xpos)
                    p.speed = 131072;
                else
                    p.speed = -131072;
            }
        }
    }
}

// Mystic Cave's C Ledge (stage object in Zone06, the crumbling ledges; update 58 VM instructions, ~1.5 hblanks a
// frame each, 8 on screen), natively: one statement per script instruction, in order. Patched in only when the sub
// and its jump-table entries match CLEDGE_SIG.
static void PS1CLedgeUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    auto get   = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    switch (me.state) { // cases 1, 2, 4, 5 (3 goes to the end)
        case 1:
            if (v[0] < 10) {
                v[0]++;
            }
            else {
                t[0] = 0;
                while (t[0] < 6) {
                    get(65802, t[0], t[1]);
                    PS1CreateTempObject(43, t[1], me.xpos, me.ypos);
                    PS1_OBJ(ap[8]).state     = 4;
                    PS1_OBJ(ap[8]).direction = me.direction;
                    get(65809, t[0], PS1_OBJ(ap[8]).values[1]);
                    t[0]++;
                }
                PlaySfx(44, 0);
                v[0] = 0;
                me.state++;
            }
            break;
        case 2:
            if (v[0] < 20) {
                v[0]++;
            }
            else {
                v[0] = 0;
                me.state++;
            }
            break;
        case 4:
            if (v[0] < v[1]) {
                v[0]++;
            }
            else {
                v[0] = 0;
                me.state++;
            }
            break;
        case 5:
            me.ypos += me.yvel;
            me.yvel += 16384;
            if (PS1ObjectOutOfBounds(&me) == 1)
                me.type = 0;
            break;
    }
    if (me.state < 4) {
        if (PS1ObjectOutOfBounds(&me) == 1) {
            me.state    = 0;
            v[0]        = 0;
            me.priority = 0;
        }
    }
    if (me.state < 3) {
        TypeGroupList &players = objectTypeGroupList[256];
        for (int loop = 0; loop < players.listSize; ++loop) {
            ap[6] = players.entityRefs[loop];
            PlatformCollision(&PS1_OBJ(self), -32, -24, 32, -8, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 1) {
                if (me.state == 0) {
                    me.state    = 1;
                    me.priority = 1;
                }
            }
        }
    }
}

// Metropolis's Steam Piston (stage object in Zone09; update 95 VM instructions, ~6 hblanks a frame each), natively:
// one statement per script instruction, in order; GlobalCode function 45 inline (PS1PushPlayerOff). Patched in only
// when the sub (with its jump-table entries) and function 45 match STEAMPISTON_SIG / PUSHPLAYER_FN_SIG.
static void PS1SteamPistonUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    t[7]       = 0;
    switch (me.state) {
        case 0:
            t[0] = G[17];
            t[0] &= 127;
            if (t[0] == 0) {
                me.priority = 1;
                v[0]        = 2;
                me.state    = 1;
            }
            break;
        case 1:
            me.ypos -= 524288;
            t[7] = -524288;
            v[0]--;
            if (v[0] == 0) {
                me.state     = 2;
                me.frame     = 0;
                me.animation = 1;
            }
            break;
        case 2:
            t[0] = G[17];
            t[0] &= 63;
            if (t[0] == 0) {
                me.priority = 1;
                v[0]        = 2;
                me.state    = 3;
            }
            break;
        case 3:
            me.ypos += 524288;
            t[7] = 524288;
            v[0]--;
            if (v[0] == 0)
                me.state = 0;
            break;
    }
    if (me.animation == 1) {
        me.frame = me.animationTimer;
        me.frame = me.frame >> 3;
        me.animationTimer++;
        if (me.frame == 7) {
            me.animationTimer = 0;
            me.animation      = 0;
        }
    }
    t[2]                   = 0;
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        t[0]      = (v[2] & (1 << t[2])) >> t[2];
        if (t[0] == 1)
            p.ypos += t[7];
        v[2] &= ~(1 << t[2]);
        BoxCollision(&PS1_OBJ(self), -16, -16, 16, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
        if (cr == 1) {
            v[2] |= 1 << t[2];
            if (me.state == 1) {
                p.yvel = -655360;
                p.yvel -= p.values[25];
                p.state          = 12;
                p.tileCollisions = 1;
                p.gravity        = 1;
                p.values[1]      = 0;
                p.collisionMode  = 0;
                p.pushing        = 0;
                p.animation      = G[90];
                p.prevAnimation  = G[90];
                p.frame          = 0;
                p.animationSpeed = 40;
                PlaySfx(11, 0);
            }
        }
        else {
            if (me.frame == 3) {
                TouchCollision(&PS1_OBJ(self), -50, -4, -30, 4, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                if (cr == 1) {
                    PS1PushPlayerOff(me);
                }
                else {
                    TouchCollision(&PS1_OBJ(self), 30, -4, 50, 4, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                    if (cr == 1)
                        PS1PushPlayerOff(me);
                }
            }
        }
        t[2]++;
    }
    if (PS1ObjectOutOfBounds(&me) == 1) {
        me.ypos = v[1];
        me.ypos += v[31];
        v[0]              = 0;
        v[2]              = 0;
        me.animation      = 0;
        me.animationTimer = 0;
        me.priority       = 0;
        me.state          = 0;
    }
}

// The player's physics functions (GlobalCode functions 2-6 and 51-53, run by the player states every frame for each
// player), natively: one statement per script instruction, in order, on the running entity. Each function's body is
// patched to `op / return` only when all of them match PLAYERFN*_SIG (patch_bytecode.py patch_player_physics).
static int PS1CollisionTop(Entity &e) // VAR_OBJECTCOLLISIONTOP's getter
{
    AnimationFile *animFile = objectScriptList[e.type].animFile;
    if (!animFile)
        return 0;
    int h = animFrames[animationList[animFile->aniListOffset + e.animation].frameListOffset + e.frame].hitboxID;
    return hitboxList[animFile->hitboxListOffset + h].top[0];
}
// The player scripts Sonic 1 and Sonic 2 share (docs/37 phase 2b): Sonic 1's GlobalCode has the same physics functions
// but numbers a few things differently (its animation globals are 3 lower, its player states from 11 on one lower); the
// natives name those numbers. Player states are GlobalCode function numbers (CallFunction OBJECTSTATE).
#if PS1_GAME == 1
#define PS1_ANI_JUMPING   64 // G[] index of the jumping (rolling ball) animation
#define PS1_ST_AIR        11 // in the air
#define PS1_ST_ROLL       13 // rolling
#define PS1_ST_ROLLJUMP   14 // jumped while rolling
#else
#define PS1_ANI_JUMPING   67
#define PS1_ST_AIR        12
#define PS1_ST_ROLL       14
#define PS1_ST_ROLLJUMP   15
#endif
static void PS1PlayerFn51() // walking animation speed from the ground speed (value 5)
{
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    if (me.propertyValue == 1) {
        v[5] = 120;
    }
    else {
        v[5] = me.speed;
        if (v[5] < 0)
            v[5] = -v[5];
        v[5] *= 240;
        v[5] = PS1ScriptDiv(v[5], 393216);
        v[5] += 48;
    }
}
static void PS1PlayerFn52()
{
    Entity &me        = objectEntityList[objectEntityPos];
    me.animationSpeed = me.speed;
    if (me.animationSpeed < 0)
        me.animationSpeed = -me.animationSpeed;
    me.animationSpeed *= 60;
    me.animationSpeed = PS1ScriptDiv(me.animationSpeed, 393216);
    me.animationSpeed += 20;
}
static void PS1PlayerFn53()
{
    Entity &me        = objectEntityList[objectEntityPos];
    me.animationSpeed = me.speed;
    if (me.animationSpeed < 0)
        me.animationSpeed = -me.animationSpeed;
    me.animationSpeed *= 80;
    me.animationSpeed = PS1ScriptDiv(me.animationSpeed, 393216);
}
static void PS1PlayerFn5() // ground speed -> x / y velocity along the floor angle
{
    int *t             = scriptEng.temp;
    Entity &me         = objectEntityList[objectEntityPos];
    me.scrollTracking = 0;
    t[0]               = Cos256(me.angle);
    t[0] *= me.speed;
    t[0] >>= 8;
    me.xvel = t[0];
    t[0]    = Sin256(me.angle);
    t[0] *= me.speed;
    t[0] >>= 8;
    me.yvel = t[0];
}
static void PS1PlayerFn3() // air control
{
    int *t     = scriptEng.temp;
    int *G     = globalVariables;
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    if (me.yvel > -262144) {
        if (me.yvel < 0) {
            t[0] = me.speed;
            t[0] >>= 5;
            me.speed -= t[0];
        }
    }
    t[0] = v[20];
    t[0] = -t[0];
    if (me.speed > t[0]) {
        if (me.left == 1) {
            me.speed -= v[23];
            me.direction = 1;
        }
    }
    else {
        if (me.left == 1)
            me.direction = 1;
    }
    if (me.speed < v[20]) {
        if (me.right == 1) {
            me.speed += v[23];
            me.direction = 0;
        }
    }
    else {
        if (me.right == 1)
            me.direction = 0;
    }
    if (G[9] == 1) {
        if (me.left == 1) {
            t[0] = v[20];
            t[0] = -t[0];
            if (me.speed < t[0])
                me.speed = t[0];
        }
        if (me.right == 1) {
            if (me.speed > v[20])
                me.speed = v[20];
        }
    }
}
static void PS1PlayerFn4() // air movement: gravity, jump release, rotation
{
    int *t     = scriptEng.temp;
    int *G     = globalVariables;
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    me.scrollTracking = 1;
    me.yvel += v[25];
    if (me.yvel < v[28]) {
        if (me.jumpHold == 0) {
            if (v[1] > 0) {
                me.yvel = v[28];
                t[0]    = me.speed;
                t[0] >>= 5;
                me.speed -= t[0];
            }
        }
    }
    me.xvel = me.speed;
    if (me.rotation < 256) {
        if (me.rotation > 0)
            me.rotation -= 4;
        else
            me.rotation = 0;
    }
    else {
        if (me.rotation < 512)
            me.rotation += 4;
        else
            me.rotation = 0;
    }
    me.collisionMode = 0;
    if (me.animation == G[PS1_ANI_JUMPING])
        me.animationSpeed = v[5];
}
static void PS1PlayerFn6() // the jump (roof check first)
{
    int *t     = scriptEng.temp;
    int &cr    = scriptEng.checkResult;
    int *G     = globalVariables;
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    t[1]       = 0;
    if (me.collisionMode == 0) {
        t[6] = me.xpos;
        t[7] = me.ypos;
        t[0] = PS1CollisionTop(me);
        t[0] -= 2;
        ObjectRoofCollision(0, t[0] - 1, me.collisionPlane); // ObjectTileCollision CSIDE_ROOF
        t[1]    = cr;
        me.xpos = t[6];
        me.ypos = t[7];
        t[0]    = PS1CollisionBottom(me);
        if (me.animation != G[PS1_ANI_JUMPING]) {
            me.ypos = ((me.ypos >> 16) - v[30]) << 16; // Sub OBJECTIYPOS OBJECTVALUE30 (write-back)
            t[0] += v[30];
        }
        ObjectFloorCollision(0, t[0], me.collisionPlane); // ObjectTileCollision CSIDE_FLOOR
    }
    if (t[1] == 0) {
        me.controlLock = 0;
        me.gravity     = 1;
        t[1]           = v[27];
        t[1] += v[25];
        me.xvel = Sin256(me.angle);
        me.xvel *= t[1];
        t[0] = Cos256(me.angle);
        t[0] *= me.speed;
        me.xvel += t[0];
        me.xvel >>= 8;
        me.yvel = Sin256(me.angle);
        me.yvel *= me.speed;
        t[0] = Cos256(me.angle);
        t[0] *= t[1];
        me.yvel -= t[0];
        me.yvel >>= 8;
        me.speed          = me.xvel;
        me.scrollTracking = 1;
        me.animation      = G[PS1_ANI_JUMPING];
        me.angle          = 0;
        me.collisionMode  = 0;
        v[1]              = 1;
        PS1PlayerFn51(); // CallFunction 51 (Sonic 1: 52)
        if (me.state == PS1_ST_ROLL)
            me.state = PS1_ST_ROLLJUMP;
        else
            me.state = PS1_ST_AIR;
        PlaySfx(0, 0);
        v[34] = 1;
        v[35] = 1;
    }
}
static void PS1PlayerFn2() // ground acceleration / deceleration, slopes, falling off walls
{
    int *t     = scriptEng.temp;
    int *G     = globalVariables;
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    auto slope = [&]() {
        t[0] = Sin256(me.angle);
        t[0] *= 8192;
        t[0] >>= 8;
        me.speed += t[0];
    };
    auto fall = [&]() {
        me.gravity       = 1;
        me.angle         = 0;
        me.collisionMode = 0;
        me.speed         = me.xvel;
    };
    if (me.controlLock > 0) {
        me.controlLock--;
        slope();
    }
    else {
        if (me.left == 1) {
            t[0] = v[20];
            t[0] = -t[0];
            if (me.speed > t[0]) {
                if (me.speed > 0) {
                    if (me.collisionMode == 0) {
                        if (me.speed > 262144)
                            v[14] = 16;
                    }
                    if (me.speed < v[9]) {
                        me.speed = v[9];
                        me.speed = -me.speed;
                        v[14]    = 0;
                    }
                    else {
                        me.speed -= v[9];
                    }
                }
                else {
                    me.speed -= v[21];
                    v[14] = 0;
                }
            }
            if (me.speed <= 0)
                me.direction = 1;
        }
        if (me.right == 1) {
            if (me.speed < v[20]) {
                if (me.speed < 0) {
                    if (me.collisionMode == 0) {
                        if (me.speed < -262144)
                            v[14] = 16;
                    }
                    t[0] = v[9];
                    t[0] = -t[0];
                    if (me.speed > t[0]) {
                        me.speed = v[9];
                        v[14]    = 0;
                    }
                    else {
                        me.speed += v[9];
                    }
                }
                else {
                    me.speed += v[21];
                    v[14] = 0;
                }
            }
            if (me.speed >= 0)
                me.direction = 0;
        }
        t[0] = me.left;
        t[0] |= me.right;
        if (t[0] == 0) {
            if (me.speed > 0) {
                me.speed -= v[22];
                if (me.speed < 0)
                    me.speed = 0;
            }
            else {
                me.speed += v[22];
                if (me.speed > 0)
                    me.speed = 0;
            }
            if (me.speed > 8192)
                slope();
            if (me.speed < -8192)
                slope();
            if (me.angle > 192) {
                if (me.angle < 228) {
                    if (me.speed > -65536) {
                        if (me.speed < 65536)
                            me.controlLock = 30;
                    }
                }
            }
            if (me.angle > 28) {
                if (me.angle < 64) {
                    if (me.speed > -65536) {
                        if (me.speed < 65536)
                            me.controlLock = 30;
                    }
                }
            }
        }
        else {
            slope();
            if (me.right == 1) {
                if (me.left == 0) {
                    if (me.angle > 192) {
                        if (me.angle < 228) {
                            if (me.speed < 163840) {
                                if (me.speed > -131072)
                                    me.controlLock = 30;
                            }
                        }
                    }
                }
            }
            else {
                if (me.left == 1) {
                    if (me.angle > 28) {
                        if (me.angle < 64) {
                            if (me.speed > -163840) {
                                if (me.speed < 131072)
                                    me.controlLock = 30;
                            }
                        }
                    }
                }
            }
        }
        if (G[1] == 1) {
            if (me.left == 1) {
                t[0] = v[20];
                t[0] = -t[0];
                if (me.speed < t[0])
                    me.speed = t[0];
            }
            if (me.right == 1) {
                if (me.speed > v[20])
                    me.speed = v[20];
            }
        }
    }
    switch (me.collisionMode) {
        case 1:
            if (me.angle <= 192) {
                if (me.speed > -131072) {
                    if (me.speed < 131072)
                        fall();
                }
            }
            break;
        case 2:
            if (me.speed > -131072) {
                if (me.speed < 131072)
                    fall();
            }
            break;
        case 3:
            if (me.angle >= 64) {
                if (me.speed > -131072) {
                    if (me.speed < 131072)
                        fall();
                }
            }
            break;
    }
}

// The player's ground and air states (GlobalCode functions 10 and 12: CallFunction OBJECTSTATE, every frame for each
// player on the ground / in the air), natively: one statement per script instruction, in order. They call the physics
// functions' native bodies directly (patched as one group with them); the air state's variable call (OBJECTVALUE32,
// the character's air action) runs in the VM.
static void PS1PlayerState10() // on the ground
{
    int *t     = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int *G     = globalVariables;
    Entity &me = objectEntityList[objectEntityPos];
    int *v     = me.values;
    auto idle  = [&](int dir) { // the balance animation at a ledge (property value 2: restarted from frame 4 when turning)
        v[1]         = 0;
        me.animation = G[75];
        if (me.direction == 1 - dir) {
            me.prevAnimation  = G[75];
            me.frame          = 4;
            me.animationTimer = 0;
            me.animationSpeed = 0;
        }
        me.direction = dir;
    };
    auto roll = [&]() {
        me.state     = 14;
        me.animation = G[67];
        if (me.prevAnimation != G[67])
            me.ypos = ((me.ypos >> 16) - v[30]) << 16; // Sub OBJECTIYPOS OBJECTVALUE30 (write-back)
        v[2] = 1024;
        PlaySfx(18, 0);
    };
    if (me.animation != G[64])
        t[7] = 1;
    else
        t[7] = 0;
    PS1PlayerFn2();
    if (me.gravity == 1) {
        me.state = 12;
        PS1PlayerFn4();
        return;
    }
    PS1PlayerFn5();
    if (me.speed == 0) {
        if (me.collisionMode == 0) {
            switch (me.propertyValue) {
                case 0:
                    if (scriptCode[4] == 1) { // LOCAL[4]
                        me.animation = G[57];
                        v[1]         = 0;
                        if (me.floorSensors[1] == 0) {
                            if (me.floorSensors[2] == 0) {
                                me.animation = G[75];
                                me.direction = 0;
                            }
                            if (me.floorSensors[0] == 0) {
                                me.animation = G[75];
                                me.direction = 1;
                            }
                        }
                    }
                    else {
                        if (v[1] < 240) {
                            me.animation = G[57];
                            v[1]++;
                        }
                        else {
                            me.animation = G[58];
                            v[1]++;
                            if (v[1] == 1200) {
                                v[1]     = 0;
                                me.state = 11;
                            }
                        }
                        if (me.direction == 0) {
                            if (me.floorSensors[2] == 0) {
                                if (me.floorSensors[1] == 0) {
                                    v[1] = 0;
                                    if (me.floorSensors[3] == 0)
                                        me.animation = G[77];
                                    else
                                        me.animation = G[75];
                                }
                            }
                            else {
                                if (me.floorSensors[0] == 0) {
                                    if (me.floorSensors[1] == 0) {
                                        v[1]         = 0;
                                        me.animation = G[76];
                                    }
                                }
                            }
                        }
                        else {
                            if (me.floorSensors[0] == 0) {
                                if (me.floorSensors[1] == 0) {
                                    v[1] = 0;
                                    if (me.floorSensors[4] == 0)
                                        me.animation = G[77];
                                    else
                                        me.animation = G[75];
                                }
                            }
                            else {
                                if (me.floorSensors[2] == 0) {
                                    if (me.floorSensors[1] == 0) {
                                        v[1]         = 0;
                                        me.animation = G[76];
                                    }
                                }
                            }
                        }
                    }
                    break;
                case 1:
                    if (v[1] < 240) {
                        me.animation = G[57];
                        v[1]++;
                    }
                    else {
                        me.animation = G[58];
                    }
                    if (me.floorSensors[1] == 0) {
                        if (me.floorSensors[2] == 0) {
                            v[1]         = 0;
                            me.animation = G[75];
                            me.direction = 0;
                        }
                        if (me.floorSensors[0] == 0) {
                            v[1]         = 0;
                            me.animation = G[75];
                            me.direction = 1;
                        }
                    }
                    break;
                case 2:
                    if (v[1] < 240) {
                        me.animation = G[57];
                        v[1]++;
                    }
                    else {
                        me.animation = G[58];
                        v[1]++;
                        if (v[1] == 834) {
                            v[1]         = 0;
                            me.animation = G[57];
                        }
                    }
                    if (me.floorSensors[1] == 0) {
                        if (me.floorSensors[2] == 0)
                            idle(0);
                        if (me.floorSensors[0] == 0)
                            idle(1);
                    }
                    break;
            }
        }
    }
    else {
        v[1] = 0;
        if (me.speed > 0) {
            if (me.speed < 390594) {
                me.animation = G[62];
                PS1PlayerFn52();
            }
            else {
                if (me.speed > 655359)
                    me.animation = G[65];
                else
                    me.animation = G[63];
                PS1PlayerFn53();
            }
        }
        else {
            if (me.speed > -390594) {
                me.animation = G[62];
                PS1PlayerFn52();
            }
            else {
                if (me.speed < -655359)
                    me.animation = G[65];
                else
                    me.animation = G[63];
                PS1PlayerFn53();
            }
        }
    }
    if (v[14] > 0) {
        if (t[7] == 1)
            PlaySfx(3, 0);
        me.animation      = G[64];
        me.animationSpeed = 0;
        v[14]--;
        if (G[18] == 0) {
            PS1CreateTempObject(20, 0, me.xpos, me.ypos);
            PS1_OBJ(ap[8]).ypos      = ((PS1_OBJ(ap[8]).ypos >> 16) + PS1CollisionBottom(me)) << 16; // Add OBJECTIYPOS[+8]
            PS1_OBJ(ap[8]).drawOrder = v[18];
        }
        if (me.speed > 0)
            me.direction = 0;
        else
            me.direction = 1;
    }
    if (me.collisionMode == 0) {
        if (me.pushing == 2) {
            me.animation      = G[74];
            me.animationSpeed = 0;
        }
    }
    if (me.jumpPress == 1) {
        PS1PlayerFn6();
        return;
    }
    if (me.up == 1) {
        if (me.speed == 0) {
            if (me.animation != G[75]) {
                if (me.animation != G[76]) {
                    me.state = 16;
                    v[1]     = 0;
                }
                else {
                    me.up   = 0;
                    me.down = 0;
                }
            }
            else {
                me.up   = 0;
                me.down = 0;
            }
        }
    }
    if (me.down == 1) {
        if (me.speed == 0) {
            if (me.animation != G[75]) {
                if (me.animation != G[76]) {
                    me.state = 17;
                    v[1]     = 0;
                }
                else {
                    me.up   = 0;
                    me.down = 0;
                }
            }
            else {
                me.up   = 0;
                me.down = 0;
            }
        }
        else {
            if (me.left == 0) {
                if (me.right == 0) {
                    if (me.speed > 0) {
                        if (me.speed > 34816)
                            roll();
                    }
                    else {
                        if (me.speed < -34816)
                            roll();
                    }
                }
            }
        }
    }
}
static void PS1PlayerState12() // in the air
{
    int *G     = globalVariables;
    Entity *me = &objectEntityList[objectEntityPos];
    int *v     = me->values;
    auto toWalk = [&]() { // animation G[62] from the start (speed 40)
        me->animation     = G[62];
        me->prevAnimation = G[62];
        me->frame         = 0;
    };
    PS1PlayerFn3();
    if (me->gravity == 1) {
        PS1PlayerFn4();
        if (me->yvel > 131072) {
            if (me->animation == G[75])
                me->animation = G[62];
            if (me->animation == G[76])
                me->animation = G[62];
        }
        if (me->animation == G[68]) {
            if (me->yvel >= 0) {
                if (v[10] == G[57])
                    v[10] = G[62];
                me->animation = v[10];
            }
        }
        if (me->animation == G[64]) {
            if (v[14] > 0) {
                v[14]--;
            }
            else {
                toWalk();
                me->animationSpeed = 40;
            }
        }
        if (me->animation == G[90]) {
            if (me->animationSpeed == 40) {
                if (me->frame >= 12)
                    toWalk();
            }
            else {
                if (me->frame >= 24) {
                    toWalk();
                    me->animationSpeed = 40;
                }
            }
        }
        if (me->animation == G[69]) {
            if (me->yvel >= 0) {
                if (v[10] == G[57])
                    v[10] = G[62];
                me->animation = v[10];
            }
        }
        if (me->animation == G[67]) {
            if (v[35] == 1) {
                if (me->yvel >= v[28])
                    PS1CallScriptFunction(v[32], 0); // CallFunction OBJECTVALUE32 (the character's air action)
            }
        }
    }
    else {
        me->state = 10;
        PS1PlayerFn5();
        v[14] = 0;
    }
}

// Sonic 2's numbers in the player / Tails scripts both games share -> the running game's (docs/37 phase 2b): Sonic 1's
// GlobalCode has one global object less before the player states (states 26-29 one lower), one function less before
// 11-30 and one more before 31-69, and its animation globals are 3 lower. Only the numbers these natives use; each is
// checked by the Sonic 1 signatures (tools/scripts/patch_bytecode.py TAILSFN*_SIG_S1). Sonic 2: the number itself.
#if PS1_GAME == 1
static constexpr int PS1N(int n)
{
    switch (n) {
        case 16: return 15;
        case 17: return 16;
        case 19: return 18;
        case 27: return 26;
        case 28: return 27;
        case 29: return 28;
        case 34: return 35;
        case 63: return 64;
        case 65: return 66;
        case 68: return 69;
        case 74: return 71; // G[]: an animation global
        default: return n;
    }
}
#define PS1_TAILS_LOCAL 19146 // function 61's script locals and tables (Sonic 2: 20210)
#else
#define PS1N(n) (n)
#define PS1_TAILS_LOCAL 20210
#endif

// Tails's CPU (GlobalCode functions 61, 62, 66, 67: every frame for the second player in Sonic & Tails), natively: one
// statement per script instruction, in order, on the running entity (Tails; player 1 is slot 0). 61 delays player
// 1's inputs through six 16-bit shift registers (script locals) and records his positions in two 16-entry script
// tables; 62 follows him (calls 61 and the jump, PS1PlayerFn6, directly); 66 respawns Tails after 4 s off screen;
// 67 runs the AI mode in value 44 (a variable call, in the VM) and the blink / look timers. Every local and table
// write goes through PS1ScriptWrite, as the VM's. Patched as one group (patch_tails_ai).
static void PS1TailsFn61()
{
    int *t     = scriptEng.temp;
    int &cr    = scriptEng.checkResult;
    Entity &me = objectEntityList[objectEntityPos];
    Entity &p  = PS1_OBJ(0);
    auto L     = [](int pos) { return (int)scriptCode[pos]; };
    auto get   = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    auto set = [](int table, int index, int value) { // SetTableValue
        if (index >= 0 && index < scriptCode[table])
            PS1ScriptWrite(table + index + 1, value);
    };
    auto shift = [&](int pos, int bit) { // ShL LOCAL 1 / Or LOCAL <bit> / And LOCAL 65535
        PS1ScriptWrite(pos, L(pos) << 1);
        PS1ScriptWrite(pos, L(pos) | bit);
        PS1ScriptWrite(pos, L(pos) & 65535);
    };
    auto late = [&]() { // the position recorded one frame back
        t[0] = L(PS1_TAILS_LOCAL + 7);
        t[0]--;
        if (t[0] < 0)
            t[0] += 16;
        get(PS1_TAILS_LOCAL + 8, t[0], me.values[46]);
        get(PS1_TAILS_LOCAL + 25, t[0], me.values[47]);
    };
    if (me.controlMode > -1) {
        shift(PS1_TAILS_LOCAL + 0, p.up);
        shift(PS1_TAILS_LOCAL + 1, p.down);
        shift(PS1_TAILS_LOCAL + 2, p.left);
        shift(PS1_TAILS_LOCAL + 3, p.right);
        shift(PS1_TAILS_LOCAL + 4, p.jumpPress);
        shift(PS1_TAILS_LOCAL + 5, p.jumpHold);
        if (p.state == PS1N(34)) {
            for (int pos = PS1_TAILS_LOCAL + 1; pos <= PS1_TAILS_LOCAL + 5; ++pos)
                PS1ScriptWrite(pos, L(pos) << 15);
        }
        t[0] = L(PS1_TAILS_LOCAL + 0);
        t[0] >>= 15;
        me.up = t[0];
        t[0]  = L(PS1_TAILS_LOCAL + 1);
        t[0] >>= 15;
        me.down = t[0];
        t[0]    = L(PS1_TAILS_LOCAL + 2);
        t[0] >>= 15;
        me.left = t[0];
        t[0]    = L(PS1_TAILS_LOCAL + 3);
        t[0] >>= 15;
        me.right = t[0];
        t[0]     = L(PS1_TAILS_LOCAL + 4);
        t[0] >>= 15;
        me.jumpPress = t[0];
        t[0]         = L(PS1_TAILS_LOCAL + 5);
        t[0] >>= 15;
        me.jumpHold = t[0];
    }
    else {
        for (int pos = PS1_TAILS_LOCAL + 0; pos <= PS1_TAILS_LOCAL + 5; ++pos)
            PS1ScriptWrite(pos, 0);
    }
    if (p.state != PS1N(28)) {
        if (p.type != 7) {
            set(PS1_TAILS_LOCAL + 8, L(PS1_TAILS_LOCAL + 7), p.xpos);
            set(PS1_TAILS_LOCAL + 25, L(PS1_TAILS_LOCAL + 7), p.ypos);
            PS1ScriptWrite(PS1_TAILS_LOCAL + 7, L(PS1_TAILS_LOCAL + 7) + 1);
            PS1ScriptWrite(PS1_TAILS_LOCAL + 7, L(PS1_TAILS_LOCAL + 7) & 15);
            PS1ScriptWrite(PS1_TAILS_LOCAL + 6, L(PS1_TAILS_LOCAL + 6) + 1);
            PS1ScriptWrite(PS1_TAILS_LOCAL + 6, L(PS1_TAILS_LOCAL + 6) & 15);
            cr   = p.gravity == 1;
            t[0] = cr;
            cr   = p.values[42] == 0;
            t[0] &= cr;
            if (t[0] == 0) {
                get(PS1_TAILS_LOCAL + 8, L(PS1_TAILS_LOCAL + 6), me.values[46]);
                get(PS1_TAILS_LOCAL + 25, L(PS1_TAILS_LOCAL + 6), me.values[47]);
            }
            else {
                me.values[46] = p.xpos;
                me.values[47] = p.ypos;
            }
        }
        else {
            late();
        }
    }
    else {
        late();
    }
}
static void PS1TailsFn62()
{
    int *t = scriptEng.temp;
    int *G = globalVariables;
    PS1TailsFn61(); // CallFunction 61 (Sonic 1: 62)
    Entity &me = objectEntityList[objectEntityPos];
    Entity &p  = PS1_OBJ(0);
    int *v     = me.values;
    if (p.type != 1)
        return;
    t[0] = me.angle;
    t[0] += 16;
    t[0] &= 224;
    if (t[0] == 0) {
        if (me.left == 1) {
            t[0] = p.xpos;
            t[0] -= 524288;
            if (me.xpos < t[0]) {
                if (me.xvel <= 0)
                    me.left = 0;
            }
        }
        if (me.right == 1) {
            t[0] = p.xpos;
            t[0] += 524288;
            if (me.xpos > t[0]) {
                if (me.yvel <= 0)
                    me.right = 0;
            }
        }
    }
    if (p.state == PS1N(34))
        return;
    t[0] = v[46];
    t[1] = p.gravity;
    t[1] |= p.values[42];
    if (t[1] == 0) {
        if (p.speed < 131072) {
            if (p.speed > -131072) {
                if (p.direction == 0)
                    t[0] -= 2097152;
                else
                    t[0] += 2097152;
            }
        }
    }
    t[0] -= me.xpos;
    if (t[0] != 0) {
        if (t[0] < 0) {
            if (t[0] <= -3145728) {
                me.right = 0;
                me.left  = 1;
            }
            if (me.speed != 0) {
                if (me.direction == 1) {
                    t[1] = Cos256(me.angle);
                    t[1] *= 192;
                    me.xpos -= t[1];
                }
            }
        }
        else {
            if (t[0] >= 3145728) {
                me.left  = 0;
                me.right = 1;
            }
            if (me.speed != 0) {
                if (me.direction == 0) {
                    t[1] = Cos256(me.angle);
                    t[1] *= 192;
                    me.xpos += t[1];
                }
            }
        }
    }
    if (me.animation == G[PS1N(74)]) {
        v[45]++;
        if (p.direction == me.direction) {
            if (p.animation == G[PS1N(74)])
                v[45] = 0;
        }
        if (v[45] >= 30) {
            if (me.gravity == 0)
                PS1PlayerFn6(); // CallFunction 6
            v[1]  = 0;
            v[45] = 0;
        }
    }
    else {
        t[0] = me.ypos;
        t[0] -= v[47];
        if (t[0] > 2097152) {
            v[45]++;
            if (v[45] >= 64) {
                if (me.gravity == 0)
                    PS1PlayerFn6();
                v[1]  = 0;
                v[45] = 0;
            }
        }
        else {
            v[45] = 0;
        }
    }
    if (me.controlLock > 0) {
        if (me.speed < 32768) {
            if (me.speed > -32768)
                v[44] = PS1N(63);
        }
    }
}
static void PS1TailsFn66()
{
    int *t     = scriptEng.temp;
    int &cr    = scriptEng.checkResult;
    Entity &me = objectEntityList[objectEntityPos];
    Entity &p  = PS1_OBJ(0);
    int *v     = me.values;
    auto park  = [&]() { // state 65 (Sonic 1: 66; flying back in), motion and interactions off
        me.state              = PS1N(65);
        me.xvel               = 0;
        me.yvel               = 0;
        me.speed              = 0;
        me.tileCollisions     = 0;
        me.objectInteractions = 0;
    };
    if (p.type != 1)
        return;
    if (PS1ObjectOutOfBounds(&me) == 1)
        v[43]++;
    else
        v[43] = 0;
    if (v[43] > 239) {
        v[43]    = 0;
        me.state = PS1N(65);
        me.xpos  = p.xpos;
        me.ypos  = yScrollOffset;
        me.ypos -= 128;
        me.ypos <<= 16;
        me.xvel               = 0;
        me.yvel               = 0;
        me.speed              = 0;
        me.tileCollisions     = 0;
        me.objectInteractions = 0;
        me.controlMode        = 1;
        v[18]                 = 4;
        v[3]                  = 0;
        v[4]                  = 0;
    }
    cr   = p.state == PS1N(28);
    t[0] = cr;
    cr   = p.type == 7;
    t[0] |= cr;
    cr = me.state != PS1N(27);
    t[0] &= cr;
    cr = me.state != PS1N(28);
    t[0] &= cr;
    if (t[0] == 1) {
        v[43] = 0;
        park();
    }
}
static void PS1TailsFn67()
{
    int *t  = scriptEng.temp;
    auto E  = []() -> Entity & { return objectEntityList[objectEntityPos]; };
    if (E().state == PS1N(28))
        E().values[44] = PS1N(68);
    if (E().state == PS1N(29))
        E().values[44] = PS1N(68);
    PS1CallScriptFunction(E().values[44], 0); // CallFunction OBJECTVALUE44 (the AI mode)
    Entity &me = E();
    int *v     = me.values;
    if (me.state != PS1N(27)) {
        if (v[8] > 0) {
            v[8]--;
            t[0] = (v[8] & (1 << 2)) >> 2;
            if (t[0] == 1)
                me.visible = 0;
            else
                me.visible = 1;
        }
    }
    if (v[7] > 0) {
        if (me.state != PS1N(27)) {
            if (v[7] > 2000) {
                v[7] = 120;
                v[8] = 3;
            }
        }
        v[7]--;
        if (v[7] == 0) {
            v[8]       = 0;
            me.visible = 1;
        }
    }
    if (me.state != PS1N(16)) {
        if (me.state != PS1N(17)) {
            if (me.lookPosY > 0)
                me.lookPosY -= 2;
            if (me.lookPosY < 0)
                me.lookPosY += 2;
        }
    }
    if (me.state != PS1N(19)) {
        if (v[26] != 0) {
            StopSfx(19);
            StopSfx(20);
            v[26] = 0;
        }
    }
}

// Metropolis's MPZ Setup (stage object in Zone09; update 195 VM instructions, ~86 a frame, 12.7 hblanks),
// natively: one statement per script instruction, in order. Palette cycling, the animated tiles (Copy16x16Tile),
// the players' conveyor tiles (function 46 in the VM, from inside the player loop), the vertical wrap (entities
// shifted by 2048 px, indexes past ENTITY_COUNT through PS1_OBJ as in the VM) and the achievement call.
// Patched in only when the sub and its jump-table entries match MPZSETUP_SIG and function 46 is callable.
static void PS1MPZSetupUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    auto L     = [](int pos) { return (int)scriptCode[pos]; };
    auto get   = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    auto copies = [](const int *tiles, int n, int &src) { // Copy16x16Tile <tile> src / Inc src, n times
        for (int k = 0; k < n; ++k) {
            Copy16x16Tile(tiles[k], src);
            src++;
        }
    };
    v[0]++;
    if (v[0] > 2) {
        v[0] = 0;
        RotatePalette(0, 161, 163, 0);
    }
    v[10]++;
    if (v[10] >= 10) {
        v[10] = 0;
        v[11]++;
        if (v[11] >= 10)
            v[11] = 0;
        get(63679, v[11], t[0]);
        SetPaletteEntryPacked(0, 175, t[0]);
    }
    v[12]++;
    if (v[12] >= 18) {
        v[12] = 0;
        v[13]++;
        if (v[13] >= 6)
            v[13] = 0;
        get(63690, v[13], t[0]);
        SetPaletteEntryPacked(0, 168, t[0]);
    }
    static const int belt2[8] = { 760, 761, 762, 763, 764, 765, 766, 767 };
    static const int belt3[8] = { 744, 745, 746, 747, 748, 749, 750, 751 };
    static const int gear4[3] = { 753, 754, 755 };
    static const int gear5[3] = { 742, 743, 752 };
    static const int gear6[4] = { 756, 757, 758, 759 };
    copies(belt2, 8, v[2]);
    if (v[2] == 832)
        v[2] = 768;
    copies(belt3, 8, v[3]);
    if (v[3] == 896)
        v[3] = 832;
    if (v[7] < 2) {
        copies(gear4, 3, v[4]);
        if (v[4] == 908)
            v[4] = 896;
        copies(gear5, 3, v[5]);
        if (v[5] == 908)
            v[5] = 896;
        if (v[8] == 0)
            v[7] = 20;
        else
            v[7] = 8;
        v[8] ^= 1;
    }
    else {
        v[7]--;
    }
    if (v[9] < 2) {
        copies(gear6, 4, v[6]);
        if (v[6] == 932)
            v[6] = 908;
        v[9] = 14;
    }
    else {
        v[9]--;
    }
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6] = players.entityRefs[loop];
        t[1]  = PS1_OBJ(ap[6]).xpos;
        t[1] >>= 16;
        t[2] = PS1_OBJ(ap[6]).ypos;
        t[2] >>= 16;
        t[2] += 4;
        { // Get16x16TileInfo TEMP0 TEMP1 TEMP2 8 (TILEINFO_ANGLEB)
            int chunk = stageLayouts[0].tiles[(t[1] >> 7) + ((t[2] >> 7) << 8)] << 6;
            chunk += ((t[1] & 0x7F) >> 4) + 8 * ((t[2] & 0x7F) >> 4);
            int index = tiles128x128.tileIndex[chunk];
            t[0]      = collisionMasks[1].angles[index];
        }
        t[0] &= 1;
        if (t[0] == 1)
            PS1CallScriptFunction(46, 1);
    }
    PS1ScriptWrite(63698, L(63698) + L(63699));
    PS1ScriptWrite(63698, L(63698) & 3);
    if (PS1_OBJ(0).ypos >= 149946368) {
        t[1]  = PS1_OBJ(0).ypos;
        ap[6] = 0;
        while (ap[6] < ap[7]) {
            t[0] = PS1_OBJ(ap[6]).ypos;
            t[0] -= t[1];
            t[0] = abs(t[0]);
            if (t[0] < 9175040)
                PS1_OBJ(ap[6]).ypos -= 134217728;
            ap[6]++;
        }
        if (cameraYPos >= 2048) {
            cameraYPos -= 2048;
            yScrollOffset -= 2048;
        }
    }
    if (PS1_OBJ(0).ypos <= 9437184) {
        t[1]  = PS1_OBJ(0).ypos;
        ap[6] = 0;
        while (ap[6] < ap[7]) {
            t[0] = PS1_OBJ(ap[6]).ypos;
            t[0] -= t[1];
            t[0] = abs(t[0]);
            if (t[0] < 9175040)
                PS1_OBJ(ap[6]).ypos += 134217728;
            ap[6]++;
        }
        if (cameraYPos <= 384) {
            cameraYPos += 2048;
            yScrollOffset += 2048;
        }
    }
    if (cameraYPos >= 1552) {
        if (L(63697) == 0) {
            PS1ScriptWrite(63697, 1);
            ap[0] = 32;
            while (ap[0] < 1184) {
                if (PS1_OBJ(ap[0]).ypos <= 41943040) {
                    PS1_OBJ(ap[0]).ypos += 134217728;
                    PS1_OBJ(ap[0]).values[31] += 134217728;
                }
                ap[0]++;
            }
        }
    }
    if (cameraYPos <= 640) {
        if (L(63697) == 1) {
            PS1ScriptWrite(63697, 0);
            ap[0] = 32;
            while (ap[0] < 1184) {
                if (PS1_OBJ(ap[0]).ypos >= 109051904) {
                    PS1_OBJ(ap[0]).ypos -= 134217728;
                    PS1_OBJ(ap[0]).values[31] -= 134217728;
                }
                ap[0]++;
            }
        }
    }
    if (PS1_OBJ(0).state == 27)
        PS1ScriptWrite(63701, 1);
    if (L(63700) == 0) {
        if (debugMode == 0) {
            if (PS1_OBJ(30).type == 8) {
                PS1ScriptWrite(63700, 1);
                if (L(63701) == 0) { // CallNativeFunction2 GLOBAL[104] 9 100
                    int op0 = G[104], op1 = 9, op2 = 100;
                    if (op0 >= 0 && op0 < NATIIVEFUNCTION_COUNT) {
                        void (*func)(int *, int *) = (void (*)(int *, int *))nativeFunction[op0];
                        if (func)
                            func(&op1, &op2);
                    }
                }
            }
        }
    }
}

// Sonic 2's Monitor (GlobalCode object "Monitor", in most zones; update 112 VM instructions, ~21 a frame each),
// natively: one statement per script instruction, in order. Falling after a hit from below, the solid box, and the
// break (the power-up chosen by the script's random generator, GLOBAL[132]). Patched in only when the sub and its
// jump-table entries match MONITOR_SIG.
static void PS1MonitorUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    auto prop  = [&](int table, int index) { // GetTableValue OBJECTPROPERTYVALUE <index> <table>
        if (index >= 0 && index < scriptCode[table])
            me.propertyValue = scriptCode[table + index + 1];
    };
    if (me.state == 1) {
        me.yvel += 14336;
        me.ypos += me.yvel;
        if (me.yvel >= 0) {
            ObjectFloorCollision(0, 16, 0); // ObjectTileCollision CSIDE_FLOOR 0 16 0
            if (cr == 1) {
                me.yvel  = 0;
                me.state = 0;
            }
        }
    }
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        cr        = p.yvel > -1;
        t[0]      = cr;
        cr        = p.gravity == 0;
        t[0] |= cr;
        if (t[0] == 1) {
            cr   = p.animation == G[67];
            t[0] = cr;
            cr   = p.animation == G[85];
            t[0] |= cr;
            cr = p.animation == G[87];
            t[0] |= cr;
            if (t[0] == 1) {
                if (p.values[16] == 0) {
                    TouchCollision(&PS1_OBJ(self), -16, -14, 16, 16, &PS1_OBJ(ap[6]), p.values[40], p.values[38], p.values[41],
                                   p.values[39]);
                    if (cr == 1) {
                        me.state = 0;
                        PS1CreateTempObject(19, 0, me.xpos, me.ypos);
                        PS1_OBJ(ap[8]).drawOrder = 4;
                        p.yvel += p.values[25];
                        p.yvel += p.values[25];
                        p.yvel       = -p.yvel;
                        me.type      = 14;
                        me.values[3] = ap[6];
                        if (me.priority != 4)
                            me.priority = 1;
                        me.alpha     = 255;
                        me.values[0] = me.ypos;
                        me.values[1] = -196608;
                        if (me.propertyValue == 14) {
                            G[132] *= 1103515245;
                            G[132] += 12345;
                            G[132] &= 2147483647;
                            t[0] = G[132];
                            t[0] >>= 16;
                            t[0] %= 10; // Mod (divisor never 0)
                            if (G[13] == 1) {
                                if (p.propertyValue == 0)
                                    prop(36222, t[0]);
                                else
                                    prop(36211, t[0]);
                                if (me.propertyValue == 13) {
                                    if (PS1_OBJ(0).controlMode == -1)
                                        me.propertyValue = 2;
                                    if (PS1_OBJ(1).controlMode == -1)
                                        me.propertyValue = 2;
                                    if (PS1_OBJ(0).state == 1)
                                        me.propertyValue = 1;
                                    if (PS1_OBJ(1).state == 1)
                                        me.propertyValue = 1;
                                }
                            }
                            else {
                                if (G[12] == 3)
                                    prop(36222, t[0]);
                                else
                                    prop(36211, t[0]);
                                if (me.propertyValue == 13)
                                    me.propertyValue = 1;
                            }
                        }
                        if (G[13] == 1) {
                            if (G[108] == 0) {
                                if (ap[6] == 0)
                                    G[120]++;
                                else
                                    G[124]++;
                            }
                            else {
                                if (ap[6] == 1)
                                    G[120]++;
                                else
                                    G[124]++;
                            }
                        }
                        PlaySfx(8, 0);
                    }
                }
                else {
                    BoxCollision(&PS1_OBJ(self), -15, -14, 15, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                }
            }
            else {
                BoxCollision(&PS1_OBJ(self), -15, -14, 15, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            }
        }
        else {
            BoxCollision(&PS1_OBJ(self), -15, -16, 15, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 4) {
                me.state = 1;
                me.yvel  = -131072;
                p.yvel   = 131072;
            }
        }
    }
}

#if PS1_GAME == 1
// Sonic 1's Monitor update sub (GlobalCode object "Monitor", every zone), natively: one statement per script
// instruction, in order (MONITOR_SIG_S1). Falling after a hit from below (state 1: gravity, then the floor), then per
// player (group 256, ARRAYPOS6): a rolling / jumping / spin-dashing player (animation globals 64 / 81 / 83) coming
// down or grounded breaks it (touch box against the player's attack box, values 38-41): the explosion (temp object
// 18), the bounce, the Broken Monitor (type 14), the sound; otherwise it is solid (from below: knocked up).
static void PS1MonitorUpdateS1()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr    = scriptEng.checkResult;
    int *G     = globalVariables;
    int self   = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    if (me.state == 1) {
        me.yvel += 14336;
        me.ypos += me.yvel;
        if (me.yvel >= 0) {
            ObjectFloorCollision(0, 16, 0); // ObjectTileCollision CSIDE_FLOOR 0 16 0
            if (cr == 1) {
                me.yvel  = 0;
                me.state = 0;
            }
        }
    }
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        cr        = p.yvel > -1;
        t[0]      = cr;
        cr        = p.gravity == 0;
        t[0] |= cr;
        if (t[0] == 1) {
            cr   = p.animation == G[64];
            t[0] = cr;
            cr   = p.animation == G[81];
            t[0] |= cr;
            cr = p.animation == G[83];
            t[0] |= cr;
            if (t[0] == 1) {
                if (p.values[16] == 0) {
                    TouchCollision(&PS1_OBJ(self), -16, -14, 16, 16, &PS1_OBJ(ap[6]), p.values[40], p.values[38], p.values[41],
                                   p.values[39]);
                    if (cr == 1) {
                        me.state = 0;
                        PS1CreateTempObject(18, 0, me.xpos, me.ypos);
                        PS1_OBJ(ap[8]).drawOrder = 4;
                        p.yvel += p.values[25];
                        p.yvel += p.values[25];
                        p.yvel  = -p.yvel;
                        me.type = 14;
                        if (me.priority != 4)
                            me.priority = 1;
                        me.alpha     = 255;
                        me.values[0] = me.ypos;
                        me.values[1] = -196608;
                        PlaySfx(8, 0);
                    }
                }
                else {
                    BoxCollision(&PS1_OBJ(self), -15, -14, 15, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                }
            }
            else {
                BoxCollision(&PS1_OBJ(self), -15, -14, 15, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            }
        }
        else {
            BoxCollision(&PS1_OBJ(self), -15, -16, 15, 16, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
            if (cr == 4) {
                me.state = 1;
                me.yvel  = -131072;
                p.yvel   = 131072;
            }
        }
    }
}
#endif

#if PS1_GAME == 1
// Sonic 1's Red Spring / Yellow Spring update subs (GlobalCode, every zone), natively: one statement per script
// instruction, in order (SPRING_SIGS_S1; the operand says which: 0 red, 1 yellow, whose differences are marked). Per
// player (group 256, ARRAYPOS6), by the spring's direction (property value 0 up, 1 right, 2 left, 3 down): the solid
// (or platform) box, then the launch when the spring's face touches the player.
static void PS1S1Spring(int yellow)
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr    = scriptEng.checkResult;
    int *G     = globalVariables;
    int self   = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        auto upLaunch = [&]() { // case 0's launch (both of its branches)
            p.values[10] = G[59];
            if (p.animation == G[60])
                p.values[10] = G[60];
            if (p.animation == G[62])
                p.values[10] = G[62];
            me.values[0]     = 1;
            p.state          = 11;
            p.tileCollisions = 1;
            p.gravity        = 1;
            p.speed          = p.xvel;
            if (yellow) {
                p.yvel = -655360;
                p.yvel += me.values[2];
            }
            else
                p.yvel = -1048576;
            p.animation = 11;
            p.values[1] = 0;
            PlaySfx(11, 0);
        };
        switch (me.propertyValue) {
            case 0: // up
                t[0] = me.values[1];
                if (p.gravity == 1)
                    t[0] = 1;
                if (p.collisionMode > 0) {
                    if (p.yvel < 0)
                        t[0] = 1;
                }
                if (t[0] == 0) {
                    BoxCollision(&PS1_OBJ(self), -14, -8, 14, 8, &PS1_OBJ(ap[6]), C_BOX, C_BOX, C_BOX, C_BOX);
                    PS1TouchS1(&PS1_OBJ(self), -14, -10, 14, -6, &PS1_OBJ(ap[6]));
                    if (cr == 1)
                        upLaunch();
                }
                else {
                    if (p.yvel >= 0) {
                        PlatformCollision(&PS1_OBJ(self), -14, -8, 14, 8, &PS1_OBJ(ap[6]), C_BOX, C_BOX, C_BOX, C_BOX);
                        PS1TouchS1(&PS1_OBJ(self), -14, -10, 14, -6, &PS1_OBJ(ap[6]));
                        if (cr == 1)
                            upLaunch();
                    }
                }
                break;
            case 1: // right
                BoxCollision(&PS1_OBJ(self), -8, -14, 8, 14, &PS1_OBJ(ap[6]), C_BOX, C_BOX, C_BOX, C_BOX);
                if (p.gravity == 0) {
                    PS1TouchS1(&PS1_OBJ(self), 6, -14, yellow ? 10 : 11, 14, &PS1_OBJ(ap[6]));
                    if (cr == 1) {
                        me.values[0]     = 1;
                        p.tileCollisions = 1;
                        if (!yellow)
                            p.angle = 0;
                        p.speed         = yellow ? 655360 : 1048576;
                        p.collisionMode = 0;
                        p.pushing       = 0;
                        p.direction     = 0;
                        p.controlLock   = yellow ? 15 : 12;
                        PlaySfx(11, 0);
                        if (p.state != 13) {
                            p.state     = 10;
                            p.animation = G[60];
                        }
                    }
                }
                else {
                    if (me.values[7] == 1) {
                        PS1TouchS1(&PS1_OBJ(self), 6, -4, 11, 4, &PS1_OBJ(ap[6]));
                        if (cr == 1) {
                            me.values[0]     = 1;
                            p.tileCollisions = 1;
                            if (!yellow)
                                p.angle = 0;
                            p.speed         = yellow ? 655360 : 1048576;
                            p.yvel          = 0;
                            p.collisionMode = 0;
                            p.pushing       = 0;
                            p.direction     = 0;
                            p.controlLock   = yellow ? 15 : 12;
                            PlaySfx(11, 0);
                            if (p.state != 14) {
                                p.animation = 11;
                                if (p.animation != G[64])
                                    p.animation = G[60];
                                me.animationSpeed = me.speed; // the spring's own (as the script)
                                p.animationSpeed *= 80;
                                p.animationSpeed /= 393216;
                            }
                        }
                    }
                }
                break;
            case 2: // left
                BoxCollision(&PS1_OBJ(self), -8, -14, 8, 14, &PS1_OBJ(ap[6]), C_BOX, C_BOX, C_BOX, C_BOX);
                if (p.gravity == 0) {
                    PS1TouchS1(&PS1_OBJ(self), -10, -14, -6, 14, &PS1_OBJ(ap[6]));
                    if (cr == 1) {
                        me.values[0]     = 1;
                        p.tileCollisions = 1;
                        p.speed          = yellow ? -655360 : -1048576;
                        p.collisionMode  = 0;
                        p.pushing        = 0;
                        p.direction      = 1;
                        p.controlLock    = 15;
                        PlaySfx(11, 0);
                        if (p.state != 13) {
                            p.state     = 10;
                            p.animation = G[60];
                        }
                    }
                }
                else {
                    if (me.values[7] == 1) {
                        PS1TouchS1(&PS1_OBJ(self), -10, -14, -6, 14, &PS1_OBJ(ap[6]));
                        if (cr == 1) {
                            me.values[0]     = 1;
                            p.tileCollisions = 1;
                            p.speed          = yellow ? -655360 : -1048576;
                            p.yvel           = 0;
                            p.collisionMode  = 0;
                            p.pushing        = 0;
                            p.direction      = 1;
                            p.controlLock    = 15;
                            PlaySfx(11, 0);
                            if (p.state != 14) {
                                p.animation = 11;
                                if (p.animation != G[64])
                                    p.animation = G[60];
                                me.animationSpeed = me.speed;
                                p.animationSpeed  = -p.animationSpeed;
                                p.animationSpeed *= 80;
                                p.animationSpeed /= 393216;
                            }
                        }
                    }
                }
                break;
            case 3: // down
                BoxCollision(&PS1_OBJ(self), -14, -8, 14, 8, &PS1_OBJ(ap[6]), C_BOX, C_BOX, C_BOX, C_BOX);
                if (yellow || p.yvel <= 0) { // the yellow one has no IfLowerOrEqual
                    PS1TouchS1(&PS1_OBJ(self), -14, 6, 14, 10, &PS1_OBJ(ap[6]));
                    if (cr == 1) {
                        if (yellow)
                            me.values[0] = 1;
                        if (p.collisionMode == 2) {
                            p.speed = -p.speed;
                            p.xvel  = -p.xvel;
                        }
                        if (!yellow)
                            me.values[0] = 1;
                        p.state          = 11;
                        p.tileCollisions = 1;
                        p.gravity        = 1;
                        p.speed          = p.xvel;
                        p.yvel           = yellow ? 655360 : 1048576;
                        p.values[1]      = 0;
                        PlaySfx(11, 0);
                    }
                }
                break;
            default: break;
        }
    }
}
#endif

#if PS1_GAME == 1
// FUNC_GET16X16TILEINFO's body for Sonic 1's natives: `type` of the 16x16 tile at pixel (x, y) of the FG layer; `old` =
// the output variable's value, kept for a type the VM doesn't handle.
static int PS1TileInfoS1(int x, int y, int type, int old)
{
    int cx = x >> 7, cy = y >> 7;
    int c  = stageLayouts[0].tiles[cx + (cy << 8)] << 6;
    c += ((x & 0x7F) >> 4) + 8 * ((y & 0x7F) >> 4);
    int index = tiles128x128.tileIndex[c];
    switch (type) {
        case TILEINFO_INDEX: return tiles128x128.tileIndex[c];
        case TILEINFO_DIRECTION: return tiles128x128.direction[c];
        case TILEINFO_VISUALPLANE: return tiles128x128.visualPlane[c];
        case TILEINFO_SOLIDITYA: return tiles128x128.collisionFlags[0][c];
        case TILEINFO_SOLIDITYB: return tiles128x128.collisionFlags[1][c];
        case TILEINFO_FLAGSA: return collisionMasks[0].flags[index];
        case TILEINFO_ANGLEA: return collisionMasks[0].angles[index];
        case TILEINFO_FLAGSB: return collisionMasks[1].flags[index];
        case TILEINFO_ANGLEB: return collisionMasks[1].angles[index];
        default: return old;
    }
}

// Labyrinth's LZ Setup subs (stage object; LZSETUP_SIGS_S1, the operand says which), one statement per instruction:
// 0 update: the underwater ripple and the two palette cycles (every 3 frames; the waterfall's from its table), then per
// player (group 256, ARRAYPOS6) the tiles under its feet (71 / 72: the slide; tile angle B 1: the water current, its
// direction), the current's sounds for player 1 and the countdown in LOCAL[52320]. 1 draw: the palette banks above /
// below the water line.
static void PS1S1LZSetup(int sub)
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr    = scriptEng.checkResult;
    int *G     = globalVariables;
    Entity &me = PS1_OBJ(objectEntityPos);
    if (sub == 1) {
        t[0] = waterLevel;
        t[0] -= yScrollOffset;
        if (t[0] < 0)
            t[0] = 0;
        if (t[0] > SCREEN_YSIZE)
            t[0] = SCREEN_YSIZE;
        SetActivePalette(0, 0, t[0]);
        if (scriptCode[52320] > 0)
            SetActivePalette(2, t[0], SCREEN_YSIZE);
        else
            SetActivePalette(1, t[0], SCREEN_YSIZE);
        return;
    }
    me.values[0]++;
    if (me.values[0] > 1) {
        stageLayouts[0].deformationOffsetW++;
        stageLayouts[1].deformationOffsetW++;
        me.values[0] = 0;
    }
    me.values[1]++;
    if (me.values[1] == 3) {
        me.values[1] = 0;
        RotatePalette(0, 171, 174, 0);
        RotatePalette(1, 171, 174, 0);
    }
    if (me.values[2] > 0) {
        me.values[2]--;
    }
    else {
        me.values[3]++;
        me.values[3] %= 3; // Mod (divisor 3)
        int idx = me.values[3]; // GetTableValue OBJECTVALUE2 OBJECTVALUE3 52321
        if (idx >= 0 && idx < scriptCode[52321])
            me.values[2] = scriptCode[52321 + idx + 1];
        RotatePalette(0, 187, 189, scriptCode[52319]);
        RotatePalette(1, 187, 189, scriptCode[52319]);
    }
    TypeGroupList &players = objectTypeGroupList[256];
    for (int loop = 0; loop < players.listSize; ++loop) {
        ap[6]     = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        t[1]      = p.xpos;
        t[1] >>= 16;
        t[2] = p.ypos;
        t[2] >>= 16;
        t[2] += PS1CollisionBottom(p);
        t[2]--;
        t[0] = PS1TileInfoS1(t[1], t[2], TILEINFO_INDEX, t[0]);
        cr   = t[0] == 71;
        t[3] = cr;
        cr   = t[0] == 72;
        t[3] |= cr;
        if (t[3] == 1) {
            cr   = p.state == 19;
            t[3] = cr;
            cr   = p.state == 20;
            t[3] |= cr;
            cr = p.state == 23;
            t[3] |= cr;
            cr = p.state == 24;
            t[3] |= cr;
            if (t[3] == 1) {
                if (p.state == 19) {
                    p.xvel  = -p.xvel;
                    p.speed = -p.speed;
                }
                p.state     = 21;
                p.animation = G[82];
            }
        }
        if (p.gravity == 0) {
            t[0] = PS1TileInfoS1(t[1], t[2], TILEINFO_ANGLEB, t[0]);
            if (t[0] == 1) {
                p.state = 34;
                t[0]    = PS1TileInfoS1(t[1], t[2], TILEINFO_DIRECTION, t[0]);
                switch (t[0]) { // cases 0 / 2 break after their statement, 1 / 3 fall to the end
                    case 0:
                    case 2: p.direction = 1; break;
                    case 1:
                    case 3: p.direction = 0; break;
                    default: break;
                }
            }
        }
    }
    if (PS1_OBJ(0).state == 34) {
        if (me.values[4] == 0) {
            if (me.values[5] == 0) {
                PlaySfx(50, 0);
                StopSfx(51);
                me.values[5] = 1;
            }
            else {
                StopSfx(50);
                PlaySfx(51, 0);
            }
        }
        me.values[4]++;
        me.values[4] &= 63;
    }
    else {
        if (me.values[4] != 0) {
            me.values[4]++;
            me.values[4] &= 63;
        }
        else {
            me.values[4] = 0;
            me.values[5] = 0;
        }
    }
    if (scriptCode[52320] > 0)
        PS1ScriptWrite(52320, scriptCode[52320] - 1);
}
#endif

// Sonic 2's Invisible Block (GlobalCode object "Invisible Block"; update 81 VM instructions), natively: one statement
// per script instruction, in order (the temps keep growing across the player loop, as in the script). Solid (state 0:
// crushing a player against it long enough runs function 50, the death, in the VM from inside the loop), right wall
// (1), left wall (2). Patched in only when the sub and its jump-table entries match INVBLOCK_SIG.
static void PS1InvisibleBlockUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int &cr  = scriptEng.checkResult;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    TypeGroupList &players = objectTypeGroupList[256];
    t[0] = v[0];
    t[0] = -t[0];
    t[1] = v[1];
    t[1] = -t[1];
    switch (me.state) {
        case 0:
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                if (PS1_OBJ(ap[6]).state == 25)
                    continue;
                BoxCollision(&PS1_OBJ(self), t[0], t[1], v[0], v[1], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                switch (cr) {
                    case 0:
                        t[0] += 2;
                        t[1] += 2;
                        t[2] = v[0];
                        t[3] = v[1];
                        t[2] -= 2;
                        t[3] -= 2;
                        TouchCollision(&PS1_OBJ(self), t[0], t[1], t[2], t[3], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                        if (cr == 1)
                            PS1_OBJ(ap[6]).gravity = 0;
                        if (ap[6] == 0)
                            v[2] = 0;
                        else
                            v[3] = 0;
                        break;
                    case 4:
                        if (PS1_OBJ(ap[6]).gravity == 0) {
                            if (ap[6] == 0) {
                                v[2]++;
                                if (v[2] > scriptCode[61439])
                                    PS1CallScriptFunction(50, 1);
                            }
                            else {
                                v[3]++;
                                if (v[3] > scriptCode[61439])
                                    PS1CallScriptFunction(50, 1);
                            }
                        }
                        break;
                }
            }
            break;
        case 1:
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                if (PS1_OBJ(ap[6]).state == 25)
                    continue;
                TouchCollision(&PS1_OBJ(self), t[0], t[1], v[0], v[1], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                if (cr == 1) {
                    Entity &p = PS1_OBJ(ap[6]);
                    if (p.gravity == 0) {
                        p.xpos = PS1CollisionRight(p);
                        p.xpos = -p.xpos;
                        p.xpos -= v[0];
                        p.xpos <<= 16;
                        p.xpos += me.xpos;
                        if (p.speed > 0)
                            p.speed = 0;
                    }
                }
            }
            break;
        case 2:
            for (int loop = 0; loop < players.listSize; ++loop) {
                ap[6] = players.entityRefs[loop];
                if (PS1_OBJ(ap[6]).state == 25)
                    continue;
                TouchCollision(&PS1_OBJ(self), t[0], t[1], v[0], v[1], &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                if (cr == 1) {
                    Entity &p = PS1_OBJ(ap[6]);
                    if (p.gravity == 0) {
                        p.xpos = PS1CollisionLeft(p);
                        p.xpos = -p.xpos;
                        p.xpos += v[0];
                        p.xpos <<= 16;
                        p.xpos += me.xpos;
                        if (p.speed < 0)
                            p.speed = 0;
                    }
                }
            }
            break;
    }
}
// Scene3D buffer entries as the VM indexes them (PS1_VTX / PS1_FACE: mapped, clamped, high-water marks), out of line:
// the special-stage natives touch them in many places (code size, docs/30 phase 7.2b).
__attribute__((noinline)) static Vertex &PS1Vertex(int i) { return vertexBuffer[PS1_VTX(i)]; }
__attribute__((noinline)) static Face &PS1Face(int i) { return faceBuffer[PS1_FACE(i)]; }
// Scene3D counts as the VM writes them (VAR_SCENE3DVERTEXCOUNT / FACECOUNT: high-water marks, then mapped/clamped).
static inline void PS1SetVertexCount(int n)
{
    if (n > g_ps1Scene3DMaxCountV)
        g_ps1Scene3DMaxCountV = n;
    vertexCount = PS1_VTX_COUNT(n);
}
static inline void PS1SetFaceCount(int n)
{
    if (n > g_ps1Scene3DMaxCountF)
        g_ps1Scene3DMaxCountF = n;
    faceCount = PS1_FACE_COUNT(n);
}
// The special stage's pipe-segment build (Special function 0: the Halfpipe's update calls it each time the pipe
// advances one segment, its startup 8 times), natively: one statement per script instruction, in order; every local
// write goes through PS1ScriptWrite as the VM's does (same order, so the big-value table ends the same). Reads the pipe
// layout (tile angles) for the turn, moves the Halfpipe, places the objects the pipe reaches (rings / bombs: their 3D
// positions), copies the next segment's vertices from its table and transforms them. Patched in only when the function
// and its jump-table entries match HPSEG_FN_SIG.
__attribute__((noinline)) static void PS1TransformWorld(int from, int to) // TransformVertices 0 <from> <to>
{
    TransformVertices(&matWorld, PS1_VTX(from), PS1_VTX_COUNT(to));
}
volatile uint32_t g_ps1HalfpipeCover = 0; // special-stage native branches taken (GDB): PS1HalfpipeSegment bits 0-3 turn,
// 4-11 segment kind, 12-13 objects; PS1PlayerFaces 14 visible, 15 value4 sprite, 16 value23 sprite; PS1SpecialPlayerRun
// 17 right, 18 left, 19 neither, 20 jump, 21 off the top
__attribute__((optimize("Os"))) static void PS1HalfpipeSegment()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    Entity &o0 = PS1_OBJ(0);
    auto L     = [](int pos) { return (int)scriptCode[pos]; };
    auto LW    = [](int pos, int value) { PS1ScriptWrite(pos, value); };
    auto angle = [](int x, int y) { // Get16x16TileInfo <dst> x y TILEINFO_ANGLEA
        int chunk = stageLayouts[0].tiles[(x >> 7) + ((y >> 7) << 8)] << 6;
        chunk += ((x & 0x7F) >> 4) + 8 * ((y & 0x7F) >> 4);
        return (int)collisionMasks[0].angles[tiles128x128.tileIndex[chunk]];
    };
    auto getL = [&](int pos, int table, int index) { // GetTableValue LOCAL<pos> <index> <table> (the VM writes it back)
        int value = L(pos);
        if (index >= 0 && index < scriptCode[table])
            value = scriptCode[table + index + 1];
        LW(pos, value);
    };
    auto get = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    auto set = [](int table, int index, int value) { // SetTableValue
        if (index >= 0 && index < scriptCode[table])
            PS1ScriptWrite(table + index + 1, value);
    };
    auto TV = [](int from, int to) { PS1TransformWorld(from, to); };
    auto V  = [](int i) -> Vertex & { return PS1Vertex(i); };
    t[1] = o0.xpos;
    t[1] >>= 16;
    t[1] += 376;
    t[2] = o0.ypos;
    t[2] >>= 16;
    t[0] = angle(t[1], t[2]);
    if (t[0] == 1) {
        t[2] += 128;
        t[2] &= 65408;
        o0.ypos = t[2] << 16;
    }
    t[1] += 16;
    t[3] = L(942);
    t[3] += 64;
    t[3] &= 511;
    t[3] >>= 7;
    g_ps1HalfpipeCover = g_ps1HalfpipeCover | (1u << (t[3] & 3));
    switch (t[3]) {
        case 0:
            t[0] = angle(t[1], t[2]);
            getL(935, 1206, t[0]);
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(936, 1206, t[0]);
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(937, 1206, t[0]);
            break;
        case 1:
            t[0] = angle(t[1], t[2]);
            getL(937, 1206, t[0]);
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(936, 1206, t[0]);
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(935, 1206, t[0]);
            break;
        case 2:
            t[0] = angle(t[1], t[2]);
            getL(935, 1206, t[0]);
            LW(935, -L(935));
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(936, 1206, t[0]);
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(937, 1206, t[0]);
            LW(937, -L(937));
            break;
        case 3:
            t[0] = angle(t[1], t[2]);
            getL(937, 1206, t[0]);
            LW(937, -L(937));
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(936, 1206, t[0]);
            t[1] += 16;
            t[0] = angle(t[1], t[2]);
            getL(935, 1206, t[0]);
            LW(935, -L(935));
            break;
    }
    t[0] = curYBoundary2;
    t[0] <<= 16;
    o0.ypos += 524288;
    if (o0.ypos >= t[0])
        o0.ypos -= t[0];
    o0.values[22] += 524288;
    if (o0.values[22] >= t[0])
        o0.values[22] -= t[0];
    MatrixRotateXYZ(&matWorld, L(941), L(942), L(943));
    MatrixTranslateXYZ(&matTemp, L(938), L(939), L(940));
    MatrixMultiply(&matWorld, &matTemp);
    t[0] = 1;
    while (t[0] == 1) { // the objects the pipe has reached (sorted by y), from ARRAYPOS5 on
        if (PS1_OBJ(ap[5]).ypos < o0.ypos) {
            PS1_OBJ(ap[5]).priority = 1;
            g_ps1HalfpipeCover      = g_ps1HalfpipeCover | (PS1_OBJ(ap[5]).values[7] == 1 ? 1u << 12 : 1u << 13);
            if (PS1_OBJ(ap[5]).values[7] == 1) {
                V(4094).x = PS1_OBJ(ap[5]).values[1];
                V(4094).y = PS1_OBJ(ap[5]).values[2];
                V(4094).z = 0;
                V(4095).x = PS1_OBJ(ap[5]).values[8];
                V(4095).y = PS1_OBJ(ap[5]).values[9];
                V(4095).z = 0;
                TV(4094, 4096);
                PS1_OBJ(ap[5]).values[3]  = o0.values[22];
                PS1_OBJ(ap[5]).values[4]  = V(4094).x;
                PS1_OBJ(ap[5]).values[5]  = V(4094).y;
                PS1_OBJ(ap[5]).values[6]  = V(4094).z;
                PS1_OBJ(ap[5]).values[11] = V(4095).x;
                PS1_OBJ(ap[5]).values[12] = V(4095).y;
                PS1_OBJ(ap[5]).values[13] = V(4095).z;
            }
            else {
                V(4095).x = PS1_OBJ(ap[5]).values[1];
                V(4095).y = PS1_OBJ(ap[5]).values[2];
                V(4095).z = 0;
                TV(4095, 4096);
                PS1_OBJ(ap[5]).values[3] = o0.values[22];
                PS1_OBJ(ap[5]).values[4] = V(4095).x;
                PS1_OBJ(ap[5]).values[5] = V(4095).y;
                PS1_OBJ(ap[5]).values[6] = V(4095).z;
            }
            ap[5]++;
        }
        else
            t[0] = 0;
    }
    t[2] = L(932);
    t[2] &= 7;
    getL(929, 1224, t[2]);
    LW(932, L(932) + 1);
    t[0]  = 0;
    ap[0] = L(930);
    get(L(929), t[0], t[1]);
    t[0]++;
    while (t[1] > 0) { // the segment's vertices from its table
        int table = L(929);
        Vertex &q = V(ap[0]);
        get(table, t[0], q.x);
        t[0]++;
        get(table, t[0], q.y);
        t[0]++;
        get(table, t[0], q.z);
        t[0]++;
        ap[0]++;
        t[1]--;
    }
    set(945, L(944), L(938));
    set(986, L(944), L(939));
    set(1027, L(944), L(940));
    set(1068, L(944), L(941));
    set(1109, L(944), L(942));
    set(1150, L(944), L(943));
    auto turn = [&](int nearZ, int mul) { // switch cases 5 (3072, 12) and 6 (5120, 20)
        get(1197, t[2], t[0]);
        t[0] += L(930);
        TV(L(930), t[0]);
        V(4094).x = 0;
        V(4094).y = 0;
        V(4094).z = nearZ;
        V(4095).x = 0;
        V(4095).y = 0;
        V(4095).z = 8192;
        TV(4094, 4096);
        LW(938, V(4094).x);
        LW(939, V(4094).y);
        LW(940, V(4094).z);
        t[3] = L(935);
        t[3] = (int)((uint)t[3] * (uint)mul);
        t[3] >>= 5;
        LW(941, L(941) + t[3]);
        t[3] = L(936);
        t[3] = (int)((uint)t[3] * (uint)mul);
        t[3] >>= 5;
        LW(942, L(942) + t[3]);
        t[3] = L(937);
        t[3] = (int)((uint)t[3] * (uint)mul);
        t[3] >>= 5;
        LW(943, L(943) + t[3]);
        MatrixRotateXYZ(&matWorld, L(941), L(942), L(943));
        MatrixTranslateXYZ(&matTemp, L(938), L(939), L(940));
        MatrixMultiply(&matWorld, &matTemp);
        t[1] = t[0];
        t[1] += 17;
        TV(t[0], t[1]);
        LW(938, V(4095).x);
        LW(939, V(4095).y);
        LW(940, V(4095).z);
        getL(941, 1068, L(944));
        getL(942, 1109, L(944));
        getL(943, 1150, L(944));
        LW(941, L(941) + L(935));
        LW(942, L(942) + L(936));
        LW(943, L(943) + L(937));
        MatrixRotateXYZ(&matWorld, L(941), L(942), L(943));
        MatrixTranslateXYZ(&matTemp, L(938), L(939), L(940));
        MatrixMultiply(&matWorld, &matTemp);
        TV(t[1], ap[0]);
    };
    g_ps1HalfpipeCover = g_ps1HalfpipeCover | (1u << (4 + (t[2] & 7)));
    switch (t[2]) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 7:
            get(1197, t[2], t[0]);
            t[0] += L(930);
            TV(L(930), t[0]);
            V(4095).x = 0;
            V(4095).y = 0;
            V(4095).z = 8192;
            TV(4095, 4096);
            LW(938, V(4095).x);
            LW(939, V(4095).y);
            LW(940, V(4095).z);
            LW(941, L(941) + L(935));
            LW(942, L(942) + L(936));
            LW(943, L(943) + L(937));
            MatrixRotateXYZ(&matWorld, L(941), L(942), L(943));
            MatrixTranslateXYZ(&matTemp, L(938), L(939), L(940));
            MatrixMultiply(&matWorld, &matTemp);
            TV(t[0], ap[0]);
            break;
        case 5: turn(3072, 12); break;
        case 6: turn(5120, 20); break;
    }
    LW(944, L(944) + 1);
    LW(944, L(944) % 40);
    LW(930, ap[0]);
    if (L(930) >= L(933))
        LW(930, 0);
}
// The special-stage player's faces (Special function 8, from the Player's update every frame), natively: one statement
// per script instruction, in order. Places the player in the pipe's frame (the Halfpipe's camera values 14-19), then
// emits its shadow (TEXTURED_C_BLEND) and, when visible, its sprite (3DSPRITE) plus the extra sprites of values 4 / 23.
// Patched in only when the function and its jump-table entries match PLAYERFACES_FN_SIG.
__attribute__((optimize("Os"))) static void PS1PlayerFaces()
{
    int *ap = scriptEng.arrayPosition;
    Entity &me = PS1_OBJ(objectEntityPos);
    int *v     = me.values;
    auto copy  = [&](int to) { // VERTEXBUFFER X/Y/Z [ARRAYPOS0] = [ARRAYPOS2]
        Vertex &src = PS1Vertex(ap[2]);
        int x = src.x, y = src.y, z = src.z;
        Vertex &dst = PS1Vertex(to);
        dst.x = x, dst.y = y, dst.z = z;
    };
    auto sprite = [&](int u1, int v1) { // one more 3DSPRITE face at the player's vertex: corner uv (4, 1), (u1, v1), (512, v2)
        ap[0]++;
        ap[1]++;
        PS1Face(ap[1]).flag = 7;
        PS1Face(ap[1]).a    = ap[0];
        copy(ap[0]);
        PS1Vertex(ap[0]).u = 4;
        PS1Vertex(ap[0]).v = 1;
        ap[0]++;
        PS1Face(ap[1]).b = ap[0];
        copy(ap[0]);
        PS1Vertex(ap[0]).u = u1;
        if (u1 < 0) { // value17 >> 2 + 48, computed in the slot as the script does
            PS1Vertex(ap[0]).u = v[17];
            PS1Vertex(ap[0]).u >>= 2;
            PS1Vertex(ap[0]).u += 48;
        }
        PS1Vertex(ap[0]).v = v1;
    };
    MatrixRotateXYZ(&matWorld, PS1_OBJ(0).values[17], PS1_OBJ(0).values[18], PS1_OBJ(0).values[19]);
    MatrixTranslateXYZ(&matTemp, PS1_OBJ(0).values[14], PS1_OBJ(0).values[15], PS1_OBJ(0).values[16]);
    MatrixMultiply(&matWorld, &matTemp);
    ap[0]               = vertexCount;
    ap[1]               = faceCount;
    PS1Face(ap[1]).flag = 6;
    PS1Face(ap[1]).a    = ap[0];
    PS1Vertex(ap[0]).x  = v[11];
    PS1Vertex(ap[0]).y  = v[12];
    PS1Vertex(ap[0]).z  = v[2];
    PS1Vertex(ap[0]).u  = v[13];
    PS1Vertex(ap[0]).v  = 50;
    ap[2]               = ap[0];
    ap[0]++;
    PS1TransformWorld(ap[2], ap[0]);
    PS1Face(ap[1]).b = ap[0];
    copy(ap[0]);
    if (v[2] > 2048) {
        PS1Vertex(ap[0]).u = 1536;
        PS1Vertex(ap[0]).v = 1536;
    }
    else {
        PS1Vertex(ap[0]).u = 1408;
        PS1Vertex(ap[0]).v = 1408;
    }
    ap[0]++;
    PS1Face(ap[1]).c = ap[0];
    copy(ap[0]);
    PS1Vertex(ap[0]).u = 17;
    PS1Vertex(ap[0]).v = 16;
    ap[0]++;
    PS1Face(ap[1]).d = ap[0];
    copy(ap[0]);
    ap[0]++;
    ap[1]++;
    PS1SetVertexCount(vertexCount + 4);
    PS1SetFaceCount(faceCount + 1);
    if (me.visible == 1) {
        g_ps1HalfpipeCover = g_ps1HalfpipeCover | 1u << 14;
        ap[2]              = ap[0];
        ap[0]++;
        PS1Face(ap[1]).flag = 7;
        PS1Face(ap[1]).a    = ap[2];
        PS1Vertex(ap[2]).x  = me.xpos;
        PS1Vertex(ap[2]).x >>= 1;
        v[15]              = PS1Vertex(ap[2]).x;
        PS1Vertex(ap[2]).y = me.ypos;
        PS1Vertex(ap[2]).y >>= 1;
        v[16]              = PS1Vertex(ap[2]).y;
        PS1Vertex(ap[2]).z = v[2];
        PS1TransformWorld(ap[2], ap[0]);
        PS1Vertex(ap[2]).u = 4;
        PS1Vertex(ap[2]).v = 1;
        PS1Face(ap[1]).b   = ap[0];
        copy(ap[0]);
        PS1Vertex(ap[0]).u = me.frame;
        PS1Vertex(ap[0]).v = 0;
        ap[0]++;
        PS1Face(ap[1]).c = ap[0];
        copy(ap[0]);
        PS1Vertex(ap[0]).u = 512;
        PS1Vertex(ap[0]).v = me.rotation;
        ap[0]++;
        PS1Face(ap[1]).d = ap[0];
        copy(ap[0]);
        PS1SetVertexCount(vertexCount + 4);
        PS1SetFaceCount(faceCount + 1);
        if (v[4] == 16 && me.animation == 0) {
            g_ps1HalfpipeCover = g_ps1HalfpipeCover | 1u << 15;
            sprite(-1, 0);
            ap[0]++;
            PS1Face(ap[1]).c = ap[0];
            copy(ap[0]);
            PS1Vertex(ap[0]).u = 512;
            PS1Vertex(ap[0]).v = me.rotation;
            ap[0]++;
            PS1Face(ap[1]).d = ap[0];
            copy(ap[0]);
            PS1SetVertexCount(vertexCount + 4);
            PS1SetFaceCount(faceCount + 1);
        }
    }
    if (v[23] > 0) {
        g_ps1HalfpipeCover = g_ps1HalfpipeCover | 1u << 16;
        sprite(v[23], 0);
        ap[0]++;
        PS1Face(ap[1]).c = ap[0];
        copy(ap[0]);
        PS1Vertex(ap[0]).u = 512;
        PS1Vertex(ap[0]).v = 0;
        ap[0]++;
        PS1Face(ap[1]).d = ap[0];
        copy(ap[0]);
        PS1SetVertexCount(vertexCount + 4);
        PS1SetFaceCount(faceCount + 1);
    }
}
// The special-stage player's running state (Special function 4: the Player's state 4, every frame), natively: one
// statement per script instruction, in order. Left / right speed, the angle around the pipe, the gravity pull, the
// jump (state 5), falling off the top (state 5) and the shadow's offset / frame. Patched in only when the function and
// its jump-table entries match SPPLAYERRUN_FN_SIG.
__attribute__((optimize("Os"))) static void PS1SpecialPlayerRun()
{
    int *t = scriptEng.temp;
    Entity &me = PS1_OBJ(objectEntityPos);
    int *v     = me.values;
    auto mul   = [](int a, int b) { return (int)((uint)a * (uint)b); };
    g_ps1HalfpipeCover = g_ps1HalfpipeCover | (me.right == 1 ? 1u << 17 : me.left == 1 ? 1u << 18 : 1u << 19);
    if (me.right == 1) {
        me.speed -= 48;
        if (me.speed < -1536)
            me.speed = -1536;
        v[26] = 0;
        v[1]  = 0;
    }
    else if (me.left == 1) {
        me.speed += 48;
        if (me.speed > 1536)
            me.speed = 1536;
        v[26] = 0;
        v[1]  = 0;
    }
    else {
        if (v[26] == 0) {
            v[1]  = 60;
            v[26] = 1;
        }
        t[0] = me.speed;
        t[0] >>= 4;
        me.speed -= t[0];
    }
    t[0] = me.speed;
    if (me.rotation != 0) {
        t[0] /= 256;
        t[0] = mul(t[0], 128);
    }
    else
        t[0] >>= 1;
    me.angle += t[0];
    me.angle &= 65535;
    me.rotation = me.angle;
    me.rotation >>= 7;
    if (v[1] > 0)
        v[1]--;
    else {
        t[1] = me.rotation;
        t[1] >>= 1;
        t[0] = Sin256(t[1]);
        t[0] = mul(t[0], -80);
        t[0] >>= 9;
        me.speed += t[0];
    }
    me.xpos = Sin512(me.rotation);
    me.xpos = mul(me.xpos, v[9]);
    me.xpos >>= 9;
    me.ypos = Cos512(me.rotation);
    me.ypos = mul(me.ypos, v[9]);
    me.ypos >>= 9;
    me.animation      = 0;
    me.animationSpeed = 60;
    t[0]              = me.rotation;
    v[25]             = 0;
    if (me.jumpPress == 1) {
        g_ps1HalfpipeCover = g_ps1HalfpipeCover | 1u << 20;
        me.speed           = 0;
        me.xvel            = Sin512(me.rotation);
        me.xvel  = mul(me.xvel, 1440);
        me.xvel >>= 9;
        me.yvel = Cos512(me.rotation);
        me.yvel = mul(me.yvel, 2432);
        me.yvel >>= 9;
        me.state    = 5;
        me.rotation = 0;
        me.frame    = 8;
        me.frame += v[4];
        me.animation      = 8;
        me.animationSpeed = 60;
        me.animationTimer = 0;
        me.gravity        = 1;
        PlaySfx(0, 0);
    }
    else if (me.rotation > 152 && me.rotation < 360 && me.speed < 128 && me.speed > -128) {
        g_ps1HalfpipeCover = g_ps1HalfpipeCover | 1u << 21;
        me.xvel            = me.speed;
        me.speed = 0;
        me.state = 5;
        me.yvel  = 0;
    }
    if (t[0] > 128 && t[0] < 384) {
        t[0] += 256;
        t[0] = -t[0];
        t[0] &= 511;
    }
    v[11] = Sin512(t[0]);
    v[11] = mul(v[11], -52);
    v[11] >>= 1;
    v[12] = Cos512(t[0]);
    v[12] = mul(v[12], -52);
    v[12] >>= 1;
    v[13] = 116;
    if (t[0] < 256) {
        if (t[0] > 112)
            v[13] = 224;
        else if (t[0] > 39)
            v[13] = 149;
    }
    else {
        if (t[0] < 400)
            v[13] = 215;
        else if (t[0] < 473)
            v[13] = 182;
    }
    if (me.rotation < 40)
        me.rotation = 0;
    if (me.rotation > 472)
        me.rotation = 0;
}
// The special stage's object sort at load (the tail of Special Setup's startup sub, from its instruction 81: the
// entities 32-767 bubble-sorted by y, moving type, property value, x and y only; ~270,000 compare steps, 13 s in the
// VM), natively: the same loops and swaps in order, so the slots, temps and array positions end as the script leaves
// them. Patched in (op + End at instruction 81) only when the whole sub and its jump-table entries match
// SPSETUP_STARTUP_SIG.
static void PS1SpecialSetupSort()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int a0 = 32, a1 = 767, a2 = 766;
    while (a0 < 768) {
        a1 = 767;
        a2 = 766;
        while (a1 > a0) {
            Entity &e1 = PS1_OBJ(a1), &e2 = PS1_OBJ(a2);
            if (e1.ypos < e2.ypos) {
                t[0]             = e1.type;
                t[1]             = e1.propertyValue;
                t[2]             = e1.xpos;
                t[3]             = e1.ypos;
                e1.type          = e2.type;
                e1.propertyValue = e2.propertyValue;
                e1.xpos          = e2.xpos;
                e1.ypos          = e2.ypos;
                e2.type          = t[0];
                e2.propertyValue = t[1];
                e2.xpos          = t[2];
                e2.ypos          = t[3];
            }
            a1--;
            a2--;
        }
        a0++;
    }
    ap[0] = a0;
    ap[1] = a1;
    ap[2] = a2;
}
// The special stage's Special Setup update sub (every frame: the ring frame counter, two palette-cycled colours, the
// player-control flag), natively: one statement per script instruction, in order. Patched in only when the sub and its
// jump-table entries match SPSETUP_UPDATE_SIG.
static void PS1SpecialSetupUpdate()
{
    int *t = scriptEng.temp, *G = globalVariables;
    int *v = PS1_OBJ(objectEntityPos).values;
    auto get = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    G[18]++;
    if (G[18] == 4) {
        G[18] = 0;
        G[19]++;
        G[19] &= 7;
    }
    v[1]++;
    if (v[1] == 4) {
        v[1] = 0;
        v[2]++;
        v[2] &= 15;
        get(17, v[2], t[0]);
        SetPaletteEntryPacked(0, 142, t[0]);
        v[3]++;
        v[3] &= 15;
        get(17, v[3], t[0]);
        SetPaletteEntryPacked(0, 143, t[0]);
    }
    if (G[5] == 0) {
        if (PS1_OBJ(2).controlMode > -1)
            G[7] = 1;
        else
            G[7] = 0;
    }
    else
        G[7] = 0;
}
// The Halfpipe's update sub (Special), natively: one statement per script instruction, in order; its segment build
// (function 0) runs natively too (PS1HalfpipeSegment). Advances the pipe (value1 += speed; a new segment each 64) and
// interpolates the camera / pipe tables between segments. Patched in only when the sub and its jump-table entries match
// HALFPIPE_SIG and function 0 matches HPSEG_FN_SIG.
__attribute__((optimize("Os"))) static void PS1HalfpipeUpdate()
{
    int *t = scriptEng.temp;
    Entity &me = PS1_OBJ(objectEntityPos);
    int *v     = me.values;
    auto get   = [](int table, int index, int &dst) { // GetTableValue
        if (index >= 0 && index < scriptCode[table])
            dst = scriptCode[table + index + 1];
    };
    auto mul = [](int a, int b) { return (int)((uint)a * (uint)b); };
    PS1SetVertexCount(scriptCode[933]);
    PS1SetFaceCount(scriptCode[934]);
    v[1] += v[23];
    t[0] = v[23];
    t[0] <<= 13;
    v[13] += t[0];
    if (v[1] > 63) {
        v[1] -= 64;
        PS1HalfpipeSegment(); // CallFunction 0
        v[2]++;
        v[2] %= 40;
        v[3]++;
        v[3] %= 40;
        v[20]++;
        v[20] %= 40;
        v[21]++;
        v[21] %= 40;
    }
    for (int k = 0; k < 3; ++k) { // values 4, 5, 6 from tables 945, 986, 1027, negated
        get(945 + 41 * k, v[2], v[4 + k]);
        get(945 + 41 * k, v[3], t[0]);
        t[0] -= v[4 + k];
        t[0] = mul(t[0], v[1]);
        t[0] >>= 6;
        v[4 + k] += t[0];
        v[4 + k] = -v[4 + k];
    }
    static const int easeShift[3] = { 1, 2, 1 };
    for (int k = 0; k < 3; ++k) { // values 10, 11, 12 ease toward tables 1068, 1109, 1150 (x2)
        get(1068 + 41 * k, v[2], t[1]);
        get(1068 + 41 * k, v[3], t[0]);
        t[0] -= t[1];
        t[0] = mul(t[0], v[1]);
        t[0] >>= 6;
        t[1] += t[0];
        t[1] <<= 1;
        int &e = v[10 + k];
        if (e != t[1]) {
            t[0] = t[1];
            t[0] -= e;
            t[0] >>= easeShift[k];
            if (t[0] == 0) {
                if (e < t[1])
                    e++;
                else
                    e--;
            }
            else
                e += t[0];
        }
    }
    v[7] = v[10];
    v[8] = v[11];
    v[9] = v[12];
    v[7] >>= 1;
    v[8] >>= 1;
    v[9] >>= 1;
    for (int k = 0; k < 6; ++k) { // values 14..19 from tables 945..1150 at values 20 / 21
        get(945 + 41 * k, v[20], v[14 + k]);
        get(945 + 41 * k, v[21], t[0]);
        t[0] -= v[14 + k];
        t[0] = mul(t[0], v[1]);
        t[0] >>= 6;
        v[14 + k] += t[0];
    }
}
// The special stage's Ring update sub (stage object "Ring" in Special, every ring in range each frame), natively: one
// statement per script instruction, in order. Its shadow face (value7 == 1: TEXTURED_C_BLEND) and ring face
// (TEXTURED_C, U / V from tables by the ring's frame) go into the Scene3D buffers; the players (type 4) it touches
// collect it (type 9, the sparkle). Patched in only when the sub and its jump-table entries match SPECIALRING_SIG.
static void PS1SpecialRingUpdate()
{
    int *t = scriptEng.temp, *ap = scriptEng.arrayPosition;
    int *G   = globalVariables;
    int self = objectEntityPos;
    Entity &me = PS1_OBJ(self);
    int *v     = me.values;
    auto scale = [&]() { // the ring's size from its height over the player's
        t[0] = PS1_OBJ(0).ypos;
        t[0] -= 14680064;
        me.scale = me.ypos;
        me.scale -= t[0];
        if (me.scale > 0) {
            me.scale >>= 14;
            me.scale += 1024;
        }
        else
            me.scale = 1024;
    };
    ap[0] = vertexCount;
    ap[1] = faceCount;
    if (v[7] == 1) {
        Face &f = PS1Face(ap[1]);
        f.flag  = 6;
        f.a     = ap[0];
        Vertex *q = &PS1Vertex(ap[0]);
        q->x    = v[11];
        q->y    = v[12];
        q->z    = v[13];
        q->u    = v[14];
        q->v    = 50;
        ap[0]++;
        f.b  = ap[0];
        q    = &PS1Vertex(ap[0]);
        q->x = v[11];
        q->y = v[12];
        q->z = v[13];
        scale();
        q->u = me.scale;
        q->v = me.scale;
        ap[0]++;
        f.c  = ap[0];
        q    = &PS1Vertex(ap[0]);
        q->x = v[11];
        q->y = v[12];
        q->z = v[13];
        q->u = 17;
        q->v = 16;
        ap[0]++;
        f.d  = ap[0];
        q    = &PS1Vertex(ap[0]);
        q->x = v[11];
        q->y = v[12];
        q->z = v[13];
        ap[0]++;
        ap[1]++;
        PS1SetVertexCount(vertexCount + 4);
        PS1SetFaceCount(faceCount + 1);
    }
    Face &f = PS1Face(ap[1]);
    f.flag  = 5;
    f.a     = ap[0];
    Vertex *q = &PS1Vertex(ap[0]);
    q->x    = v[4];
    q->y    = v[5];
    q->z    = v[6];
    if (G[19] >= 0 && G[19] < scriptCode[13793]) // GetTableValue VERTEXBUFFERU GLOBAL19 13793
        q->u = scriptCode[13793 + G[19] + 1];
    if (G[19] >= 0 && G[19] < scriptCode[13802]) // GetTableValue VERTEXBUFFERV GLOBAL19 13802
        q->v = scriptCode[13802 + G[19] + 1];
    ap[0]++;
    f.b  = ap[0];
    q    = &PS1Vertex(ap[0]);
    q->x = v[4];
    q->y = v[5];
    q->z = v[6];
    scale();
    q->u = me.scale;
    q->v = me.scale;
    ap[0]++;
    f.c  = ap[0];
    q    = &PS1Vertex(ap[0]);
    q->x = v[4];
    q->y = v[5];
    q->z = v[6];
    q->u = 16;
    q->v = 16;
    ap[0]++;
    f.d  = ap[0];
    q    = &PS1Vertex(ap[0]);
    q->x = v[4];
    q->y = v[5];
    q->z = v[6];
    PS1SetVertexCount(vertexCount + 4);
    PS1SetFaceCount(faceCount + 1);
    if (v[3] < PS1_OBJ(0).values[13]) {
        me.type = 0;
        return;
    }
    TypeGroupList &players = objectTypeGroupList[4];
    for (int loop = 0; loop < players.listSize; ++loop) { // ForEachActive 4, ARRAYPOS6
        ap[6] = players.entityRefs[loop];
        Entity &p = PS1_OBJ(ap[6]);
        t[1] = v[3];
        t[1] -= 131072;
        t[2] = v[3];
        t[2] += 131072;
        if (p.values[14] > t[1] && p.values[14] < t[2]) {
            t[0] = p.values[15];
            t[0] -= v[1];
            t[1] = t[0];
            t[1] = (int)((uint)t[1] * (uint)t[0]);
            t[0] = p.values[16];
            t[0] -= v[2];
            t[2] = t[0];
            t[2] = (int)((uint)t[2] * (uint)t[0]);
            t[1] += t[2];
            if (t[1] < 12845056) {
                if (G[13] == 0)
                    p.values[0]++;
                else if (G[140] > -1) {
                    if (ap[6] == 2)
                        p.values[0]++;
                }
                else
                    p.values[0]++;
                me.type           = 9;
                me.animationTimer = 0;
                me.frame          = 0;
                if (G[20] == 0) {
                    PlaySfx(1, 0);
                    SetSfxAttributes(1, -1, -100);
                    G[20] = 1;
                }
                else {
                    PlaySfx(2, 0);
                    SetSfxAttributes(2, -1, 100);
                    G[20] = 0;
                }
            }
        }
    }
}
#endif
#if RETRO_REV03 && !RETRO_USE_ORIGINAL_CODE && RETRO_PLATFORM == RETRO_PS1
// PS1: upstream computes inputCheck for every variable operand (two flag loads and branches); only the key
// variables read it. The same value, evaluated where it is read (arrayVal is the operand's, in scope there).
#define inputCheck (!(forceUseScripts || Engine.usingOrigins) || arrayVal <= 1)
#endif
#if RETRO_PLATFORM == RETRO_PS1
#define PS1_RAW(p) scriptCode.raw(p) // always-small fields (Script.hpp PS1ScriptCode::raw)
#else
#define PS1_RAW(p) scriptCode[p]
#endif
#if RETRO_PLATFORM == RETRO_PS1 && PS1_GAME == 1 && !defined(RETRO_PS1_HOST_TOOL)
// Sonic 1's special stage runs ~400 object subs a frame that are just one native opcode (docs/37 phase 7): `op End`,
// `op int End` or `op int int End` as tools/scripts/patch_bytecode.py writes them. For those the VM's per-sub work (the
// instruction loop, operand decoding, dispatch, write-back, then End) is skipped: the native runs directly, with what
// the VM leaves behind done the same way (scriptText cleared per instruction, the operands' values). Anything else,
// and every nested run (a native's PS1CallScriptFunction), takes the VM.
static bool PS1S1FastSub(int p)
{
    int op = PS1_RAW(p);
    if (op < FUNC_PS1SSROTPOS || op > FUNC_PS1S1LZSETUP)
        return false;
    int n = s_ps1OpSize[op];
    for (int i = 0; i < n; ++i)
        if (PS1_RAW(p + 1 + 2 * i) != SCRIPTVAR_INTCONST)
            return false;
    if (PS1_RAW(p + 1 + 2 * n) != FUNC_END)
        return false;
    for (int i = 0; i < n; ++i) scriptEng.operands[i] = scriptCode[p + 2 + 2 * i];
    scriptText[0] = '\0';
    switch (op) {
        case FUNC_PS1SSRING: PS1SSRingUpdate(); break;
        case FUNC_PS1SSBLOCKUPDATE: PS1SSBlockUpdate(); break;
        case FUNC_PS1SSBLOCKDRAW: PS1SSBlockDraw(scriptEng.operands[0], scriptEng.operands[1]); break;
        case FUNC_PS1SSPLACEDRAW: PS1SSPlaceDraw(scriptEng.operands[0]); break;
        case FUNC_PS1SSANIMDRAW: PS1SSAnimDraw(scriptEng.operands[0]); break;
        case FUNC_PS1SSGEMDRAW: PS1SSGemDraw(); break;
        case FUNC_PS1SSOBJUPDATE: PS1SSObjUpdate(scriptEng.operands[0]); break;
        case FUNC_PS1S1ZONEOBJ: PS1S1ZoneObj(scriptEng.operands[0]); break;
        case FUNC_PS1S1SPRING: PS1S1Spring(scriptEng.operands[0]); break;
        case FUNC_PS1S1LZSETUP: PS1S1LZSetup(scriptEng.operands[0]); break;
        default: return false; // functions' natives (`op return`): never a sub
    }
    scriptText[0] = '\0'; // End
    return true;
}
// The same before ProcessScript is entered (ProcessObjects / DrawObjectList, docs/37 phase 7): the stacks reset as
// ProcessScript resets them for a top-level run, then the fast path; false = run ProcessScript as before.
bool PS1S1TopSub(int p)
{
    if (s_ps1CallNested)
        return false;
    jumpTableStackPos = 0;
    functionStackPos  = 0;
    foreachStackPos   = 0;
    return PS1S1FastSub(p);
}
#endif

void ProcessScript(int scriptCodeStart, int jumpTableStart, byte scriptEvent)
{
    bool running      = true;
    int scriptCodePtr = scriptCodeStart;
#if RETRO_PLATFORM == RETRO_PS1
    int ps1FuncBase = 0; // a native's PS1CallScriptFunction: the stacks go on from the caller's; `return` here ends the run
    if (s_ps1CallNested) {
        s_ps1CallNested   = false;
        jumpTableStackPos = s_ps1CallJumpPos;
        functionStackPos  = s_ps1CallFuncPos;
        foreachStackPos   = s_ps1CallForeachPos;
        ps1FuncBase       = functionStackPos;
    }
    else {
        jumpTableStackPos = 0;
        functionStackPos  = 0;
        foreachStackPos   = 0;
    }
#else
    jumpTableStackPos = 0;
    functionStackPos  = 0;
    foreachStackPos   = 0;
#endif
#if RETRO_PLATFORM == RETRO_PS1 && PS1_GAME == 1 && !defined(RETRO_PS1_HOST_TOOL)
    if (!ps1FuncBase && PS1S1FastSub(scriptCodePtr))
        return;
#endif

    while (running) {
        int opcode           = PS1_RAW(scriptCodePtr++);
#if RETRO_PLATFORM == RETRO_PS1
#if PS1_PROFILE
        ++g_ps1VmOps;
#endif
        if ((uint)opcode >= FUNC_MAX_CNT) { // corrupt / desynced code: stop this sub instead of jumping wild
            g_ps1ScriptBadOpcode = g_ps1ScriptBadOpcode + 1;
            break;
        }
#endif
#if RETRO_PLATFORM == RETRO_PS1
        int opcodeSize       = s_ps1OpSize[opcode]; // functions[opcode].opcodeSize as a byte table (ClearScriptData)
#else
        int opcodeSize       = functions[opcode].opcodeSize;
#endif
        int scriptCodeOffset = scriptCodePtr;

        scriptText[0] = '\0';

        // Get Values
        for (int i = 0; i < opcodeSize; ++i) {
            int opcodeType = PS1_RAW(scriptCodePtr++);

            if (opcodeType == SCRIPTVAR_VAR) {
                int arrayVal = 0;
                switch (PS1_RAW(scriptCodePtr++)) {
                    case VARARR_NONE: arrayVal = objectEntityPos; break;

                    case VARARR_ARRAY:
                        if (PS1_RAW(scriptCodePtr++) == 1)
                            arrayVal = scriptEng.arrayPosition[PS1_RAW(scriptCodePtr++)];
                        else
                            arrayVal = scriptCode[scriptCodePtr++];
                        break;

                    case VARARR_ENTNOPLUS1:
                        if (PS1_RAW(scriptCodePtr++) == 1)
                            arrayVal = scriptEng.arrayPosition[PS1_RAW(scriptCodePtr++)] + objectEntityPos;
                        else
                            arrayVal = scriptCode[scriptCodePtr++] + objectEntityPos;
                        break;

                    case VARARR_ENTNOMINUS1:
                        if (PS1_RAW(scriptCodePtr++) == 1)
                            arrayVal = objectEntityPos - scriptEng.arrayPosition[PS1_RAW(scriptCodePtr++)];
                        else
                            arrayVal = objectEntityPos - scriptCode[scriptCodePtr++];
                        break;

                    default: break;
                }

#if RETRO_REV03 && !RETRO_USE_ORIGINAL_CODE && RETRO_PLATFORM != RETRO_PS1 // PS1: inputCheck is evaluated where it is read
                bool inputCheck = true; // Default to true for mobile bytecode
                // If we're using the scripts or an Origins datafile, check the array value
                if (forceUseScripts || Engine.usingOrigins)
                    inputCheck = arrayVal <= 1;
#endif

                // Variables
                switch (PS1_RAW(scriptCodePtr++)) {
                    default: break;
                    case VAR_TEMP0: scriptEng.operands[i] = scriptEng.temp[0]; break;
                    case VAR_TEMP1: scriptEng.operands[i] = scriptEng.temp[1]; break;
                    case VAR_TEMP2: scriptEng.operands[i] = scriptEng.temp[2]; break;
                    case VAR_TEMP3: scriptEng.operands[i] = scriptEng.temp[3]; break;
                    case VAR_TEMP4: scriptEng.operands[i] = scriptEng.temp[4]; break;
                    case VAR_TEMP5: scriptEng.operands[i] = scriptEng.temp[5]; break;
                    case VAR_TEMP6: scriptEng.operands[i] = scriptEng.temp[6]; break;
                    case VAR_TEMP7: scriptEng.operands[i] = scriptEng.temp[7]; break;
                    case VAR_CHECKRESULT: scriptEng.operands[i] = scriptEng.checkResult; break;
                    case VAR_ARRAYPOS0: scriptEng.operands[i] = scriptEng.arrayPosition[0]; break;
                    case VAR_ARRAYPOS1: scriptEng.operands[i] = scriptEng.arrayPosition[1]; break;
                    case VAR_ARRAYPOS2: scriptEng.operands[i] = scriptEng.arrayPosition[2]; break;
                    case VAR_ARRAYPOS3: scriptEng.operands[i] = scriptEng.arrayPosition[3]; break;
                    case VAR_ARRAYPOS4: scriptEng.operands[i] = scriptEng.arrayPosition[4]; break;
                    case VAR_ARRAYPOS5: scriptEng.operands[i] = scriptEng.arrayPosition[5]; break;
                    case VAR_ARRAYPOS6: scriptEng.operands[i] = scriptEng.arrayPosition[6]; break;
                    case VAR_ARRAYPOS7: scriptEng.operands[i] = scriptEng.arrayPosition[7]; break;
                    case VAR_GLOBAL: scriptEng.operands[i] = globalVariables[arrayVal]; break;
                    case VAR_LOCAL: scriptEng.operands[i] = scriptCode[arrayVal]; break;
                    case VAR_OBJECTENTITYPOS: scriptEng.operands[i] = arrayVal; break;
                    case VAR_OBJECTGROUPID: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).groupID;
                        break;
                    }
                    case VAR_OBJECTTYPE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).type;
                        break;
                    }
                    case VAR_OBJECTPROPERTYVALUE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).propertyValue;
                        break;
                    }
                    case VAR_OBJECTXPOS: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).xpos;
                        break;
                    }
                    case VAR_OBJECTYPOS: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).ypos;
                        break;
                    }
                    case VAR_OBJECTIXPOS: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).xpos >> 16;
                        break;
                    }
                    case VAR_OBJECTIYPOS: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).ypos >> 16;
                        break;
                    }
                    case VAR_OBJECTXVEL: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).xvel;
                        break;
                    }
                    case VAR_OBJECTYVEL: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).yvel;
                        break;
                    }
                    case VAR_OBJECTSPEED: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).speed;
                        break;
                    }
                    case VAR_OBJECTSTATE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).state;
                        break;
                    }
                    case VAR_OBJECTROTATION: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).rotation;
                        break;
                    }
                    case VAR_OBJECTSCALE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).scale;
                        break;
                    }
                    case VAR_OBJECTPRIORITY: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).priority;
                        break;
                    }
                    case VAR_OBJECTDRAWORDER: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).drawOrder;
                        break;
                    }
                    case VAR_OBJECTDIRECTION: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).direction;
                        break;
                    }
                    case VAR_OBJECTINKEFFECT: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).inkEffect;
                        break;
                    }
                    case VAR_OBJECTALPHA: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).alpha;
                        break;
                    }
                    case VAR_OBJECTFRAME: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).frame;
                        break;
                    }
                    case VAR_OBJECTANIMATION: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).animation;
                        break;
                    }
                    case VAR_OBJECTPREVANIMATION: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).prevAnimation;
                        break;
                    }
                    case VAR_OBJECTANIMATIONSPEED: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).animationSpeed;
                        break;
                    }
                    case VAR_OBJECTANIMATIONTIMER: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).animationTimer;
                        break;
                    }
                    case VAR_OBJECTANGLE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).angle;
                        break;
                    }
                    case VAR_OBJECTLOOKPOSX: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).lookPosX;
                        break;
                    }
                    case VAR_OBJECTLOOKPOSY: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).lookPosY;
                        break;
                    }
                    case VAR_OBJECTCOLLISIONMODE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).collisionMode;
                        break;
                    }
                    case VAR_OBJECTCOLLISIONPLANE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).collisionPlane;
                        break;
                    }
                    case VAR_OBJECTCONTROLMODE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).controlMode;
                        break;
                    }
                    case VAR_OBJECTCONTROLLOCK: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).controlLock;
                        break;
                    }
                    case VAR_OBJECTPUSHING: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).pushing;
                        break;
                    }
                    case VAR_OBJECTVISIBLE: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).visible;
                        break;
                    }
                    case VAR_OBJECTTILECOLLISIONS: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).tileCollisions;
                        break;
                    }
                    case VAR_OBJECTINTERACTION: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).objectInteractions;
                        break;
                    }
                    case VAR_OBJECTGRAVITY: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).gravity;
                        break;
                    }
                    case VAR_OBJECTUP: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).up;
                        break;
                    }
                    case VAR_OBJECTDOWN: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).down;
                        break;
                    }
                    case VAR_OBJECTLEFT: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).left;
                        break;
                    }
                    case VAR_OBJECTRIGHT: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).right;
                        break;
                    }
                    case VAR_OBJECTJUMPPRESS: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).jumpPress;
                        break;
                    }
                    case VAR_OBJECTJUMPHOLD: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).jumpHold;
                        break;
                    }
                    case VAR_OBJECTSCROLLTRACKING: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).scrollTracking;
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORL: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).floorSensors[0];
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORC: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).floorSensors[1];
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORR: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).floorSensors[2];
                        break;
                    }
#if !RETRO_REV00
                    case VAR_OBJECTFLOORSENSORLC: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).floorSensors[3];
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORRC: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).floorSensors[4];
                        break;
                    }
#endif
                    case VAR_OBJECTCOLLISIONLEFT: {
                        AnimationFile *animFile = objectScriptList[PS1_OBJ(arrayVal).type].animFile;
                        Entity *ent             = &PS1_OBJ(arrayVal);
                        if (animFile) {
                            int h = animFrames[animationList[animFile->aniListOffset + ent->animation].frameListOffset + ent->frame].hitboxID;

                            scriptEng.operands[i] = hitboxList[animFile->hitboxListOffset + h].left[0];
                        }
                        else {
                            scriptEng.operands[i] = 0;
                        }
                        break;
                    }
                    case VAR_OBJECTCOLLISIONTOP: {
                        AnimationFile *animFile = objectScriptList[PS1_OBJ(arrayVal).type].animFile;
                        Entity *ent             = &PS1_OBJ(arrayVal);
                        if (animFile) {
                            int h = animFrames[animationList[animFile->aniListOffset + ent->animation].frameListOffset + ent->frame].hitboxID;

                            scriptEng.operands[i] = hitboxList[animFile->hitboxListOffset + h].top[0];
                        }
                        else {
                            scriptEng.operands[i] = 0;
                        }
                        break;
                    }
                    case VAR_OBJECTCOLLISIONRIGHT: {
                        AnimationFile *animFile = objectScriptList[PS1_OBJ(arrayVal).type].animFile;
                        Entity *ent             = &PS1_OBJ(arrayVal);
                        if (animFile) {
                            int h = animFrames[animationList[animFile->aniListOffset + ent->animation].frameListOffset + ent->frame].hitboxID;

                            scriptEng.operands[i] = hitboxList[animFile->hitboxListOffset + h].right[0];
                        }
                        else {
                            scriptEng.operands[i] = 0;
                        }
                        break;
                    }
                    case VAR_OBJECTCOLLISIONBOTTOM: {
                        AnimationFile *animFile = objectScriptList[PS1_OBJ(arrayVal).type].animFile;
                        Entity *ent             = &PS1_OBJ(arrayVal);
                        if (animFile) {
                            int h = animFrames[animationList[animFile->aniListOffset + ent->animation].frameListOffset + ent->frame].hitboxID;

                            scriptEng.operands[i] = hitboxList[animFile->hitboxListOffset + h].bottom[0];
                        }
                        else {
                            scriptEng.operands[i] = 0;
                        }
                        break;
                    }
                    case VAR_OBJECTOUTOFBOUNDS: {
#if !RETRO_REV00
                        int boundX1_2P = -(0x200 << 16);
                        int boundX2_2P = (0x200 << 16);
                        int boundX3_2P = -(0x180 << 16);
                        int boundX4_2P = (0x180 << 16);

                        int boundY1_2P = -(0x180 << 16);
                        int boundY2_2P = (0x180 << 16);
                        int boundY3_2P = -(0x100 << 16);
                        int boundY4_2P = (0x100 << 16);

                        Entity *entPtr = &PS1_OBJ(arrayVal);
                        int x          = entPtr->xpos >> 16;
                        int y          = entPtr->ypos >> 16;

                        if (entPtr->priority == PRIORITY_BOUNDS_SMALL || entPtr->priority == PRIORITY_ACTIVE_SMALL) {
                            if (stageMode == STAGEMODE_2P) {
                                x = entPtr->xpos;
                                y = entPtr->ypos;

                                int boundL_P1 = objectEntityList[0].xpos + boundX3_2P;
                                int boundR_P1 = objectEntityList[0].xpos + boundX4_2P;
                                int boundT_P1 = objectEntityList[0].ypos + boundY3_2P;
                                int boundB_P1 = objectEntityList[0].ypos + boundY4_2P;

                                int boundL_P2 = objectEntityList[1].xpos + boundX3_2P;
                                int boundR_P2 = objectEntityList[1].xpos + boundX4_2P;
                                int boundT_P2 = objectEntityList[1].ypos + boundY3_2P;
                                int boundB_P2 = objectEntityList[1].ypos + boundY4_2P;

                                bool oobP1 = scriptEng.operands[i] = x <= boundL_P1 || x >= boundR_P1 || y <= boundT_P1 || y >= boundB_P1;
                                bool oobP2 = scriptEng.operands[i] = x <= boundL_P2 || x >= boundR_P2 || y <= boundT_P2 || y >= boundB_P2;

                                scriptEng.operands[i] = oobP1 && oobP2;
                            }
                            else {
                                int boundL = xScrollOffset - OBJECT_BORDER_X3;
                                int boundR = xScrollOffset + OBJECT_BORDER_X4;
                                int boundT = yScrollOffset - OBJECT_BORDER_Y3;
                                int boundB = yScrollOffset + OBJECT_BORDER_Y4;

                                scriptEng.operands[i] = x <= boundL || x >= boundR || y <= boundT || y >= boundB;
                            }
                        }
                        else {
                            if (stageMode == STAGEMODE_2P) {
                                x = entPtr->xpos;
                                y = entPtr->ypos;

                                int boundL_P1 = objectEntityList[0].xpos + boundX1_2P;
                                int boundR_P1 = objectEntityList[0].xpos + boundX2_2P;
                                int boundT_P1 = objectEntityList[0].ypos + boundY1_2P;
                                int boundB_P1 = objectEntityList[0].ypos + boundY2_2P;

                                int boundL_P2 = objectEntityList[1].xpos + boundX1_2P;
                                int boundR_P2 = objectEntityList[1].xpos + boundX2_2P;
                                int boundT_P2 = objectEntityList[1].ypos + boundY1_2P;
                                int boundB_P2 = objectEntityList[1].ypos + boundY2_2P;

                                bool oobP1 = scriptEng.operands[i] = x <= boundL_P1 || x >= boundR_P1 || y <= boundT_P1 || y >= boundB_P1;
                                bool oobP2 = scriptEng.operands[i] = x <= boundL_P2 || x >= boundR_P2 || y <= boundT_P2 || y >= boundB_P2;

                                scriptEng.operands[i] = oobP1 && oobP2;
                            }
                            else {
                                int boundL = xScrollOffset - OBJECT_BORDER_X1;
                                int boundR = xScrollOffset + OBJECT_BORDER_X2;
                                int boundT = yScrollOffset - OBJECT_BORDER_Y1;
                                int boundB = yScrollOffset + OBJECT_BORDER_Y2;

                                scriptEng.operands[i] = x <= boundL || x >= boundR || y <= boundT || y >= boundB;
                            }
                        }
#else
                        int x = PS1_OBJ(arrayVal).xpos >> 16;
                        int y = PS1_OBJ(arrayVal).ypos >> 16;

                        int boundL = xScrollOffset - OBJECT_BORDER_X1;
                        int boundR = xScrollOffset + OBJECT_BORDER_X2;
                        int boundT = yScrollOffset - OBJECT_BORDER_Y1;
                        int boundB = yScrollOffset + OBJECT_BORDER_Y2;

                        scriptEng.operands[i] = x <= boundL || x >= boundR || y <= boundT || y >= boundB;
#endif
                        break;
                    }
                    case VAR_OBJECTSPRITESHEET: {
                        scriptEng.operands[i] = objectScriptList[PS1_OBJ(arrayVal).type].spriteSheetID;
                        break;
                    }
                    case VAR_OBJECTVALUE0: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[0];
                        break;
                    }
                    case VAR_OBJECTVALUE1: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[1];
                        break;
                    }
                    case VAR_OBJECTVALUE2: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[2];
                        break;
                    }
                    case VAR_OBJECTVALUE3: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[3];
                        break;
                    }
                    case VAR_OBJECTVALUE4: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[4];
                        break;
                    }
                    case VAR_OBJECTVALUE5: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[5];
                        break;
                    }
                    case VAR_OBJECTVALUE6: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[6];
                        break;
                    }
                    case VAR_OBJECTVALUE7: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[7];
                        break;
                    }
                    case VAR_OBJECTVALUE8: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[8];
                        break;
                    }
                    case VAR_OBJECTVALUE9: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[9];
                        break;
                    }
                    case VAR_OBJECTVALUE10: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[10];
                        break;
                    }
                    case VAR_OBJECTVALUE11: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[11];
                        break;
                    }
                    case VAR_OBJECTVALUE12: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[12];
                        break;
                    }
                    case VAR_OBJECTVALUE13: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[13];
                        break;
                    }
                    case VAR_OBJECTVALUE14: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[14];
                        break;
                    }
                    case VAR_OBJECTVALUE15: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[15];
                        break;
                    }
                    case VAR_OBJECTVALUE16: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[16];
                        break;
                    }
                    case VAR_OBJECTVALUE17: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[17];
                        break;
                    }
                    case VAR_OBJECTVALUE18: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[18];
                        break;
                    }
                    case VAR_OBJECTVALUE19: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[19];
                        break;
                    }
                    case VAR_OBJECTVALUE20: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[20];
                        break;
                    }
                    case VAR_OBJECTVALUE21: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[21];
                        break;
                    }
                    case VAR_OBJECTVALUE22: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[22];
                        break;
                    }
                    case VAR_OBJECTVALUE23: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[23];
                        break;
                    }
                    case VAR_OBJECTVALUE24: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[24];
                        break;
                    }
                    case VAR_OBJECTVALUE25: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[25];
                        break;
                    }
                    case VAR_OBJECTVALUE26: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[26];
                        break;
                    }
                    case VAR_OBJECTVALUE27: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[27];
                        break;
                    }
                    case VAR_OBJECTVALUE28: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[28];
                        break;
                    }
                    case VAR_OBJECTVALUE29: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[29];
                        break;
                    }
                    case VAR_OBJECTVALUE30: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[30];
                        break;
                    }
                    case VAR_OBJECTVALUE31: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[31];
                        break;
                    }
                    case VAR_OBJECTVALUE32: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[32];
                        break;
                    }
                    case VAR_OBJECTVALUE33: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[33];
                        break;
                    }
                    case VAR_OBJECTVALUE34: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[34];
                        break;
                    }
                    case VAR_OBJECTVALUE35: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[35];
                        break;
                    }
                    case VAR_OBJECTVALUE36: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[36];
                        break;
                    }
                    case VAR_OBJECTVALUE37: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[37];
                        break;
                    }
                    case VAR_OBJECTVALUE38: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[38];
                        break;
                    }
                    case VAR_OBJECTVALUE39: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[39];
                        break;
                    }
                    case VAR_OBJECTVALUE40: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[40];
                        break;
                    }
                    case VAR_OBJECTVALUE41: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[41];
                        break;
                    }
                    case VAR_OBJECTVALUE42: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[42];
                        break;
                    }
                    case VAR_OBJECTVALUE43: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[43];
                        break;
                    }
                    case VAR_OBJECTVALUE44: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[44];
                        break;
                    }
                    case VAR_OBJECTVALUE45: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[45];
                        break;
                    }
                    case VAR_OBJECTVALUE46: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[46];
                        break;
                    }
                    case VAR_OBJECTVALUE47: {
                        scriptEng.operands[i] = PS1_OBJ(arrayVal).values[47];
                        break;
                    }
                    case VAR_STAGESTATE: scriptEng.operands[i] = stageMode; break;
                    case VAR_STAGEACTIVELIST: scriptEng.operands[i] = activeStageList; break;
                    case VAR_STAGELISTPOS: scriptEng.operands[i] = stageListPosition; break;
                    case VAR_STAGETIMEENABLED: scriptEng.operands[i] = timeEnabled; break;
                    case VAR_STAGEMILLISECONDS: scriptEng.operands[i] = stageMilliseconds; break;
                    case VAR_STAGESECONDS: scriptEng.operands[i] = stageSeconds; break;
                    case VAR_STAGEMINUTES: scriptEng.operands[i] = stageMinutes; break;
                    case VAR_STAGEACTNUM: scriptEng.operands[i] = actID; break;
                    case VAR_STAGEPAUSEENABLED: scriptEng.operands[i] = pauseEnabled; break;
                    case VAR_STAGELISTSIZE: scriptEng.operands[i] = stageListCount[activeStageList]; break;
                    case VAR_STAGENEWXBOUNDARY1: scriptEng.operands[i] = newXBoundary1; break;
                    case VAR_STAGENEWXBOUNDARY2: scriptEng.operands[i] = newXBoundary2; break;
                    case VAR_STAGENEWYBOUNDARY1: scriptEng.operands[i] = newYBoundary1; break;
                    case VAR_STAGENEWYBOUNDARY2: scriptEng.operands[i] = newYBoundary2; break;
                    case VAR_STAGECURXBOUNDARY1: scriptEng.operands[i] = curXBoundary1; break;
                    case VAR_STAGECURXBOUNDARY2: scriptEng.operands[i] = curXBoundary2; break;
                    case VAR_STAGECURYBOUNDARY1: scriptEng.operands[i] = curYBoundary1; break;
                    case VAR_STAGECURYBOUNDARY2: scriptEng.operands[i] = curYBoundary2; break;
                    case VAR_STAGEDEFORMATIONDATA0: scriptEng.operands[i] = bgDeformationData0[arrayVal]; break;
                    case VAR_STAGEDEFORMATIONDATA1: scriptEng.operands[i] = bgDeformationData1[arrayVal]; break;
                    case VAR_STAGEDEFORMATIONDATA2: scriptEng.operands[i] = bgDeformationData2[arrayVal]; break;
                    case VAR_STAGEDEFORMATIONDATA3: scriptEng.operands[i] = bgDeformationData3[arrayVal]; break;
                    case VAR_STAGEWATERLEVEL: scriptEng.operands[i] = waterLevel; break;
                    case VAR_STAGEACTIVELAYER: scriptEng.operands[i] = activeTileLayers[arrayVal]; break;
                    case VAR_STAGEMIDPOINT: scriptEng.operands[i] = tLayerMidPoint; break;
                    case VAR_STAGEPLAYERLISTPOS: scriptEng.operands[i] = playerListPos; break;
                    case VAR_STAGEDEBUGMODE: scriptEng.operands[i] = debugMode; break;
                    case VAR_STAGEENTITYPOS: scriptEng.operands[i] = objectEntityPos; break;
                    case VAR_SCREENCAMERAENABLED: scriptEng.operands[i] = cameraEnabled; break;
                    case VAR_SCREENCAMERATARGET: scriptEng.operands[i] = cameraTarget; break;
                    case VAR_SCREENCAMERASTYLE: scriptEng.operands[i] = cameraStyle; break;
                    case VAR_SCREENCAMERAX: scriptEng.operands[i] = cameraXPos; break;
                    case VAR_SCREENCAMERAY: scriptEng.operands[i] = cameraYPos; break;
                    case VAR_SCREENDRAWLISTSIZE: scriptEng.operands[i] = drawListEntries[arrayVal].listSize; break;
                    case VAR_SCREENXCENTER: scriptEng.operands[i] = SCREEN_CENTERX; break;
                    case VAR_SCREENYCENTER: scriptEng.operands[i] = SCREEN_CENTERY; break;
                    case VAR_SCREENXSIZE: scriptEng.operands[i] = SCREEN_XSIZE; break;
                    case VAR_SCREENYSIZE: scriptEng.operands[i] = SCREEN_YSIZE; break;
                    case VAR_SCREENXOFFSET: scriptEng.operands[i] = xScrollOffset; break;
                    case VAR_SCREENYOFFSET: scriptEng.operands[i] = yScrollOffset; break;
                    case VAR_SCREENSHAKEX: scriptEng.operands[i] = cameraShakeX; break;
                    case VAR_SCREENSHAKEY: scriptEng.operands[i] = cameraShakeY; break;
                    case VAR_SCREENADJUSTCAMERAY: scriptEng.operands[i] = cameraAdjustY; break;
                    case VAR_TOUCHSCREENDOWN: scriptEng.operands[i] = touchDown[arrayVal]; break;
                    case VAR_TOUCHSCREENXPOS: scriptEng.operands[i] = touchX[arrayVal]; break;
                    case VAR_TOUCHSCREENYPOS: scriptEng.operands[i] = touchY[arrayVal]; break;
                    case VAR_MUSICVOLUME: scriptEng.operands[i] = masterVolume; break;
                    case VAR_MUSICCURRENTTRACK: scriptEng.operands[i] = trackID; break;
                    case VAR_MUSICPOSITION: scriptEng.operands[i] = musicPosition; break;
#if RETRO_REV03 && !RETRO_USE_ORIGINAL_CODE
                    case VAR_KEYDOWNUP: scriptEng.operands[i] = keyDown.up && inputCheck; break;
                    case VAR_KEYDOWNDOWN: scriptEng.operands[i] = keyDown.down && inputCheck; break;
                    case VAR_KEYDOWNLEFT: scriptEng.operands[i] = keyDown.left && inputCheck; break;
                    case VAR_KEYDOWNRIGHT: scriptEng.operands[i] = keyDown.right && inputCheck; break;
                    case VAR_KEYDOWNBUTTONA: scriptEng.operands[i] = keyDown.A && inputCheck; break;
                    case VAR_KEYDOWNBUTTONB: scriptEng.operands[i] = keyDown.B && inputCheck; break;
                    case VAR_KEYDOWNBUTTONC: scriptEng.operands[i] = keyDown.C && inputCheck; break;
                    case VAR_KEYDOWNBUTTONX: scriptEng.operands[i] = keyDown.X && inputCheck; break;
                    case VAR_KEYDOWNBUTTONY: scriptEng.operands[i] = keyDown.Y && inputCheck; break;
                    case VAR_KEYDOWNBUTTONZ: scriptEng.operands[i] = keyDown.Z && inputCheck; break;
                    case VAR_KEYDOWNBUTTONL: scriptEng.operands[i] = keyDown.L && inputCheck; break;
                    case VAR_KEYDOWNBUTTONR: scriptEng.operands[i] = keyDown.R && inputCheck; break;
                    case VAR_KEYDOWNSTART: scriptEng.operands[i] = keyDown.start && inputCheck; break;
                    case VAR_KEYDOWNSELECT: scriptEng.operands[i] = keyDown.select && inputCheck; break;
                    case VAR_KEYPRESSUP: scriptEng.operands[i] = keyPress.up && inputCheck; break;
                    case VAR_KEYPRESSDOWN: scriptEng.operands[i] = keyPress.down && inputCheck; break;
                    case VAR_KEYPRESSLEFT: scriptEng.operands[i] = keyPress.left && inputCheck; break;
                    case VAR_KEYPRESSRIGHT: scriptEng.operands[i] = keyPress.right && inputCheck; break;
                    case VAR_KEYPRESSBUTTONA: scriptEng.operands[i] = keyPress.A && inputCheck; break;
                    case VAR_KEYPRESSBUTTONB: scriptEng.operands[i] = keyPress.B && inputCheck; break;
                    case VAR_KEYPRESSBUTTONC: scriptEng.operands[i] = keyPress.C && inputCheck; break;
                    case VAR_KEYPRESSBUTTONX: scriptEng.operands[i] = keyPress.X && inputCheck; break;
                    case VAR_KEYPRESSBUTTONY: scriptEng.operands[i] = keyPress.Y && inputCheck; break;
                    case VAR_KEYPRESSBUTTONZ: scriptEng.operands[i] = keyPress.Z && inputCheck; break;
                    case VAR_KEYPRESSBUTTONL: scriptEng.operands[i] = keyPress.L && inputCheck; break;
                    case VAR_KEYPRESSBUTTONR: scriptEng.operands[i] = keyPress.R && inputCheck; break;
                    case VAR_KEYPRESSSTART: scriptEng.operands[i] = keyPress.start && inputCheck; break;
                    case VAR_KEYPRESSSELECT: scriptEng.operands[i] = keyPress.select && inputCheck; break;
#else
                    case VAR_KEYDOWNUP: scriptEng.operands[i] = keyDown.up; break;
                    case VAR_KEYDOWNDOWN: scriptEng.operands[i] = keyDown.down; break;
                    case VAR_KEYDOWNLEFT: scriptEng.operands[i] = keyDown.left; break;
                    case VAR_KEYDOWNRIGHT: scriptEng.operands[i] = keyDown.right; break;
                    case VAR_KEYDOWNBUTTONA: scriptEng.operands[i] = keyDown.A; break;
                    case VAR_KEYDOWNBUTTONB: scriptEng.operands[i] = keyDown.B; break;
                    case VAR_KEYDOWNBUTTONC: scriptEng.operands[i] = keyDown.C; break;
                    case VAR_KEYDOWNBUTTONX: scriptEng.operands[i] = keyDown.X; break;
                    case VAR_KEYDOWNBUTTONY: scriptEng.operands[i] = keyDown.Y; break;
                    case VAR_KEYDOWNBUTTONZ: scriptEng.operands[i] = keyDown.Z; break;
                    case VAR_KEYDOWNBUTTONL: scriptEng.operands[i] = keyDown.L; break;
                    case VAR_KEYDOWNBUTTONR: scriptEng.operands[i] = keyDown.R; break;
                    case VAR_KEYDOWNSTART: scriptEng.operands[i] = keyDown.start; break;
                    case VAR_KEYDOWNSELECT: scriptEng.operands[i] = keyDown.select; break;
                    case VAR_KEYPRESSUP: scriptEng.operands[i] = keyPress.up; break;
                    case VAR_KEYPRESSDOWN: scriptEng.operands[i] = keyPress.down; break;
                    case VAR_KEYPRESSLEFT: scriptEng.operands[i] = keyPress.left; break;
                    case VAR_KEYPRESSRIGHT: scriptEng.operands[i] = keyPress.right; break;
                    case VAR_KEYPRESSBUTTONA: scriptEng.operands[i] = keyPress.A; break;
                    case VAR_KEYPRESSBUTTONB: scriptEng.operands[i] = keyPress.B; break;
                    case VAR_KEYPRESSBUTTONC: scriptEng.operands[i] = keyPress.C; break;
                    case VAR_KEYPRESSBUTTONX: scriptEng.operands[i] = keyPress.X; break;
                    case VAR_KEYPRESSBUTTONY: scriptEng.operands[i] = keyPress.Y; break;
                    case VAR_KEYPRESSBUTTONZ: scriptEng.operands[i] = keyPress.Z; break;
                    case VAR_KEYPRESSBUTTONL: scriptEng.operands[i] = keyPress.L; break;
                    case VAR_KEYPRESSBUTTONR: scriptEng.operands[i] = keyPress.R; break;
                    case VAR_KEYPRESSSTART: scriptEng.operands[i] = keyPress.start; break;
                    case VAR_KEYPRESSSELECT: scriptEng.operands[i] = keyPress.select; break;
#endif
                    case VAR_MENU1SELECTION: scriptEng.operands[i] = gameMenu[0].selection1; break;
                    case VAR_MENU2SELECTION: scriptEng.operands[i] = gameMenu[1].selection1; break;
                    case VAR_TILELAYERXSIZE: scriptEng.operands[i] = stageLayouts[arrayVal].xsize; break;
                    case VAR_TILELAYERYSIZE: scriptEng.operands[i] = stageLayouts[arrayVal].ysize; break;
                    case VAR_TILELAYERTYPE: scriptEng.operands[i] = stageLayouts[arrayVal].type; break;
                    case VAR_TILELAYERANGLE: scriptEng.operands[i] = stageLayouts[arrayVal].angle; break;
                    case VAR_TILELAYERXPOS: scriptEng.operands[i] = stageLayouts[arrayVal].xpos; break;
                    case VAR_TILELAYERYPOS: scriptEng.operands[i] = stageLayouts[arrayVal].ypos; break;
                    case VAR_TILELAYERZPOS: scriptEng.operands[i] = stageLayouts[arrayVal].zpos; break;
                    case VAR_TILELAYERPARALLAXFACTOR: scriptEng.operands[i] = stageLayouts[arrayVal].parallaxFactor; break;
                    case VAR_TILELAYERSCROLLSPEED: scriptEng.operands[i] = stageLayouts[arrayVal].scrollSpeed; break;
                    case VAR_TILELAYERSCROLLPOS: scriptEng.operands[i] = stageLayouts[arrayVal].scrollPos; break;
                    case VAR_TILELAYERDEFORMATIONOFFSET: scriptEng.operands[i] = stageLayouts[arrayVal].deformationOffset; break;
                    case VAR_TILELAYERDEFORMATIONOFFSETW: scriptEng.operands[i] = stageLayouts[arrayVal].deformationOffsetW; break;
                    case VAR_HPARALLAXPARALLAXFACTOR: scriptEng.operands[i] = hParallax.parallaxFactor[arrayVal]; break;
                    case VAR_HPARALLAXSCROLLSPEED: scriptEng.operands[i] = hParallax.scrollSpeed[arrayVal]; break;
                    case VAR_HPARALLAXSCROLLPOS: scriptEng.operands[i] = hParallax.scrollPos[arrayVal]; break;
                    case VAR_VPARALLAXPARALLAXFACTOR: scriptEng.operands[i] = vParallax.parallaxFactor[arrayVal]; break;
                    case VAR_VPARALLAXSCROLLSPEED: scriptEng.operands[i] = vParallax.scrollSpeed[arrayVal]; break;
                    case VAR_VPARALLAXSCROLLPOS: scriptEng.operands[i] = vParallax.scrollPos[arrayVal]; break;
                    case VAR_SCENE3DVERTEXCOUNT: scriptEng.operands[i] = vertexCount; break;
                    case VAR_SCENE3DFACECOUNT: scriptEng.operands[i] = faceCount; break;
                    case VAR_SCENE3DPROJECTIONX: scriptEng.operands[i] = projectionX; break;
                    case VAR_SCENE3DPROJECTIONY: scriptEng.operands[i] = projectionY; break;
#if !RETRO_REV00
                    case VAR_SCENE3DFOGCOLOR: scriptEng.operands[i] = fogColor; break;
                    case VAR_SCENE3DFOGSTRENGTH: scriptEng.operands[i] = fogStrength; break;
#endif
                    case VAR_VERTEXBUFFERX: scriptEng.operands[i] = vertexBuffer[PS1_VTX(arrayVal)].x; break;
                    case VAR_VERTEXBUFFERY: scriptEng.operands[i] = vertexBuffer[PS1_VTX(arrayVal)].y; break;
                    case VAR_VERTEXBUFFERZ: scriptEng.operands[i] = vertexBuffer[PS1_VTX(arrayVal)].z; break;
                    case VAR_VERTEXBUFFERU: scriptEng.operands[i] = vertexBuffer[PS1_VTX(arrayVal)].u; break;
                    case VAR_VERTEXBUFFERV: scriptEng.operands[i] = vertexBuffer[PS1_VTX(arrayVal)].v; break;
                    case VAR_FACEBUFFERA: scriptEng.operands[i] = faceBuffer[PS1_FACE(arrayVal)].a; break;
                    case VAR_FACEBUFFERB: scriptEng.operands[i] = faceBuffer[PS1_FACE(arrayVal)].b; break;
                    case VAR_FACEBUFFERC: scriptEng.operands[i] = faceBuffer[PS1_FACE(arrayVal)].c; break;
                    case VAR_FACEBUFFERD: scriptEng.operands[i] = faceBuffer[PS1_FACE(arrayVal)].d; break;
                    case VAR_FACEBUFFERFLAG: scriptEng.operands[i] = faceBuffer[PS1_FACE(arrayVal)].flag; break;
                    case VAR_FACEBUFFERCOLOR: scriptEng.operands[i] = faceBuffer[PS1_FACE(arrayVal)].color; break;
#if RETRO_PLATFORM == RETRO_PS1
                    case VAR_SAVERAM:
                        if ((uint)arrayVal < SAVEDATA_SIZE)
                            scriptEng.operands[i] = saveRAM[arrayVal];
                        else {
                            scriptEng.operands[i] = 0;
                            g_ps1SaveRAMClamped   = g_ps1SaveRAMClamped + 1;
                        }
                        break;
#else
                    case VAR_SAVERAM: scriptEng.operands[i] = saveRAM[arrayVal]; break;
#endif
                    case VAR_ENGINESTATE: scriptEng.operands[i] = Engine.gameMode; break;
#if RETRO_REV00
                    case VAR_ENGINEMESSAGE: scriptEng.operands[i] = Engine.message; break;
#endif
                    case VAR_ENGINELANGUAGE: scriptEng.operands[i] = Engine.language; break;
                    case VAR_ENGINEONLINEACTIVE: scriptEng.operands[i] = Engine.onlineActive; break;
                    case VAR_ENGINESFXVOLUME: scriptEng.operands[i] = sfxVolume; break;
                    case VAR_ENGINEBGMVOLUME: scriptEng.operands[i] = bgmVolume; break;
#if RETRO_REV00
                    case VAR_ENGINEPLATFORMID: scriptEng.operands[i] = RETRO_GAMEPLATFORMID; break;
#endif
                    case VAR_ENGINETRIALMODE: scriptEng.operands[i] = Engine.trialMode; break;
#if !RETRO_REV00
                    case VAR_ENGINEDEVICETYPE: scriptEng.operands[i] = RETRO_DEVICETYPE; break;
#endif

#if RETRO_REV03
                    // Origins Extras
                    // Due to using regular v4, these don't support array values like origins expects, so its always screen[0]
                    case VAR_SCREENCURRENTID: scriptEng.operands[i] = 0; break;
                    case VAR_CAMERAENABLED:
                        if (arrayVal == 0)
                            scriptEng.operands[i] = cameraEnabled;
                        else
                            scriptEng.operands[i] = 0;
                        break;
                    case VAR_CAMERATARGET:
                        if (arrayVal == 0)
                            scriptEng.operands[i] = cameraTarget;
                        else
                            scriptEng.operands[i] = 0;
                        break;
                    case VAR_CAMERASTYLE:
                        if (arrayVal == 0)
                            scriptEng.operands[i] = cameraStyle;
                        else
                            scriptEng.operands[i] = 0;
                        break;
                    case VAR_CAMERAXPOS:
                        if (arrayVal == 0)
                            scriptEng.operands[i] = cameraXPos;
                        else
                            scriptEng.operands[i] = 0;
                        break;
                    case VAR_CAMERAYPOS:
                        if (arrayVal == 0)
                            scriptEng.operands[i] = cameraYPos;
                        else
                            scriptEng.operands[i] = 0;
                        break;
                    case VAR_CAMERAADJUSTY:
                        if (arrayVal == 0)
                            scriptEng.operands[i] = cameraAdjustY;
                        else
                            scriptEng.operands[i] = 0;
                        break;
#endif

#if RETRO_USE_HAPTICS
                    case VAR_HAPTICSENABLED: scriptEng.operands[i] = Engine.hapticsEnabled; break;
#endif
                }
            }
            else if (opcodeType == SCRIPTVAR_INTCONST) { // int constant
                scriptEng.operands[i] = scriptCode[scriptCodePtr++];
            }
            else if (opcodeType == SCRIPTVAR_STRCONST) { // string constant
                int strLen         = scriptCode[scriptCodePtr++];
                PS1_TXT(strLen) = 0;
                for (int c = 0; c < strLen; ++c) {
                    switch (c % 4) {
                        case 0: 
                            PS1_TXT(c) = scriptCode[scriptCodePtr] >> 24;
                            break;

                        case 1: 
                            PS1_TXT(c) = (0xFFFFFF & scriptCode[scriptCodePtr]) >> 16;
                            break;

                        case 2: 
                            PS1_TXT(c) = (0xFFFF & scriptCode[scriptCodePtr]) >> 8;
                            break;

                        case 3: 
                            PS1_TXT(c) = scriptCode[scriptCodePtr++];
                            break;

                        default: break;
                    }
                }
                scriptCodePtr++;
            }
        }

#if RETRO_PLATFORM == RETRO_PS1
        // PS1: upstream computes these two for every instruction; they are evaluated where read instead (the same
        // values: no instruction changes objectEntityPos or its own entity's type before reading them).
#define scriptInfo (&objectScriptList[objectEntityList[objectEntityPos].type])
#define entity     (&objectEntityList[objectEntityPos])
#else
        ObjectScript *scriptInfo = &objectScriptList[objectEntityList[objectEntityPos].type];
        Entity *entity           = &objectEntityList[objectEntityPos];
#endif
        SpriteFrame *spriteFrame = nullptr;

        // Functions
        switch (opcode) {
            default: break;
            case FUNC_END: running = false; break;
            case FUNC_EQUAL: scriptEng.operands[0] = scriptEng.operands[1]; break;
            case FUNC_ADD: scriptEng.operands[0] += scriptEng.operands[1]; break;
            case FUNC_SUB: scriptEng.operands[0] -= scriptEng.operands[1]; break;
            case FUNC_INC: ++scriptEng.operands[0]; break;
            case FUNC_DEC: --scriptEng.operands[0]; break;
            case FUNC_MUL: scriptEng.operands[0] *= scriptEng.operands[1]; break;
            case FUNC_DIV:
#if RETRO_PLATFORM == RETRO_PS1 // GCC's zero-division check is a `break 7` trap on the R3000A (docs/09)
                if (!scriptEng.operands[1]) {
                    g_ps1ScriptDivZero    = g_ps1ScriptDivZero + 1;
                    scriptEng.operands[0] = 0;
                    break;
                }
#endif
                scriptEng.operands[0] /= scriptEng.operands[1];
                break;
            case FUNC_SHR: scriptEng.operands[0] >>= scriptEng.operands[1]; break;
            case FUNC_SHL: scriptEng.operands[0] <<= scriptEng.operands[1]; break;
            case FUNC_AND: scriptEng.operands[0] &= scriptEng.operands[1]; break;
            case FUNC_OR: scriptEng.operands[0] |= scriptEng.operands[1]; break;
            case FUNC_XOR: scriptEng.operands[0] ^= scriptEng.operands[1]; break;
            case FUNC_MOD:
#if RETRO_PLATFORM == RETRO_PS1 // GCC's zero-division check is a `break 7` trap on the R3000A (docs/09)
                if (!scriptEng.operands[1]) {
                    g_ps1ScriptDivZero    = g_ps1ScriptDivZero + 1;
                    scriptEng.operands[0] = 0;
                    break;
                }
#endif
                scriptEng.operands[0] %= scriptEng.operands[1];
                break;
            case FUNC_FLIPSIGN: scriptEng.operands[0] = -scriptEng.operands[0]; break;
            case FUNC_CHECKEQUAL:
                scriptEng.checkResult = scriptEng.operands[0] == scriptEng.operands[1];
                opcodeSize            = 0;
                break;
            case FUNC_CHECKGREATER:
                scriptEng.checkResult = scriptEng.operands[0] > scriptEng.operands[1];
                opcodeSize            = 0;
                break;
            case FUNC_CHECKLOWER:
                scriptEng.checkResult = scriptEng.operands[0] < scriptEng.operands[1];
                opcodeSize            = 0;
                break;
            case FUNC_CHECKNOTEQUAL:
                scriptEng.checkResult = scriptEng.operands[0] != scriptEng.operands[1];
                opcodeSize            = 0;
                break;
            case FUNC_IFEQUAL:
                if (scriptEng.operands[1] != scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0]];
                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize                          = 0;
                break;
            case FUNC_IFGREATER:
                if (scriptEng.operands[1] <= scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0]];
                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize                          = 0;
                break;
            case FUNC_IFGREATEROREQUAL:
                if (scriptEng.operands[1] < scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0]];
                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize                          = 0;
                break;
            case FUNC_IFLOWER:
                if (scriptEng.operands[1] >= scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0]];
                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize                          = 0;
                break;
            case FUNC_IFLOWEROREQUAL:
                if (scriptEng.operands[1] > scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0]];
                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize                          = 0;
                break;
            case FUNC_IFNOTEQUAL:
                if (scriptEng.operands[1] == scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0]];
                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize                          = 0;
                break;
            case FUNC_ELSE:
                opcodeSize    = 0;
                scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + jumpTableStack[jumpTableStackPos--] + 1];
                break;
            case FUNC_ENDIF:
                opcodeSize = 0;
                --jumpTableStackPos;
                break;
            case FUNC_WEQUAL:
                if (scriptEng.operands[1] != scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                else
                    jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize = 0;
                break;
            case FUNC_WGREATER:
                if (scriptEng.operands[1] <= scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                else
                    jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize = 0;
                break;
            case FUNC_WGREATEROREQUAL:
                if (scriptEng.operands[1] < scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                else
                    jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize = 0;
                break;
            case FUNC_WLOWER:
                if (scriptEng.operands[1] >= scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                else
                    jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize = 0;
                break;
            case FUNC_WLOWEROREQUAL:
                if (scriptEng.operands[1] > scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                else
                    jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize = 0;
                break;
            case FUNC_WNOTEQUAL:
                if (scriptEng.operands[1] == scriptEng.operands[2])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                else
                    jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                opcodeSize = 0;
                break;
            case FUNC_LOOP:
                opcodeSize    = 0;
                scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + jumpTableStack[jumpTableStackPos--]];
                break;
            case FUNC_FOREACHACTIVE: {
                int groupID = scriptEng.operands[1];
                if (groupID < TYPEGROUP_COUNT) {
                    int loop                      = foreachStack[++foreachStackPos] + 1;
                    foreachStack[foreachStackPos] = loop;
                    if (loop >= objectTypeGroupList[groupID].listSize) {
                        opcodeSize                      = 0;
                        foreachStack[foreachStackPos--] = -1;
                        scriptCodePtr                   = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                        break;
                    }
                    else {
                        scriptEng.operands[2]               = objectTypeGroupList[groupID].entityRefs[loop];
                        jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                    }
                }
                else {
                    opcodeSize    = 0;
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                }
                break;
            }
            case FUNC_FOREACHALL: {
                int objType = scriptEng.operands[1];
                if (objType < OBJECT_COUNT) {
                    int loop                      = foreachStack[++foreachStackPos] + 1;
                    foreachStack[foreachStackPos] = loop;

                    if (scriptEvent == EVENT_SETUP) {
                        while (true) {
                            if (loop >= TEMPENTITY_START) {
                                opcodeSize                      = 0;
                                foreachStack[foreachStackPos--] = -1;
                                scriptCodePtr                   = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                                break;
                            }
                            else if (objType == objectEntityList[loop].type) {
                                scriptEng.operands[2]               = loop;
                                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                                break;
                            }
                            else {
                                foreachStack[foreachStackPos] = ++loop;
                            }
                        }
                    }
                    else {
                        while (true) {
                            if (loop >= ENTITY_COUNT) {
                                opcodeSize                      = 0;
                                foreachStack[foreachStackPos--] = -1;
                                scriptCodePtr                   = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                                break;
                            }
                            else if (objType == objectEntityList[loop].type) {
                                scriptEng.operands[2]               = loop;
                                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                                break;
                            }
                            else {
                                foreachStack[foreachStackPos] = ++loop;
                            }
                        }
                    }
                }
                else {
                    opcodeSize    = 0;
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 1];
                }
                break;
            }
            case FUNC_NEXT:
                opcodeSize    = 0;
                scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + jumpTableStack[jumpTableStackPos--]];
                --foreachStackPos;
                break;
            case FUNC_SWITCH:
                jumpTableStack[++jumpTableStackPos] = scriptEng.operands[0];
                if (scriptEng.operands[1] < jumpTable[jumpTableStart + scriptEng.operands[0]]
                    || scriptEng.operands[1] > jumpTable[jumpTableStart + scriptEng.operands[0] + 1])
                    scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + scriptEng.operands[0] + 2];
                else
                    scriptCodePtr = scriptCodeStart
                                    + jumpTable[jumpTableStart + scriptEng.operands[0] + 4
                                                    + (scriptEng.operands[1] - jumpTable[jumpTableStart + scriptEng.operands[0]])];
                opcodeSize = 0;
                break;
            case FUNC_BREAK:
                opcodeSize    = 0;
                scriptCodePtr = scriptCodeStart + jumpTable[jumpTableStart + jumpTableStack[jumpTableStackPos--] + 3];
                break;
            case FUNC_ENDSWITCH:
                opcodeSize = 0;
                --jumpTableStackPos;
                break;
            case FUNC_RAND:
#if RETRO_PLATFORM == RETRO_PS1 // GCC's zero-division check is a `break 7` trap on the R3000A (docs/09)
                if (!scriptEng.operands[1]) {
                    g_ps1ScriptDivZero    = g_ps1ScriptDivZero + 1;
                    scriptEng.operands[0] = 0;
                    break;
                }
#endif
                scriptEng.operands[0] = rand() % scriptEng.operands[1];
                break;
            case FUNC_SIN: {
                scriptEng.operands[0] = Sin512(scriptEng.operands[1]);
                break;
            }
            case FUNC_COS: {
                scriptEng.operands[0] = Cos512(scriptEng.operands[1]);
                break;
            }
            case FUNC_SIN256: {
                scriptEng.operands[0] = Sin256(scriptEng.operands[1]);
                break;
            }
            case FUNC_COS256: {
                scriptEng.operands[0] = Cos256(scriptEng.operands[1]);
                break;
            }
            case FUNC_ATAN2: {
                scriptEng.operands[0] = ArcTanLookup(scriptEng.operands[1], scriptEng.operands[2]);
                break;
            }
            case FUNC_INTERPOLATE:
                scriptEng.operands[0] =
                    (scriptEng.operands[2] * (0x100 - scriptEng.operands[3]) + scriptEng.operands[3] * scriptEng.operands[1]) >> 8;
                break;
            case FUNC_INTERPOLATEXY:
                scriptEng.operands[0] =
                    (scriptEng.operands[3] * (0x100 - scriptEng.operands[6]) >> 8) + ((scriptEng.operands[6] * scriptEng.operands[2]) >> 8);
                scriptEng.operands[1] =
                    (scriptEng.operands[5] * (0x100 - scriptEng.operands[6]) >> 8) + (scriptEng.operands[6] * scriptEng.operands[4] >> 8);
                break;
            case FUNC_LOADSPRITESHEET:
                opcodeSize                = 0;
                scriptInfo->spriteSheetID = AddGraphicsFile(scriptText);
                break;
            case FUNC_REMOVESPRITESHEET:
                opcodeSize = 0;
                RemoveGraphicsFile(scriptText, -1);
                break;
            case FUNC_DRAWSPRITE:
                opcodeSize  = 0;
                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + scriptEng.operands[0]];
                DrawSprite((entity->xpos >> 16) - xScrollOffset + spriteFrame->pivotX, (entity->ypos >> 16) - yScrollOffset + spriteFrame->pivotY,
                           spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                break;
            case FUNC_DRAWSPRITEXY:
                opcodeSize  = 0;
                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + scriptEng.operands[0]];
                DrawSprite((scriptEng.operands[1] >> 16) - xScrollOffset + spriteFrame->pivotX,
                           (scriptEng.operands[2] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width, spriteFrame->height,
                           spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                break;
            case FUNC_DRAWSPRITESCREENXY:
                opcodeSize  = 0;
                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + scriptEng.operands[0]];
                DrawSprite(scriptEng.operands[1] + spriteFrame->pivotX, scriptEng.operands[2] + spriteFrame->pivotY, spriteFrame->width,
                           spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                break;
            case FUNC_DRAWTINTRECT:
                opcodeSize = 0;
                DrawTintRectangle(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]);
                break;
            case FUNC_DRAWNUMBERS: {
                opcodeSize = 0;
                int i      = 10;
                if (scriptEng.operands[6]) {
                    while (scriptEng.operands[4] > 0) {
                        int frameID = scriptEng.operands[3] % i / (i / 10) + scriptEng.operands[0];
                        spriteFrame = &scriptFrames[scriptInfo->frameListOffset + frameID];
                        DrawSprite(spriteFrame->pivotX + scriptEng.operands[1], spriteFrame->pivotY + scriptEng.operands[2], spriteFrame->width,
                                   spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                        scriptEng.operands[1] -= scriptEng.operands[5];
                        i *= 10;
                        --scriptEng.operands[4];
                    }
                }
                else {
                    int extra = 10;
                    if (scriptEng.operands[3])
                        extra = 10 * scriptEng.operands[3];
                    while (scriptEng.operands[4] > 0) {
                        if (extra >= i) {
                            int frameID = scriptEng.operands[3] % i / (i / 10) + scriptEng.operands[0];
                            spriteFrame = &scriptFrames[scriptInfo->frameListOffset + frameID];
                            DrawSprite(spriteFrame->pivotX + scriptEng.operands[1], spriteFrame->pivotY + scriptEng.operands[2], spriteFrame->width,
                                       spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                        }
                        scriptEng.operands[1] -= scriptEng.operands[5];
                        i *= 10;
                        --scriptEng.operands[4];
                    }
                }
                break;
            }
            case FUNC_DRAWACTNAME: {
                opcodeSize = 0;
                int charID = 0;
                switch (scriptEng.operands[3]) { // Draw Mode
                    case 0:                      // Draw Word 1 (but aligned from the right instead of left)
                        charID = 0;

                        for (charID = 0;; ++charID) {
                            int nextChar = titleCardText[charID + 1];
                            if (nextChar == '-' || !nextChar)
                                break;
                        }

                        while (charID >= 0) {
                            int character = titleCardText[charID];
                            if (character == ' ')
                                character = -1; // special space char
                            if (character == '-')
                                character = 0;
                            if (character >= '0' && character <= '9')
                                character -= 22;
                            if (character > '9' && character < 'f')
                                character -= 'A';

                            if (character <= -1) {
                                scriptEng.operands[1] -= scriptEng.operands[5] + scriptEng.operands[6]; // spaceWidth + spacing
                            }
                            else {
                                character += scriptEng.operands[0];
                                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + character];

                                scriptEng.operands[1] -= spriteFrame->width + scriptEng.operands[6];

                                DrawSprite(scriptEng.operands[1] + spriteFrame->pivotX, scriptEng.operands[2] + spriteFrame->pivotY,
                                           spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                            }
                            charID--;
                        }
                        break;

                    case 1: // Draw Word 1
                        charID = 0;

                        // Draw the first letter as a capital letter, the rest are lowercase (if scriptEng.operands[4] is true, otherwise they're all
                        // uppercase)
                        if (scriptEng.operands[4] == 1 && titleCardText[charID] != 0) {
                            int character = titleCardText[charID];
                            if (character == ' ')
                                character = -1;
                            if (character == '-')
                                character = 0;
                            if (character >= '0' && character <= '9')
                                character -= 22;
                            if (character > '9' && character < 'f')
                                character -= 'A';

                            if (character <= -1) {
                                scriptEng.operands[1] += scriptEng.operands[5] + scriptEng.operands[6]; // spaceWidth + spacing
                            }
                            else {
                                character += scriptEng.operands[0];
                                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + character];
                                DrawSprite(scriptEng.operands[1] + spriteFrame->pivotX, scriptEng.operands[2] + spriteFrame->pivotY,
                                           spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                                scriptEng.operands[1] += spriteFrame->width + scriptEng.operands[6];
                            }

                            scriptEng.operands[0] += 26;
                            charID++;
                        }

                        while (titleCardText[charID] != 0 && titleCardText[charID] != '-') {
                            int character = titleCardText[charID];
                            if (character == ' ')
                                character = -1;
                            if (character == '-')
                                character = 0;
                            if (character > '/' && character < ':')
                                character -= 22;
                            if (character > '9' && character < 'f')
                                character -= 'A';

                            if (character <= -1) {
                                scriptEng.operands[1] += scriptEng.operands[5] + scriptEng.operands[6]; // spaceWidth + spacing
                            }
                            else {
                                character += scriptEng.operands[0];
                                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + character];
                                DrawSprite(scriptEng.operands[1] + spriteFrame->pivotX, scriptEng.operands[2] + spriteFrame->pivotY,
                                           spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                                scriptEng.operands[1] += spriteFrame->width + scriptEng.operands[6];
                            }
                            charID++;
                        }
                        break;

                    case 2: // Draw Word 2
                        charID = titleCardWord2;

                        // Draw the first letter as a capital letter, the rest are lowercase (if scriptEng.operands[4] is true, otherwise they're all
                        // uppercase)
                        if (scriptEng.operands[4] == 1 && titleCardText[charID] != 0) {
                            int character = titleCardText[charID];
                            if (character == ' ')
                                character = 0;
                            if (character == '-')
                                character = 0;
                            if (character >= '0' && character <= '9')
                                character -= 22;
                            if (character > '9' && character < 'f')
                                character -= 'A';

                            if (character <= -1) {
                                scriptEng.operands[1] += scriptEng.operands[5] + scriptEng.operands[6]; // spaceWidth + spacing
                            }
                            else {
                                character += scriptEng.operands[0];
                                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + character];
                                DrawSprite(scriptEng.operands[1] + spriteFrame->pivotX, scriptEng.operands[2] + spriteFrame->pivotY,
                                           spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                                scriptEng.operands[1] += spriteFrame->width + scriptEng.operands[6];
                            }
                            scriptEng.operands[0] += 26;
                            charID++;
                        }

                        while (titleCardText[charID] != 0) {
                            int character = titleCardText[charID];
                            if (character == ' ')
                                character = 0;
                            if (character == '-')
                                character = 0;
                            if (character >= '0' && character <= '9')
                                character -= 22;
                            if (character > '9' && character < 'f')
                                character -= 'A';

                            if (character <= -1) {
                                scriptEng.operands[1] += scriptEng.operands[5] + scriptEng.operands[6]; // spaceWidth + spacing
                            }
                            else {
                                character += scriptEng.operands[0];
                                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + character];
                                DrawSprite(scriptEng.operands[1] + spriteFrame->pivotX, scriptEng.operands[2] + spriteFrame->pivotY,
                                           spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                                scriptEng.operands[1] += spriteFrame->width + scriptEng.operands[6];
                            }
                            charID++;
                        }
                        break;
                }
                break;
            }
            case FUNC_DRAWMENU:
                opcodeSize        = 0;
                textMenuSurfaceNo = scriptInfo->spriteSheetID;
                DrawTextMenu(&gameMenu[scriptEng.operands[0]], scriptEng.operands[1], scriptEng.operands[2]);
                break;
            case FUNC_SPRITEFRAME:
                opcodeSize = 0;
                if (scriptEvent == EVENT_SETUP && scriptFrameCount < SPRITEFRAME_COUNT) {
                    scriptFrames[scriptFrameCount].pivotX = scriptEng.operands[0];
                    scriptFrames[scriptFrameCount].pivotY = scriptEng.operands[1];
                    scriptFrames[scriptFrameCount].width  = scriptEng.operands[2];
                    scriptFrames[scriptFrameCount].height = scriptEng.operands[3];
                    scriptFrames[scriptFrameCount].sprX   = scriptEng.operands[4];
                    scriptFrames[scriptFrameCount].sprY   = scriptEng.operands[5];
                    ++scriptFrameCount;
                }
                break;
            case FUNC_EDITFRAME: {
                opcodeSize  = 0;
                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + scriptEng.operands[0]];

                spriteFrame->pivotX = scriptEng.operands[1];
                spriteFrame->pivotY = scriptEng.operands[2];
                spriteFrame->width  = scriptEng.operands[3];
                spriteFrame->height = scriptEng.operands[4];
                spriteFrame->sprX   = scriptEng.operands[5];
                spriteFrame->sprY   = scriptEng.operands[6];
            } break;
            case FUNC_LOADPALETTE:
                opcodeSize = 0;
                LoadPalette(scriptText, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4]);
                break;
            case FUNC_ROTATEPALETTE:
                opcodeSize = 0;
                RotatePalette(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]);
                break;
            case FUNC_SETSCREENFADE:
                opcodeSize = 0;
                SetFade(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]);
                break;
            case FUNC_SETACTIVEPALETTE:
                opcodeSize = 0;
                SetActivePalette(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2]);
                break;
            case FUNC_SETPALETTEFADE:
#if RETRO_REV00
                SetLimitedFade(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                               scriptEng.operands[5], scriptEng.operands[6]);
#else
                SetPaletteFade(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                               scriptEng.operands[5]);
#endif
                break;
            case FUNC_SETPALETTEENTRY: SetPaletteEntryPacked(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2]); break;
            case FUNC_GETPALETTEENTRY: scriptEng.operands[2] = GetPaletteEntryPacked(scriptEng.operands[0], scriptEng.operands[1]); break;
            case FUNC_COPYPALETTE:
                opcodeSize = 0;
                CopyPalette(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4]);
                break;
            case FUNC_CLEARSCREEN:
                opcodeSize = 0;
                ClearScreen(scriptEng.operands[0]);
                break;
            case FUNC_DRAWSPRITEFX:
                opcodeSize  = 0;
                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + scriptEng.operands[0]];
                switch (scriptEng.operands[1]) {
                    default: break;
                    case FX_SCALE:
                        DrawSpriteScaled(entity->direction, (scriptEng.operands[2] >> 16) - xScrollOffset,
                                         (scriptEng.operands[3] >> 16) - yScrollOffset, -spriteFrame->pivotX, -spriteFrame->pivotY, entity->scale,
                                         entity->scale, spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY,
                                         scriptInfo->spriteSheetID);
                        break;
                    case FX_ROTATE:
                        DrawSpriteRotated(entity->direction, (scriptEng.operands[2] >> 16) - xScrollOffset,
                                          (scriptEng.operands[3] >> 16) - yScrollOffset, -spriteFrame->pivotX, -spriteFrame->pivotY,
                                          spriteFrame->sprX, spriteFrame->sprY, spriteFrame->width, spriteFrame->height, entity->rotation,
                                          scriptInfo->spriteSheetID);
                        break;
                    case FX_ROTOZOOM:
                        DrawSpriteRotozoom(entity->direction, (scriptEng.operands[2] >> 16) - xScrollOffset,
                                           (scriptEng.operands[3] >> 16) - yScrollOffset, -spriteFrame->pivotX, -spriteFrame->pivotY,
                                           spriteFrame->sprX, spriteFrame->sprY, spriteFrame->width, spriteFrame->height, entity->rotation,
                                           entity->scale, scriptInfo->spriteSheetID);
                        break;
                    case FX_INK:
                        switch (entity->inkEffect) {
                            case INK_NONE:
                                DrawSprite((scriptEng.operands[2] >> 16) - xScrollOffset + spriteFrame->pivotX,
                                           (scriptEng.operands[3] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width,
                                           spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                                break;
                            case INK_BLEND:
                                DrawBlendedSprite((scriptEng.operands[2] >> 16) - xScrollOffset + spriteFrame->pivotX,
                                                  (scriptEng.operands[3] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width,
                                                  spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                                break;
                            case INK_ALPHA:
                                DrawAlphaBlendedSprite((scriptEng.operands[2] >> 16) - xScrollOffset + spriteFrame->pivotX,
                                                       (scriptEng.operands[3] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width,
                                                       spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, entity->alpha,
                                                       scriptInfo->spriteSheetID);
                                break;
                            case INK_ADD:
                                DrawAdditiveBlendedSprite((scriptEng.operands[2] >> 16) - xScrollOffset + spriteFrame->pivotX,
                                                          (scriptEng.operands[3] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width,
                                                          spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, entity->alpha,
                                                          scriptInfo->spriteSheetID);
                                break;
                            case INK_SUB:
                                DrawSubtractiveBlendedSprite((scriptEng.operands[2] >> 16) - xScrollOffset + spriteFrame->pivotX,
                                                             (scriptEng.operands[3] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width,
                                                             spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, entity->alpha,
                                                             scriptInfo->spriteSheetID);
                                break;
                        }
                        break;
                    case FX_TINT:
                        if (entity->inkEffect == INK_ALPHA) {
                            DrawScaledTintMask(entity->direction, (scriptEng.operands[2] >> 16) - xScrollOffset,
                                               (scriptEng.operands[3] >> 16) - yScrollOffset, -spriteFrame->pivotX, -spriteFrame->pivotY,
                                               entity->scale, entity->scale, spriteFrame->width, spriteFrame->height, spriteFrame->sprX,
                                               spriteFrame->sprY, scriptInfo->spriteSheetID);
                        }
                        else {
                            DrawSpriteScaled(entity->direction, (scriptEng.operands[2] >> 16) - xScrollOffset,
                                             (scriptEng.operands[3] >> 16) - yScrollOffset, -spriteFrame->pivotX, -spriteFrame->pivotY, entity->scale,
                                             entity->scale, spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY,
                                             scriptInfo->spriteSheetID);
                        }
                        break;
                    case FX_FLIP:
                        switch (entity->direction) {
                            default:
                            case FLIP_NONE:
                                DrawSpriteFlipped((scriptEng.operands[2] >> 16) - xScrollOffset + spriteFrame->pivotX,
                                                  (scriptEng.operands[3] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width,
                                                  spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, FLIP_NONE, scriptInfo->spriteSheetID);
                                break;
                            case FLIP_X:
                                DrawSpriteFlipped((scriptEng.operands[2] >> 16) - xScrollOffset - spriteFrame->width - spriteFrame->pivotX,
                                                  (scriptEng.operands[3] >> 16) - yScrollOffset + spriteFrame->pivotY, spriteFrame->width,
                                                  spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, FLIP_X, scriptInfo->spriteSheetID);
                                break;
                            case FLIP_Y:
                                DrawSpriteFlipped((scriptEng.operands[2] >> 16) - xScrollOffset + spriteFrame->pivotX,
                                                  (scriptEng.operands[3] >> 16) - yScrollOffset - spriteFrame->height - spriteFrame->pivotY,
                                                  spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, FLIP_Y,
                                                  scriptInfo->spriteSheetID);
                                break;
                            case FLIP_XY:
                                DrawSpriteFlipped((scriptEng.operands[2] >> 16) - xScrollOffset - spriteFrame->width - spriteFrame->pivotX,
                                                  (scriptEng.operands[3] >> 16) - yScrollOffset - spriteFrame->height - spriteFrame->pivotY,
                                                  spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, FLIP_XY,
                                                  scriptInfo->spriteSheetID);
                                break;
                        }
                        break;
                }
                break;
            case FUNC_DRAWSPRITESCREENFX:
                opcodeSize  = 0;
                spriteFrame = &scriptFrames[scriptInfo->frameListOffset + scriptEng.operands[0]];
                switch (scriptEng.operands[1]) {
                    default: break;
                    case FX_SCALE:
                        DrawSpriteScaled(entity->direction, scriptEng.operands[2], scriptEng.operands[3], -spriteFrame->pivotX, -spriteFrame->pivotY,
                                         entity->scale, entity->scale, spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY,
                                         scriptInfo->spriteSheetID);
                        break;
                    case FX_ROTATE:
                        DrawSpriteRotated(entity->direction, scriptEng.operands[2], scriptEng.operands[3], -spriteFrame->pivotX, -spriteFrame->pivotY,
                                          spriteFrame->sprX, spriteFrame->sprY, spriteFrame->width, spriteFrame->height, entity->rotation,
                                          scriptInfo->spriteSheetID);
                        break;
                    case FX_ROTOZOOM:
                        DrawSpriteRotozoom(entity->direction, scriptEng.operands[2], scriptEng.operands[3], -spriteFrame->pivotX,
                                           -spriteFrame->pivotY, spriteFrame->sprX, spriteFrame->sprY, spriteFrame->width, spriteFrame->height,
                                           entity->rotation, entity->scale, scriptInfo->spriteSheetID);
                        break;
                    case FX_INK:
                        switch (entity->inkEffect) {
                            case INK_NONE:
                                DrawSprite(scriptEng.operands[2] + spriteFrame->pivotX, scriptEng.operands[3] + spriteFrame->pivotY,
                                           spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                                break;
                            case INK_BLEND:
                                DrawBlendedSprite(scriptEng.operands[2] + spriteFrame->pivotX, scriptEng.operands[3] + spriteFrame->pivotY,
                                                  spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY,
                                                  scriptInfo->spriteSheetID);
                                break;
                            case INK_ALPHA:
                                DrawAlphaBlendedSprite(scriptEng.operands[2] + spriteFrame->pivotX, scriptEng.operands[3] + spriteFrame->pivotY,
                                                       spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, entity->alpha,
                                                       scriptInfo->spriteSheetID);
                                break;
                            case INK_ADD:
                                DrawAdditiveBlendedSprite(scriptEng.operands[2] + spriteFrame->pivotX, scriptEng.operands[3] + spriteFrame->pivotY,
                                                          spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY,
                                                          entity->alpha, scriptInfo->spriteSheetID);
                                break;
                            case INK_SUB:
                                DrawSubtractiveBlendedSprite(scriptEng.operands[2] + spriteFrame->pivotX, scriptEng.operands[3] + spriteFrame->pivotY,
                                                             spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY,
                                                             entity->alpha, scriptInfo->spriteSheetID);
                                break;
                        }
                        break;
                    case FX_TINT:
                        if (entity->inkEffect == INK_ALPHA) {
                            DrawScaledTintMask(entity->direction, scriptEng.operands[2], scriptEng.operands[3], -spriteFrame->pivotX,
                                               -spriteFrame->pivotY, entity->scale, entity->scale, spriteFrame->width, spriteFrame->height,
                                               spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                        }
                        else {
                            DrawSpriteScaled(entity->direction, scriptEng.operands[2], scriptEng.operands[3], -spriteFrame->pivotX,
                                             -spriteFrame->pivotY, entity->scale, entity->scale, spriteFrame->width, spriteFrame->height,
                                             spriteFrame->sprX, spriteFrame->sprY, scriptInfo->spriteSheetID);
                        }
                        break;
                    case FX_FLIP:
                        switch (entity->direction) {
                            default:
                            case FLIP_NONE:
                                DrawSpriteFlipped(scriptEng.operands[2] + spriteFrame->pivotX, scriptEng.operands[3] + spriteFrame->pivotY,
                                                  spriteFrame->width, spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, FLIP_NONE,
                                                  scriptInfo->spriteSheetID);
                                break;
                            case FLIP_X:
                                DrawSpriteFlipped(scriptEng.operands[2] - spriteFrame->width - spriteFrame->pivotX,
                                                  scriptEng.operands[3] + spriteFrame->pivotY, spriteFrame->width, spriteFrame->height,
                                                  spriteFrame->sprX, spriteFrame->sprY, FLIP_X, scriptInfo->spriteSheetID);
                                break;
                            case FLIP_Y:
                                DrawSpriteFlipped(scriptEng.operands[2] + spriteFrame->pivotX,
                                                  scriptEng.operands[3] - spriteFrame->height - spriteFrame->pivotY, spriteFrame->width,
                                                  spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, FLIP_Y, scriptInfo->spriteSheetID);
                                break;
                            case FLIP_XY:
                                DrawSpriteFlipped(scriptEng.operands[2] - spriteFrame->width - spriteFrame->pivotX,
                                                  scriptEng.operands[3] - spriteFrame->height - spriteFrame->pivotY, spriteFrame->width,
                                                  spriteFrame->height, spriteFrame->sprX, spriteFrame->sprY, FLIP_XY, scriptInfo->spriteSheetID);
                                break;
                        }
                        break;
                }
                break;
            case FUNC_LOADANIMATION:
                opcodeSize           = 0;
                scriptInfo->animFile = AddAnimationFile(scriptText);
                break;
            case FUNC_SETUPMENU: {
                opcodeSize     = 0;
                TextMenu *menu = &gameMenu[scriptEng.operands[0]];
                SetupTextMenu(menu, scriptEng.operands[1]);
                menu->selectionCount = scriptEng.operands[2];
                menu->alignment      = scriptEng.operands[3];
                break;
            }
            case FUNC_ADDMENUENTRY: {
                opcodeSize                           = 0;
                TextMenu *menu                       = &gameMenu[scriptEng.operands[0]];
                menu->entryHighlight[menu->rowCount] = scriptEng.operands[2];
                AddTextMenuEntry(menu, scriptText);
                break;
            }
            case FUNC_EDITMENUENTRY: {
                opcodeSize     = 0;
                TextMenu *menu = &gameMenu[scriptEng.operands[0]];
                EditTextMenuEntry(menu, scriptText, scriptEng.operands[2]);
                menu->entryHighlight[scriptEng.operands[2]] = scriptEng.operands[3];
                break;
            }
            case FUNC_LOADSTAGE:
                opcodeSize = 0;
                stageMode  = STAGEMODE_LOAD;
                break;
            case FUNC_DRAWRECT:
                opcodeSize = 0;
                DrawRectangle(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                              scriptEng.operands[5], scriptEng.operands[6], scriptEng.operands[7]);
                break;
            case FUNC_RESETOBJECTENTITY: {
                opcodeSize     = 0;
                Entity *newEnt = &PS1_OBJ(scriptEng.operands[0]);
                memset(newEnt, 0, sizeof(Entity));
                newEnt->type               = scriptEng.operands[1];
                newEnt->propertyValue      = scriptEng.operands[2];
                newEnt->xpos               = scriptEng.operands[3];
                newEnt->ypos               = scriptEng.operands[4];
                newEnt->direction          = FLIP_NONE;
                newEnt->priority           = PRIORITY_BOUNDS;
                newEnt->drawOrder          = 3;
                newEnt->scale              = 512;
                newEnt->inkEffect          = INK_NONE;
                newEnt->objectInteractions = true;
                newEnt->visible            = true;
                newEnt->tileCollisions     = true;
                break;
            }
            case FUNC_BOXCOLLISIONTEST:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    default: break;
                    case C_TOUCH:
                        TouchCollision(&PS1_OBJ(scriptEng.operands[1]), scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                                       scriptEng.operands[5], &PS1_OBJ(scriptEng.operands[6]), scriptEng.operands[7], scriptEng.operands[8],
                                       scriptEng.operands[9], scriptEng.operands[10]);
                        break;
                    case C_SOLID:
                        BoxCollision(&PS1_OBJ(scriptEng.operands[1]), scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                                     scriptEng.operands[5], &PS1_OBJ(scriptEng.operands[6]), scriptEng.operands[7], scriptEng.operands[8],
                                     scriptEng.operands[9], scriptEng.operands[10]);
                        break;
                    case C_SOLID2:
                        BoxCollision2(&PS1_OBJ(scriptEng.operands[1]), scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                                      scriptEng.operands[5], &PS1_OBJ(scriptEng.operands[6]), scriptEng.operands[7], scriptEng.operands[8],
                                      scriptEng.operands[9], scriptEng.operands[10]);
                        break;
                    case C_PLATFORM:
                        PlatformCollision(&PS1_OBJ(scriptEng.operands[1]), scriptEng.operands[2], scriptEng.operands[3],
                                          scriptEng.operands[4], scriptEng.operands[5], &PS1_OBJ(scriptEng.operands[6]),
                                          scriptEng.operands[7], scriptEng.operands[8], scriptEng.operands[9], scriptEng.operands[10]);
                        break;
                }
                break;
            case FUNC_CREATETEMPOBJECT: {
                opcodeSize = 0;
                if (objectEntityList[scriptEng.arrayPosition[8]].type > OBJ_TYPE_BLANKOBJECT && ++scriptEng.arrayPosition[8] == ENTITY_COUNT)
                    scriptEng.arrayPosition[8] = TEMPENTITY_START;
                Entity *temp = &objectEntityList[scriptEng.arrayPosition[8]];
                memset(temp, 0, sizeof(Entity));
                temp->type               = scriptEng.operands[0];
                temp->propertyValue      = scriptEng.operands[1];
                temp->xpos               = scriptEng.operands[2];
                temp->ypos               = scriptEng.operands[3];
                temp->direction          = FLIP_NONE;
                temp->priority           = PRIORITY_ACTIVE;
                temp->drawOrder          = 3;
                temp->scale              = 512;
                temp->inkEffect          = INK_NONE;
                temp->objectInteractions = true;
                temp->visible            = true;
                temp->tileCollisions     = true;
                break;
            }
            case FUNC_PROCESSOBJECTMOVEMENT:
                opcodeSize = 0;
                if (entity->tileCollisions) {
                    ProcessTileCollisions(entity);
                }
                else {
                    entity->xpos += entity->xvel;
                    entity->ypos += entity->yvel;
                }
                break;
            case FUNC_PROCESSOBJECTCONTROL:
                opcodeSize = 0;
                ProcessObjectControl(entity);
                break;
            case FUNC_PROCESSANIMATION:
                opcodeSize = 0;
                ProcessObjectAnimation(scriptInfo, entity);
                break;
            case FUNC_DRAWOBJECTANIMATION:
                opcodeSize = 0;
                if (entity->visible)
                    DrawObjectAnimation(scriptInfo, entity, (entity->xpos >> 16) - xScrollOffset, (entity->ypos >> 16) - yScrollOffset);
                break;
            case FUNC_SETMUSICTRACK:
                opcodeSize = 0;
                if (scriptEng.operands[2] <= 1)
                    SetMusicTrack(scriptText, scriptEng.operands[1], scriptEng.operands[2], 0);
                else
                    SetMusicTrack(scriptText, scriptEng.operands[1], true, scriptEng.operands[2]);
                break;
            case FUNC_PLAYMUSIC:
                opcodeSize = 0;
                PlayMusic(scriptEng.operands[0], 0);
                break;
            case FUNC_STOPMUSIC:
                opcodeSize = 0;
                StopMusic(true);
                break;
            case FUNC_PAUSEMUSIC:
                opcodeSize = 0;
                PauseSound();
                break;
            case FUNC_RESUMEMUSIC:
                opcodeSize = 0;
                ResumeSound();
                break;
            case FUNC_SWAPMUSICTRACK:
                opcodeSize = 0;
                if (scriptEng.operands[2] <= 1)
                    SwapMusicTrack(scriptText, scriptEng.operands[1], 0, scriptEng.operands[3]);
                else
                    SwapMusicTrack(scriptText, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]);
                break;
            case FUNC_PLAYSFX:
                opcodeSize = 0;
                PlaySfx(scriptEng.operands[0], scriptEng.operands[1]);
                break;
            case FUNC_STOPSFX:
                opcodeSize = 0;
                StopSfx(scriptEng.operands[0]);
                break;
            case FUNC_SETSFXATTRIBUTES:
                opcodeSize = 0;
                SetSfxAttributes(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2]);
                break;
            case FUNC_OBJECTTILECOLLISION:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    default: break;
                    case CSIDE_FLOOR: ObjectFloorCollision(scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case CSIDE_LWALL: ObjectLWallCollision(scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case CSIDE_RWALL: ObjectRWallCollision(scriptEng.operands[1] - 1, scriptEng.operands[2], scriptEng.operands[3]); break;
                    case CSIDE_ROOF: ObjectRoofCollision(scriptEng.operands[1], scriptEng.operands[2] - 1, scriptEng.operands[3]); break;
#if RETRO_REV03
                    // Yes, the right side also calls for LWall
                    case CSIDE_LENTITY: ObjectLWallCollision(scriptEng.operands[2], 0, PS1_OBJ(scriptEng.operands[1]).collisionPlane); break;
                    case CSIDE_RENTITY: ObjectLWallCollision(scriptEng.operands[2] - 1, 0, PS1_OBJ(scriptEng.operands[1]).collisionPlane); break;
#endif
                }
                break;
            case FUNC_OBJECTTILEGRIP:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    default: break;
                    case CSIDE_FLOOR: ObjectFloorGrip(scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case CSIDE_LWALL: ObjectLWallGrip(scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case CSIDE_RWALL: ObjectRWallGrip(scriptEng.operands[1] - 1, scriptEng.operands[2], scriptEng.operands[3]); break;
                    case CSIDE_ROOF: ObjectRoofGrip(scriptEng.operands[1], scriptEng.operands[2] - 1, scriptEng.operands[3]); break;
#if RETRO_REV03
                    case CSIDE_LENTITY: ObjectLEntityGrip(scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case CSIDE_RENTITY: ObjectREntityGrip(scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
#endif
                }
                break;
            case FUNC_NOT: scriptEng.operands[0] = ~scriptEng.operands[0]; break;
            case FUNC_DRAW3DSCENE:
                opcodeSize = 0;
#if RETRO_PLATFORM == RETRO_PS1
            {
                uint32_t t0 = PS1Hblanks();
                TransformVertexBuffer();
                uint32_t t1 = PS1Hblanks();
                Sort3DDrawList();
                uint32_t t2 = PS1Hblanks();
                Draw3DScene(scriptInfo->spriteSheetID);
                g_ps1Prof3DTransformHbl = (uint16_t)(t1 - t0);
                g_ps1Prof3DSortHbl      = (uint16_t)(t2 - t1);
                g_ps1Prof3DDrawHbl      = PS1HblanksSince(t2);
            }
#else
                TransformVertexBuffer();
                Sort3DDrawList();
                Draw3DScene(scriptInfo->spriteSheetID);
#endif
                break;
            case FUNC_SETIDENTITYMATRIX:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: SetIdentityMatrix(&matWorld); break;
                    case MAT_VIEW: SetIdentityMatrix(&matView); break;
                    case MAT_TEMP: SetIdentityMatrix(&matTemp); break;
                }
                break;
            case FUNC_MATRIXMULTIPLY:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD:
                        switch (scriptEng.operands[1]) {
                            case MAT_WORLD: MatrixMultiply(&matWorld, &matWorld); break;
                            case MAT_VIEW: MatrixMultiply(&matWorld, &matView); break;
                            case MAT_TEMP: MatrixMultiply(&matWorld, &matTemp); break;
                        }
                        break;
                    case MAT_VIEW:
                        switch (scriptEng.operands[1]) {
                            case MAT_WORLD: MatrixMultiply(&matView, &matWorld); break;
                            case MAT_VIEW: MatrixMultiply(&matView, &matView); break;
                            case MAT_TEMP: MatrixMultiply(&matView, &matTemp); break;
                        }
                        break;
                    case MAT_TEMP:
                        switch (scriptEng.operands[1]) {
                            case MAT_WORLD: MatrixMultiply(&matTemp, &matWorld); break;
                            case MAT_VIEW: MatrixMultiply(&matTemp, &matView); break;
                            case MAT_TEMP: MatrixMultiply(&matTemp, &matTemp); break;
                        }
                        break;
                }
                break;
            case FUNC_MATRIXTRANSLATEXYZ:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: MatrixTranslateXYZ(&matWorld, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case MAT_VIEW: MatrixTranslateXYZ(&matView, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case MAT_TEMP: MatrixTranslateXYZ(&matTemp, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                }
                break;
            case FUNC_MATRIXSCALEXYZ:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: MatrixScaleXYZ(&matWorld, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case MAT_VIEW: MatrixScaleXYZ(&matView, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case MAT_TEMP: MatrixScaleXYZ(&matTemp, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                }
                break;
            case FUNC_MATRIXROTATEX:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: MatrixRotateX(&matWorld, scriptEng.operands[1]); break;
                    case MAT_VIEW: MatrixRotateX(&matView, scriptEng.operands[1]); break;
                    case MAT_TEMP: MatrixRotateX(&matTemp, scriptEng.operands[1]); break;
                }
                break;
            case FUNC_MATRIXROTATEY:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: MatrixRotateY(&matWorld, scriptEng.operands[1]); break;
                    case MAT_VIEW: MatrixRotateY(&matView, scriptEng.operands[1]); break;
                    case MAT_TEMP: MatrixRotateY(&matTemp, scriptEng.operands[1]); break;
                }
                break;
            case FUNC_MATRIXROTATEZ:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: MatrixRotateZ(&matWorld, scriptEng.operands[1]); break;
                    case MAT_VIEW: MatrixRotateZ(&matView, scriptEng.operands[1]); break;
                    case MAT_TEMP: MatrixRotateZ(&matTemp, scriptEng.operands[1]); break;
                }
                break;
            case FUNC_MATRIXROTATEXYZ:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: MatrixRotateXYZ(&matWorld, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case MAT_VIEW: MatrixRotateXYZ(&matView, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                    case MAT_TEMP: MatrixRotateXYZ(&matTemp, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]); break;
                }
                break;
#if !RETRO_REV00
            case FUNC_MATRIXINVERSE:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: MatrixInverse(&matWorld); break;
                    case MAT_VIEW: MatrixInverse(&matView); break;
                    case MAT_TEMP: MatrixInverse(&matTemp); break;
                }
                break;
#endif
            case FUNC_TRANSFORMVERTICES:
                opcodeSize = 0;
                switch (scriptEng.operands[0]) {
                    case MAT_WORLD: TransformVertices(&matWorld, PS1_VTX(scriptEng.operands[1]), PS1_VTX_COUNT(scriptEng.operands[2])); break;
                    case MAT_VIEW: TransformVertices(&matView, PS1_VTX(scriptEng.operands[1]), PS1_VTX_COUNT(scriptEng.operands[2])); break;
                    case MAT_TEMP: TransformVertices(&matTemp, PS1_VTX(scriptEng.operands[1]), PS1_VTX_COUNT(scriptEng.operands[2])); break;
                }
                break;
            case FUNC_CALLFUNCTION: {
                opcodeSize                        = 0;
                functionStack[functionStackPos++] = scriptCodePtr;
                functionStack[functionStackPos++] = jumpTableStart;
                functionStack[functionStackPos++] = scriptCodeStart;
                scriptCodeStart                   = scriptFunctionList[scriptEng.operands[0]].ptr.scriptCodePtr;
                jumpTableStart                    = scriptFunctionList[scriptEng.operands[0]].ptr.jumpTablePtr;
                scriptCodePtr                     = scriptCodeStart;
                break;
            }
            case FUNC_RETURN:
                opcodeSize = 0;
#if RETRO_PLATFORM == RETRO_PS1
                if (functionStackPos == ps1FuncBase) { // event (or a native's nested call), stop running
#else
                if (!functionStackPos) { // event, stop running
#endif
                    running = false;
                }
                else { // function, jump out
                    scriptCodeStart = functionStack[--functionStackPos];
                    jumpTableStart  = functionStack[--functionStackPos];
                    scriptCodePtr   = functionStack[--functionStackPos];
                }
                break;
            case FUNC_SETLAYERDEFORMATION:
                opcodeSize = 0;
                SetLayerDeformation(scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                                    scriptEng.operands[5]);
                break;
            case FUNC_CHECKTOUCHRECT: opcodeSize = 0; scriptEng.checkResult = -1;
#if !RETRO_USE_ORIGINAL_CODE
                AddDebugHitbox(H_TYPE_FINGER, NULL, scriptEng.operands[0], scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3]);
#endif
                for (int f = 0; f < touches; ++f) {
                    if (touchDown[f] && touchX[f] > scriptEng.operands[0] && touchX[f] < scriptEng.operands[2] && touchY[f] > scriptEng.operands[1]
                        && touchY[f] < scriptEng.operands[3]) {
                        scriptEng.checkResult = f;
                    }
                }
                break;
            case FUNC_GETTILELAYERENTRY:
                scriptEng.operands[0] = stageLayouts[scriptEng.operands[1]].tiles[scriptEng.operands[2] + 0x100 * scriptEng.operands[3]];
                break;
            case FUNC_SETTILELAYERENTRY:
                stageLayouts[scriptEng.operands[1]].tiles[scriptEng.operands[2] + 0x100 * scriptEng.operands[3]] = scriptEng.operands[0];
                break;
            case FUNC_GETBIT: scriptEng.operands[0] = (scriptEng.operands[1] & (1 << scriptEng.operands[2])) >> scriptEng.operands[2]; break;
            case FUNC_SETBIT:
                if (scriptEng.operands[2] <= 0)
                    scriptEng.operands[0] &= ~(1 << scriptEng.operands[1]);
                else
                    scriptEng.operands[0] |= 1 << scriptEng.operands[1];
                break;
            case FUNC_CLEARDRAWLIST:
                opcodeSize                                      = 0;
                drawListEntries[scriptEng.operands[0]].listSize = 0;
                break;
            case FUNC_ADDDRAWLISTENTITYREF: {
                opcodeSize                                                                                           = 0;
                drawListEntries[scriptEng.operands[0]].entityRefs[drawListEntries[scriptEng.operands[0]].listSize++] = scriptEng.operands[1];
                break;
            }
            case FUNC_GETDRAWLISTENTITYREF: scriptEng.operands[0] = drawListEntries[scriptEng.operands[1]].entityRefs[scriptEng.operands[2]]; break;
            case FUNC_SETDRAWLISTENTITYREF:
                opcodeSize                                                               = 0;
                drawListEntries[scriptEng.operands[1]].entityRefs[scriptEng.operands[2]] = scriptEng.operands[0];
                break;
            case FUNC_GET16X16TILEINFO: {
                scriptEng.operands[4] = scriptEng.operands[1] >> 7;
                scriptEng.operands[5] = scriptEng.operands[2] >> 7;
                scriptEng.operands[6] = stageLayouts[0].tiles[scriptEng.operands[4] + (scriptEng.operands[5] << 8)] << 6;
                scriptEng.operands[6] += ((scriptEng.operands[1] & 0x7F) >> 4) + 8 * ((scriptEng.operands[2] & 0x7F) >> 4);
                int index = tiles128x128.tileIndex[scriptEng.operands[6]];
                switch (scriptEng.operands[3]) {
                    case TILEINFO_INDEX: scriptEng.operands[0] = tiles128x128.tileIndex[scriptEng.operands[6]]; break;
                    case TILEINFO_DIRECTION: scriptEng.operands[0] = tiles128x128.direction[scriptEng.operands[6]]; break;
                    case TILEINFO_VISUALPLANE: scriptEng.operands[0] = tiles128x128.visualPlane[scriptEng.operands[6]]; break;
                    case TILEINFO_SOLIDITYA: scriptEng.operands[0] = tiles128x128.collisionFlags[0][scriptEng.operands[6]]; break;
                    case TILEINFO_SOLIDITYB: scriptEng.operands[0] = tiles128x128.collisionFlags[1][scriptEng.operands[6]]; break;
                    case TILEINFO_FLAGSA: scriptEng.operands[0] = collisionMasks[0].flags[index]; break;
                    case TILEINFO_ANGLEA: scriptEng.operands[0] = collisionMasks[0].angles[index]; break;
                    case TILEINFO_FLAGSB: scriptEng.operands[0] = collisionMasks[1].flags[index]; break;
                    case TILEINFO_ANGLEB: scriptEng.operands[0] = collisionMasks[1].angles[index]; break;
                    default: break;
                }
                break;
            }
            case FUNC_SET16X16TILEINFO: {
                scriptEng.operands[4] = scriptEng.operands[1] >> 7;
                scriptEng.operands[5] = scriptEng.operands[2] >> 7;
                scriptEng.operands[6] = stageLayouts[0].tiles[scriptEng.operands[4] + (scriptEng.operands[5] << 8)] << 6;
                scriptEng.operands[6] += ((scriptEng.operands[1] & 0x7F) >> 4) + 8 * ((scriptEng.operands[2] & 0x7F) >> 4);
                switch (scriptEng.operands[3]) {
                    case TILEINFO_INDEX:
                        tiles128x128.tileIndex[scriptEng.operands[6]]  = scriptEng.operands[0];
#if RETRO_PLATFORM != RETRO_PS1 // PS1: derived from tileIndex by the renderer
                        tiles128x128.gfxDataPos[scriptEng.operands[6]] = scriptEng.operands[0] << 8;
#endif
                        break;
                    case TILEINFO_DIRECTION: tiles128x128.direction[scriptEng.operands[6]] = scriptEng.operands[0]; break;
                    case TILEINFO_VISUALPLANE: tiles128x128.visualPlane[scriptEng.operands[6]] = scriptEng.operands[0]; break;
                    case TILEINFO_SOLIDITYA: tiles128x128.collisionFlags[0][scriptEng.operands[6]] = scriptEng.operands[0]; break;
                    case TILEINFO_SOLIDITYB: tiles128x128.collisionFlags[1][scriptEng.operands[6]] = scriptEng.operands[0]; break;
                    case TILEINFO_FLAGSA: collisionMasks[1].flags[tiles128x128.tileIndex[scriptEng.operands[6]]] = scriptEng.operands[0]; break;
                    case TILEINFO_ANGLEA: collisionMasks[1].angles[tiles128x128.tileIndex[scriptEng.operands[6]]] = scriptEng.operands[0]; break;
                    default: break;
                }
#if RETRO_PLATFORM == RETRO_PS1
                PS1TileChanged(scriptEng.operands[6]); // ps1/render.cpp: the chunk's visible tile rows
#endif
                break;
            }
            case FUNC_COPY16X16TILE:
                opcodeSize = 0;
                Copy16x16Tile(scriptEng.operands[0], scriptEng.operands[1]);
                break;
            case FUNC_GETANIMATIONBYNAME: {
                AnimationFile *animFile = scriptInfo->animFile;
                scriptEng.operands[0]   = -1;
                int id                  = 0;
                while (scriptEng.operands[0] == -1) {
                    SpriteAnimation *anim = &animationList[animFile->aniListOffset + id];
                    if (StrComp(scriptText, anim->name))
                        scriptEng.operands[0] = id;
                    else if (++id == animFile->animCount)
                        scriptEng.operands[0] = 0;
                }
                break;
            }
            case FUNC_READSAVERAM:
                opcodeSize            = 0;
                scriptEng.checkResult = ReadSaveRAMData();
                break;
            case FUNC_WRITESAVERAM:
                opcodeSize            = 0;
                scriptEng.checkResult = WriteSaveRAMData();
                break;
#if !RETRO_REV02
            case FUNC_LOADTEXTFONT: {
                opcodeSize = 0;
                LoadFontFile(scriptText);
                break;
            }
#endif
            case FUNC_LOADTEXTFILE: {
                opcodeSize     = 0;
                TextMenu *menu = &gameMenu[scriptEng.operands[0]];
#if !RETRO_REV02
                LoadTextFile(menu, scriptText, scriptEng.operands[2] != 0);
#else
                LoadTextFile(menu, scriptText, false);
#endif
                break;
            }
            case FUNC_GETTEXTINFO: {
                TextMenu *menu = &gameMenu[scriptEng.operands[1]];
                switch (scriptEng.operands[2]) {
                    case TEXTINFO_TEXTDATA:
                        scriptEng.operands[0] = menu->textData[menu->entryStart[scriptEng.operands[3]] + scriptEng.operands[4]];
                        break;
                    case TEXTINFO_TEXTSIZE: scriptEng.operands[0] = menu->entrySize[scriptEng.operands[3]]; break;
                    case TEXTINFO_ROWCOUNT: scriptEng.operands[0] = menu->rowCount; break;
                }
                break;
            }
#if !RETRO_REV02
            case FUNC_DRAWTEXT: {
                opcodeSize        = 0;
                textMenuSurfaceNo = scriptInfo->spriteSheetID;
                TextMenu *menu    = &gameMenu[scriptEng.operands[0]];
                DrawBitmapText(menu, scriptEng.operands[1], scriptEng.operands[2], scriptEng.operands[3], scriptEng.operands[4],
                               scriptEng.operands[5], scriptEng.operands[6]);
                break;
            }
#endif
            case FUNC_GETVERSIONNUMBER: {
                opcodeSize                           = 0;
                TextMenu *menu                       = &gameMenu[scriptEng.operands[0]];
                menu->entryHighlight[menu->rowCount] = scriptEng.operands[1];
                AddTextMenuEntry(menu, Engine.gameVersion);
                break;
            }
#if RETRO_PLATFORM == RETRO_PS1
#if PS1_GAME == 2
            // Sonic 2's natives (docs/30): only its build compiles them (the functions they call are static, so the
            // Sonic 1 build leaves them out entirely); the oscillators and the Bridge draw below serve both games.
            case FUNC_PS1RING:
                opcodeSize = 0;
                PS1RingUpdate();
                break;
            case FUNC_PS1LOSERING:
                opcodeSize = 0;
                PS1LoseRingUpdate();
                break;
            case FUNC_PS1BUTTONBRIDGE:
                opcodeSize = 0;
                PS1ButtonBridgeUpdate();
                break;
            case FUNC_PS1PLANESWITCHV:
                opcodeSize = 0;
                PS1PlaneSwitchUpdate(true);
                break;
            case FUNC_PS1PLANESWITCHH:
                opcodeSize = 0;
                PS1PlaneSwitchUpdate(false);
                break;
            case FUNC_PS1ROTATEPLATFORM:
                opcodeSize = 0;
                PS1RotatePlatformUpdate();
                break;
            case FUNC_PS1ROTATEPLATFORMDRAW:
                opcodeSize = 0;
                PS1RotatePlatformDraw();
                break;
            case FUNC_PS1HPZBRIDGE:
                opcodeSize = 0;
                PS1HPZBridgeUpdate();
                break;
            case FUNC_PS1HPZBRIDGEDRAW:
                opcodeSize = 0;
                PS1HPZBridgeDraw();
                break;
            case FUNC_PS1STAGESETUP:
                opcodeSize = 0;
                PS1StageSetupUpdate();
                break;
            case FUNC_PS1HUDDRAW:
                opcodeSize = 0;
                PS1HUDDraw();
                break;
            case FUNC_PS1TURRETPLATFORM:
                opcodeSize = 0;
                PS1TurretPlatformUpdate();
                break;
            case FUNC_PS1BELTPLATFORM:
                opcodeSize = 0;
                PS1BeltPlatformUpdate();
                break;
            case FUNC_PS1HFLIPPER:
                opcodeSize = 0;
                PS1HFlipperUpdate();
                break;
            case FUNC_PS1EARTHQUAKE:
                opcodeSize = 0;
                PS1EarthquakeUpdate();
                break;
            case FUNC_PS1SPIKES:
                opcodeSize = 0;
                PS1SpikesUpdate();
                break;
            case FUNC_PS1PLAYERINPUT:
                opcodeSize = 0;
                PS1PlayerInput();
                break;
            case FUNC_PS1CLEDGE:
                opcodeSize = 0;
                PS1CLedgeUpdate();
                break;
            case FUNC_PS1STEAMPISTON:
                opcodeSize = 0;
                PS1SteamPistonUpdate();
                break;
            case FUNC_PS1PLAYERFN2:
                opcodeSize = 0;
                PS1PlayerFn2();
                break;
            case FUNC_PS1PLAYERFN3:
                opcodeSize = 0;
                PS1PlayerFn3();
                break;
            case FUNC_PS1PLAYERFN4:
                opcodeSize = 0;
                PS1PlayerFn4();
                break;
            case FUNC_PS1PLAYERFN5:
                opcodeSize = 0;
                PS1PlayerFn5();
                break;
            case FUNC_PS1PLAYERFN6:
                opcodeSize = 0;
                PS1PlayerFn6();
                break;
            case FUNC_PS1PLAYERFN51:
                opcodeSize = 0;
                PS1PlayerFn51();
                break;
            case FUNC_PS1PLAYERFN52:
                opcodeSize = 0;
                PS1PlayerFn52();
                break;
            case FUNC_PS1PLAYERFN53:
                opcodeSize = 0;
                PS1PlayerFn53();
                break;
            case FUNC_PS1PLAYERSTATE10:
                opcodeSize = 0;
                PS1PlayerState10();
                break;
            case FUNC_PS1PLAYERSTATE12:
                opcodeSize = 0;
                PS1PlayerState12();
                break;
            case FUNC_PS1TAILSFN61:
                opcodeSize = 0;
                PS1TailsFn61();
                break;
            case FUNC_PS1TAILSFN62:
                opcodeSize = 0;
                PS1TailsFn62();
                break;
            case FUNC_PS1TAILSFN66:
                opcodeSize = 0;
                PS1TailsFn66();
                break;
            case FUNC_PS1TAILSFN67:
                opcodeSize = 0;
                PS1TailsFn67();
                break;
            case FUNC_PS1MPZSETUP:
                opcodeSize = 0;
                PS1MPZSetupUpdate();
                break;
            case FUNC_PS1MONITOR:
                opcodeSize = 0;
                PS1MonitorUpdate();
                break;
            case FUNC_PS1INVISIBLEBLOCK:
                opcodeSize = 0;
                PS1InvisibleBlockUpdate();
                break;
            case FUNC_PS1SPECIALRING:
                opcodeSize = 0;
                PS1SpecialRingUpdate();
                break;
            case FUNC_PS1HALFPIPE:
                opcodeSize = 0;
                PS1HalfpipeUpdate();
                break;
            case FUNC_PS1HALFPIPESEGMENT: // a function body: the `return` after it ends the call
                opcodeSize = 0;
                PS1HalfpipeSegment();
                break;
            case FUNC_PS1PLAYERFACES: // a function body
                opcodeSize = 0;
                PS1PlayerFaces();
                break;
            case FUNC_PS1SPECIALPLAYERRUN: // a function body
                opcodeSize = 0;
                PS1SpecialPlayerRun();
                break;
            case FUNC_PS1SPECIALSETUPSORT: // the tail of a sub: End follows
                opcodeSize = 0;
                PS1SpecialSetupSort();
                break;
            case FUNC_PS1SPECIALSETUPUPDATE:
                opcodeSize = 0;
                PS1SpecialSetupUpdate();
                break;
            case FUNC_PS1HORIZONTALDOOR: {
                // Metropolis's Horizontal Door update sub (stage object "Horizontal Door", up to 8 on screen), natively:
                // one statement per script instruction, in order; patched in only when it matches HDOOR_SIG exactly.
                opcodeSize             = 0;
                int *t                 = scriptEng.temp, *ap = scriptEng.arrayPosition;
                int &cr                = scriptEng.checkResult;
                int self               = objectEntityPos;
                Entity &me             = PS1_OBJ(self);
                TypeGroupList &players = objectTypeGroupList[256];
                t[0]                   = 0;
                t[1]                   = me.xpos;
                me.xpos                = me.values[1];
                for (int loop = 0; loop < players.listSize; ++loop) {
                    ap[6] = players.entityRefs[loop];
                    if (me.direction == 0) {
                        TouchCollision(&PS1_OBJ(self), -32, -16, 96, 64, &PS1_OBJ(ap[6]), -1, -1, 1, 1);
                        if (cr == 1)
                            t[0] = 1;
                    }
                    else {
                        TouchCollision(&PS1_OBJ(self), -160, -16, -32, 64, &PS1_OBJ(ap[6]), -1, -1, 1, 1);
                        if (cr == 1)
                            t[0] = 1;
                    }
                }
                me.xpos = t[1];
                if (t[0] == 1) {
                    if (me.values[0] < 4) {
                        me.values[0]++;
                        if (me.direction == 0)
                            me.xpos -= 1048576;
                        else
                            me.xpos += 1048576;
                    }
                }
                else {
                    if (me.values[0] > 0) {
                        me.values[0]--;
                        if (me.direction == 0)
                            me.xpos += 1048576;
                        else
                            me.xpos -= 1048576;
                    }
                }
                for (int loop = 0; loop < players.listSize; ++loop) {
                    ap[6] = players.entityRefs[loop];
                    if (PS1_OBJ(ap[6]).yvel >= 0)
                        BoxCollision(&PS1_OBJ(self), -32, -12, 32, 12, &PS1_OBJ(ap[6]), 65536, 65536, 65536, 65536);
                }
                break;
            }
#else
            // Sonic 1 (docs/37 phase 2b): the natives it shares with Sonic 2, with its own numbers (PS1_ANI_JUMPING,
            // PS1_G_SCORE...)
            case FUNC_PS1HUDDRAW:
                opcodeSize = 0;
                PS1HUDDraw();
                break;
            case FUNC_PS1PLAYERINPUT: // Sonic 1's own input function (PLAYERINPUT_FN_SIG_S1)
                opcodeSize = 0;
                PS1PlayerInputS1();
                break;
            case FUNC_PS1SSROTPOS: // Sonic 1's special stage function 9 (SSROTPOS_FN_SIG_S1)
                opcodeSize = 0;
                PS1SSRotPos();
                break;
            case FUNC_PS1SSBLOCKCOLLIDE: // Sonic 1's special stage function 10 (SSBLOCKCOLLIDE_FN_SIG_S1)
                opcodeSize = 0;
                PS1SSBlockCollide();
                break;
            case FUNC_PS1SSPLACEDRAW: // the special stage's plain draw subs (SSPLACEDRAW_SIG_S1): the frame's source
                opcodeSize = 0;
                PS1SSPlaceDraw(scriptEng.operands[0]);
                break;
            case FUNC_PS1SSANIMDRAW: // the special stage's animated draw subs (SSANIMDRAW_SIGS_S1): which one
                opcodeSize = 0;
                PS1SSAnimDraw(scriptEng.operands[0]);
                break;
            case FUNC_PS1S1ZONEOBJ: // Sonic 1's zone objects' update subs (S1ZONEOBJ_SIGS): which one
                opcodeSize = 0;
                PS1S1ZoneObj(scriptEng.operands[0]);
                break;
            case FUNC_PS1SSOBJUPDATE: // the special stage's other update subs (SSOBJUPDATE_SIGS_S1): which one
                opcodeSize = 0;
                PS1SSObjUpdate(scriptEng.operands[0]);
                break;
            case FUNC_PS1SSGEMDRAW: // Gem Block's draw sub (SSGEMDRAW_SIG_S1)
                opcodeSize = 0;
                PS1SSGemDraw();
                break;
            case FUNC_PS1SSRING: // the special stage's Ring update sub (SSRING_SIG_S1)
                opcodeSize = 0;
                PS1SSRingUpdate();
                break;
            case FUNC_PS1SSBLOCKUPDATE: // the special stage's coloured blocks' update sub (SSBLOCKUPDATE_SIG_S1)
                opcodeSize = 0;
                PS1SSBlockUpdate();
                break;
            case FUNC_PS1SSBLOCKDRAW: // the special stage's coloured blocks' draw sub (SSBLOCKDRAW_SIG_S1): the two types
                opcodeSize = 0;
                PS1SSBlockDraw(scriptEng.operands[0], scriptEng.operands[1]);
                break;
            case FUNC_PS1STAGESETUP: // Sonic 1's own Stage Setup update (STAGESETUP_SIG_S1)
                opcodeSize = 0;
                PS1StageSetupUpdateS1();
                break;
            case FUNC_PS1RING: // Sonic 1's own Ring update (RING_SIG_S1)
                opcodeSize = 0;
                PS1RingUpdateS1();
                break;
            case FUNC_PS1MONITOR: // Sonic 1's own Monitor update (MONITOR_SIG_S1)
                opcodeSize = 0;
                PS1MonitorUpdateS1();
                break;
            case FUNC_PS1S1SPRING: // Sonic 1's Red / Yellow Spring update subs (SPRING_SIGS_S1): which one
                opcodeSize = 0;
                PS1S1Spring(scriptEng.operands[0]);
                break;
            case FUNC_PS1S1LZSETUP: // Labyrinth's LZ Setup subs (LZSETUP_SIGS_S1): 0 update, 1 draw
                opcodeSize = 0;
                PS1S1LZSetup(scriptEng.operands[0]);
                break;
            case FUNC_PS1TAILSFN61:
                opcodeSize = 0;
                PS1TailsFn61();
                break;
            case FUNC_PS1TAILSFN62:
                opcodeSize = 0;
                PS1TailsFn62();
                break;
            case FUNC_PS1TAILSFN66:
                opcodeSize = 0;
                PS1TailsFn66();
                break;
            case FUNC_PS1TAILSFN67:
                opcodeSize = 0;
                PS1TailsFn67();
                break;
            case FUNC_PS1PLAYERFN2:
                opcodeSize = 0;
                PS1PlayerFn2();
                break;
            case FUNC_PS1PLAYERFN3:
                opcodeSize = 0;
                PS1PlayerFn3();
                break;
            case FUNC_PS1PLAYERFN4:
                opcodeSize = 0;
                PS1PlayerFn4();
                break;
            case FUNC_PS1PLAYERFN5:
                opcodeSize = 0;
                PS1PlayerFn5();
                break;
            case FUNC_PS1PLAYERFN6:
                opcodeSize = 0;
                PS1PlayerFn6();
                break;
            case FUNC_PS1PLAYERFN51:
                opcodeSize = 0;
                PS1PlayerFn51();
                break;
            case FUNC_PS1PLAYERFN52:
                opcodeSize = 0;
                PS1PlayerFn52();
                break;
            case FUNC_PS1PLAYERFN53:
                opcodeSize = 0;
                PS1PlayerFn53();
                break;
            // another game's native opcode (tools/scripts/patch_bytecode.py patches each game's own): stop the sub,
            // counted like a bad opcode
            case FUNC_PS1LOSERING: case FUNC_PS1BUTTONBRIDGE: case FUNC_PS1PLANESWITCHV:
            case FUNC_PS1PLANESWITCHH: case FUNC_PS1ROTATEPLATFORM: case FUNC_PS1ROTATEPLATFORMDRAW: case FUNC_PS1HPZBRIDGE:
            case FUNC_PS1HPZBRIDGEDRAW: case FUNC_PS1TURRETPLATFORM:
            case FUNC_PS1BELTPLATFORM: case FUNC_PS1HFLIPPER: case FUNC_PS1EARTHQUAKE: case FUNC_PS1SPIKES:
            case FUNC_PS1CLEDGE: case FUNC_PS1STEAMPISTON: case FUNC_PS1PLAYERSTATE10:
            case FUNC_PS1PLAYERSTATE12: case FUNC_PS1MPZSETUP: case FUNC_PS1INVISIBLEBLOCK:
            case FUNC_PS1SPECIALRING: case FUNC_PS1HALFPIPE: case FUNC_PS1HALFPIPESEGMENT: case FUNC_PS1PLAYERFACES:
            case FUNC_PS1SPECIALPLAYERRUN: case FUNC_PS1SPECIALSETUPSORT: case FUNC_PS1SPECIALSETUPUPDATE:
            case FUNC_PS1HORIZONTALDOOR:
                opcodeSize           = 0;
                g_ps1ScriptBadOpcode = g_ps1ScriptBadOpcode + 1;
                running              = false;
                break;
#endif
            case FUNC_PS1BRIDGEDRAW: {
                // Emerald Hill's Bridge draw sub (stage object "Bridge"), natively: the logs of a sagging bridge, a
                // sine curve on each side of the log the player stands on (~170 VM instructions a frame, 45 hblanks).
                // Each statement is one script instruction, in order (t = temps; v = this entity's values), so the
                // temps end as the script leaves them; `draw` is FUNC_DRAWSPRITEXY with frame 0, `div` FUNC_DIV with
                // the PS1 zero guard. Patched in only when the sub matches BRIDGE_SIG (tools/scripts/patch_bytecode.py).
                opcodeSize      = 0;
                int *t          = scriptEng.temp;
                Entity &self    = objectEntityList[objectEntityPos];
                int *v          = self.values;
                ObjectScript *o = scriptInfo;
                auto draw       = [o](int x, int y) {
                    SpriteFrame *f = &scriptFrames[o->frameListOffset + 0];
                    DrawSprite((x >> 16) - xScrollOffset + f->pivotX, (y >> 16) - yScrollOffset + f->pivotY, f->width, f->height, f->sprX,
                               f->sprY, o->spriteSheetID);
                };
                auto div = [](int a, int b) {
                    if (!b) {
                        g_ps1ScriptDivZero = g_ps1ScriptDivZero + 1;
                        return 0;
                    }
                    return a / b;
                };
                t[0] = 0;
                t[1] = v[6];
                t[1] += 524288;
                t[4] = 524288;
                t[5] = v[2];
                t[5] >>= 20;
                while (t[0] < t[5]) {
                    t[3] = t[4];
                    t[3] <<= 7;
                    t[3] = div(t[3], v[2]);
                    t[2] = Sin512(t[3]);
                    t[2] *= v[4];
                    t[2] >>= 9;
                    t[2] += self.ypos;
                    draw(t[1], t[2]);
                    t[1] += 1048576;
                    t[4] += 1048576;
                    t[0]++;
                }
                t[2] = v[4];
                t[2] += self.ypos;
                draw(t[1], t[2]);
                t[1] += 1048576;
                t[0]++;
                t[5] = v[7];
                t[5] -= v[6];
                t[5] -= v[2];
                t[1] = v[7];
                t[1] -= 524288;
                t[4] = 524288;
                while (t[0] < self.propertyValue) {
                    t[3] = t[4];
                    t[3] <<= 7;
                    t[3] = div(t[3], t[5]);
                    t[2] = Sin512(t[3]);
                    t[2] *= v[4];
                    t[2] >>= 9;
                    t[2] += self.ypos;
                    draw(t[1], t[2]);
                    t[1] -= 1048576;
                    t[4] += 1048576;
                    t[0]++;
                }
                break;
            }
            case FUNC_PS1OSCILLATE: {
                // Sonic 2's Stage Setup oscillators (GlobalCode function 71, ~390 VM instructions a frame): for
                // each of `count` oscillators, a velocity / position pair in `state` (a script table) moves towards
                // a limit from `params` and flips direction (a bit of the local `bits`). The same loop natively:
                //   t0 = t1 = 0; while (t0 < n) { t4 = P[t1]; t6 = S[t1]; ++t1; t5 = P[t1]; t7 = S[t1]; --t1;
                //   t2 = bit t0 of L; if (!t2) { t7 += t4; t6 += t7; if (t6 >= t5) set bit } else { t7 -= t4;
                //   t6 += t7; if (t6 < t5) clear bit } S[t1++] = t6; S[t1++] = t7; ++t0 }
                // with GetTableValue / SetTableValue's bounds (an index past the table leaves / writes nothing).
                opcodeSize = 0;
                int state = scriptEng.operands[0], params = scriptEng.operands[1], bits = scriptEng.operands[2], n = scriptEng.operands[3];
                int *t     = scriptEng.temp;
                auto get   = [](int table, int index, int &dst) {
                    if (index >= 0 && index < scriptCode[table])
                        dst = scriptCode[table + index + 1];
                };
                auto set = [](int table, int index, int value) {
                    if (index >= 0 && index < scriptCode[table])
                        PS1ScriptWrite(table + index + 1, value);
                };
                t[0] = 0;
                t[1] = 0;
                while (t[0] < n) {
                    get(params, t[1], t[4]);
                    get(state, t[1], t[6]);
                    ++t[1];
                    get(params, t[1], t[5]);
                    get(state, t[1], t[7]);
                    --t[1];
                    int l = scriptCode[bits];
                    t[2]  = (l & (1 << t[0])) >> t[0];
                    if (t[2] == 0) {
                        t[7] += t[4];
                        t[6] += t[7];
                        if (t[6] >= t[5])
                            PS1ScriptWrite(bits, l | (1 << t[0]));
                    }
                    else {
                        t[7] -= t[4];
                        t[6] += t[7];
                        if (t[6] < t[5])
                            PS1ScriptWrite(bits, l & ~(1 << t[0]));
                    }
                    set(state, t[1], t[6]);
                    ++t[1];
                    set(state, t[1], t[7]);
                    ++t[1];
                    ++t[0];
                }
                break;
            }
#endif
            case FUNC_GETTABLEVALUE: {
                int arrPos = scriptEng.operands[1];
                if (arrPos >= 0) {
                    int pos     = scriptEng.operands[2];
                    int arrSize = scriptCode[pos];
                    if (arrPos < arrSize)
                        scriptEng.operands[0] = scriptCode[pos + arrPos + 1];
                }
                break;
            }
            case FUNC_SETTABLEVALUE: {
                opcodeSize = 0;
                int arrPos = scriptEng.operands[1];
                if (arrPos >= 0) {
                    int pos     = scriptEng.operands[2];
                    int arrSize = scriptCode[pos];
                    if (arrPos < arrSize)
#if RETRO_PLATFORM == RETRO_PS1
                        PS1ScriptWrite(pos + arrPos + 1, scriptEng.operands[0]);
#else
                        scriptCode[pos + arrPos + 1] = scriptEng.operands[0];
#endif
                }
                break;
            }
            case FUNC_CHECKCURRENTSTAGEFOLDER:
                opcodeSize            = 0;
                scriptEng.checkResult = StrComp(stageList[activeStageList][stageListPosition].folder, scriptText);
#if RETRO_REV03
                // Mission Mode stuff
                if (!scriptEng.checkResult) {
                    int targetLength  = strlen(stageList[activeStageList][stageListPosition].folder);
                    int currentLength = strlen(scriptText);
                    if (targetLength > currentLength) {
                        scriptEng.checkResult =
                            StrComp(&stageList[activeStageList][stageListPosition].folder[targetLength - currentLength], scriptText);
                    }
                }
#endif
                break;
            case FUNC_ABS: {
                scriptEng.operands[0] = abs(scriptEng.operands[0]);
                break;
            }
            case FUNC_CALLNATIVEFUNCTION:
                opcodeSize = 0;
                if (scriptEng.operands[0] >= 0 && scriptEng.operands[0] < NATIIVEFUNCTION_COUNT) {
                    void (*func)(void) = (void (*)(void))nativeFunction[scriptEng.operands[0]];
                    if (func)
                        func();
                }
                break;
            case FUNC_CALLNATIVEFUNCTION2:
                if (scriptEng.operands[0] >= 0 && scriptEng.operands[0] < NATIIVEFUNCTION_COUNT) {
                    if (StrLength(scriptText)) {
                        void (*func)(int *, char *) = (void (*)(int *, char *))nativeFunction[scriptEng.operands[0]];
                        if (func)
                            func(&scriptEng.operands[2], scriptText);
                    }
                    else {
                        void (*func)(int *, int *) = (void (*)(int *, int *))nativeFunction[scriptEng.operands[0]];
                        if (func)
                            func(&scriptEng.operands[1], &scriptEng.operands[2]);
                    }
                }
                break;
            case FUNC_CALLNATIVEFUNCTION4:
                if (scriptEng.operands[0] >= 0 && scriptEng.operands[0] < NATIIVEFUNCTION_COUNT) {
                    if (StrLength(scriptText)) {
                        void (*func)(int *, char *, int *, int *) = (void (*)(int *, char *, int *, int *))nativeFunction[scriptEng.operands[0]];
                        if (func)
                            func(&scriptEng.operands[1], scriptText, &scriptEng.operands[3], &scriptEng.operands[4]);
                    }
                    else {
                        void (*func)(int *, int *, int *, int *) = (void (*)(int *, int *, int *, int *))nativeFunction[scriptEng.operands[0]];
                        if (func)
                            func(&scriptEng.operands[1], &scriptEng.operands[2], &scriptEng.operands[3], &scriptEng.operands[4]);
                    }
                }
                break;
            case FUNC_SETOBJECTRANGE: {
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = range

                opcodeSize       = 0;
                int offset       = (scriptEng.operands[0] >> 1) - SCREEN_CENTERX;
                OBJECT_BORDER_X1 = offset + 0x80;
                OBJECT_BORDER_X2 = scriptEng.operands[0] + 0x80 - offset;
                OBJECT_BORDER_X3 = offset + 0x20;
                OBJECT_BORDER_X4 = scriptEng.operands[0] + 0x20 - offset;
                break;
            }
#if RETRO_REV02
            case FUNC_GETOBJECTVALUE: {
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = result
                // scriptEng.operands[1] = valueID
                // scriptEng.operands[2] = entitySlot

                if (scriptEng.operands[1] < 48)
                    scriptEng.operands[0] = PS1_OBJ(scriptEng.operands[2]).values[scriptEng.operands[1]];
                break;
            }
            case FUNC_SETOBJECTVALUE: {
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = value
                // scriptEng.operands[1] = valueID
                // scriptEng.operands[2] = entitySlot

                opcodeSize = 0;
                if (scriptEng.operands[1] < 48)
                    PS1_OBJ(scriptEng.operands[2]).values[scriptEng.operands[1]] = scriptEng.operands[0];
                break;
            }
            case FUNC_COPYOBJECT: {
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = destSlot
                // scriptEng.operands[1] = srcSlot
                // scriptEng.operands[2] = count

                Entity *dstList = &PS1_OBJ(scriptEng.operands[0]);
                Entity *srcList = &PS1_OBJ(scriptEng.operands[1]);
                for (int i = 0; i < scriptEng.operands[2]; ++i) memcpy(&dstList[i], &srcList[i], sizeof(Entity));
                break;
            }
#endif
            case FUNC_PRINT: {
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = message (can be a regular value or a string depending on scriptEng.operands[1])
                // scriptEng.operands[1] = isInt
                // scriptEng.operands[2] = useEndLine

                endLine = false;
                if (scriptEng.operands[1])
                    PrintLog("%d", scriptEng.operands[0]);
                else
                    PrintLog("%s", scriptText);

                if (scriptEng.operands[2])
                    PrintLog("\n");
                endLine = true;
                break;
            }

#if RETRO_REV03
                // Extras for origins 2PVS,
                // most of these aren't (and won't be) implemented here because they rely on v5 tech that isn't part of the scope of this project
            case FUNC_CHECKCAMERAPROXIMITY:
                scriptEng.checkResult = false;

                // FUNCTION PARAMS:
                // scriptEng.operands[0] = pos.x
                // scriptEng.operands[1] = pos.y
                // scriptEng.operands[2] = range.x
                // scriptEng.operands[3] = range.y
                //
                // FUNCTION NOTES:
                // - Sets scriptEng.checkResult

                if (scriptEng.operands[2] > 0 && scriptEng.operands[3] > 0) {
                    int sx = abs(scriptEng.operands[0] - cameraXPos);
                    int sy = abs(scriptEng.operands[1] - cameraYPos);

                    if (sx < scriptEng.operands[2] && sy < scriptEng.operands[3]) {
                        scriptEng.checkResult = true;
                        break;
                    }
                }
                else {
                    if (scriptEng.operands[2] > 0) {
                        int sx = abs(scriptEng.operands[0] - cameraXPos);

                        if (sx < scriptEng.operands[2]) {
                            scriptEng.checkResult = true;
                            break;
                        }
                    }
                    else if (scriptEng.operands[3] > 0) {
                        int sy = abs(scriptEng.operands[1] - cameraYPos);

                        if (sy < scriptEng.operands[3]) {
                            scriptEng.checkResult = true;
                            break;
                        }
                    }
                }
                break;

            case FUNC_SETSCREENCOUNT:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = screenCount

                break;

            case FUNC_SETSCREENVERTICES:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = startVert2P_S1
                // scriptEng.operands[1] = startVert2P_S2
                // scriptEng.operands[2] = startVert3P_S1
                // scriptEng.operands[3] = startVert3P_S2
                // scriptEng.operands[4] = startVert3P_S3

                break;

            case FUNC_GETINPUTDEVICEID:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = deviceID
                // scriptEng.operands[1] = inputSlot
                //
                // FUNCTION NOTES:
                // - Assigns the device's id to scriptEng.operands[0]

                break;

            case FUNC_GETFILTEREDINPUTDEVICEID:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = deviceID
                // scriptEng.operands[1] = confirmOnly
                // scriptEng.operands[2] = unassignedOnly
                // scriptEng.operands[3] = maxInactiveTimer
                //
                // FUNCTION NOTES:
                // - Assigns the filtered device's id to scriptEng.operands[0]

                break;

            case FUNC_GETINPUTDEVICETYPE:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = deviceType
                // scriptEng.operands[1] = deviceID
                //
                // FUNCTION NOTES:
                // - Assigns the device's type to scriptEng.operands[0]

                break;

            case FUNC_ISINPUTDEVICEASSIGNED:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = deviceID

                break;

            case FUNC_ASSIGNINPUTSLOTTODEVICE:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = inputSlot
                // scriptEng.operands[1] = deviceID

                break;

            case FUNC_ISSLOTASSIGNED:
                // FUNCTION PARAMS:
                // scriptEng.operands[0] = inputSlot
                //
                // FUNCTION NOTES:
                // - Sets scriptEng.checkResult

                break;

            case FUNC_RESETINPUTSLOTASSIGNMENTS:
                // FUNCTION PARAMS:
                // None

                break;
#endif
        }

#if RETRO_PLATFORM == RETRO_PS1
#undef scriptInfo
#undef entity
#endif
        // Set Values
        if (opcodeSize > 0)
            scriptCodePtr -= scriptCodePtr - scriptCodeOffset;
        for (int i = 0; i < opcodeSize; ++i) {
            int opcodeType = PS1_RAW(scriptCodePtr++);
            if (opcodeType == SCRIPTVAR_VAR) {
                int arrayVal = 0;
                switch (PS1_RAW(scriptCodePtr++)) { // variable
                    case VARARR_NONE: arrayVal = objectEntityPos; break;

                    case VARARR_ARRAY:
                        if (PS1_RAW(scriptCodePtr++) == 1)
                            arrayVal = scriptEng.arrayPosition[PS1_RAW(scriptCodePtr++)];
                        else
                            arrayVal = scriptCode[scriptCodePtr++];
                        break;

                    case VARARR_ENTNOPLUS1:
                        if (PS1_RAW(scriptCodePtr++) == 1)
                            arrayVal = objectEntityPos + scriptEng.arrayPosition[PS1_RAW(scriptCodePtr++)];
                        else
                            arrayVal = objectEntityPos + scriptCode[scriptCodePtr++];
                        break;

                    case VARARR_ENTNOMINUS1:
                        if (PS1_RAW(scriptCodePtr++) == 1)
                            arrayVal = objectEntityPos - scriptEng.arrayPosition[PS1_RAW(scriptCodePtr++)];
                        else
                            arrayVal = objectEntityPos - scriptCode[scriptCodePtr++];
                        break;

                    default: break;
                }

#if RETRO_REV03 && !RETRO_USE_ORIGINAL_CODE && RETRO_PLATFORM != RETRO_PS1 // PS1: inputCheck is evaluated where it is read
                bool inputCheck = true; // Default to true for mobile bytecode
                // If we're using the scripts or an Origins datafile, check the array value
                if (forceUseScripts || Engine.usingOrigins)
                    inputCheck = arrayVal <= 1;
#endif

                // Variables
                switch (PS1_RAW(scriptCodePtr++)) {
                    default: break;
                    case VAR_TEMP0: scriptEng.temp[0] = scriptEng.operands[i]; break;
                    case VAR_TEMP1: scriptEng.temp[1] = scriptEng.operands[i]; break;
                    case VAR_TEMP2: scriptEng.temp[2] = scriptEng.operands[i]; break;
                    case VAR_TEMP3: scriptEng.temp[3] = scriptEng.operands[i]; break;
                    case VAR_TEMP4: scriptEng.temp[4] = scriptEng.operands[i]; break;
                    case VAR_TEMP5: scriptEng.temp[5] = scriptEng.operands[i]; break;
                    case VAR_TEMP6: scriptEng.temp[6] = scriptEng.operands[i]; break;
                    case VAR_TEMP7: scriptEng.temp[7] = scriptEng.operands[i]; break;
                    case VAR_CHECKRESULT: scriptEng.checkResult = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS0: scriptEng.arrayPosition[0] = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS1: scriptEng.arrayPosition[1] = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS2: scriptEng.arrayPosition[2] = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS3: scriptEng.arrayPosition[3] = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS4: scriptEng.arrayPosition[4] = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS5: scriptEng.arrayPosition[5] = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS6: scriptEng.arrayPosition[6] = scriptEng.operands[i]; break;
                    case VAR_ARRAYPOS7: scriptEng.arrayPosition[7] = scriptEng.operands[i]; break;
                    case VAR_GLOBAL: globalVariables[arrayVal] = scriptEng.operands[i]; break;
#if RETRO_PLATFORM == RETRO_PS1
                    case VAR_LOCAL: PS1ScriptWrite(arrayVal, scriptEng.operands[i]); break;
#else
                    case VAR_LOCAL: scriptCode[arrayVal] = scriptEng.operands[i]; break;
#endif
                    case VAR_OBJECTENTITYPOS: break;
                    case VAR_OBJECTGROUPID: {
                        PS1_OBJ(arrayVal).groupID = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTTYPE: {
                        PS1_OBJ(arrayVal).type = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTPROPERTYVALUE: {
                        PS1_OBJ(arrayVal).propertyValue = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTXPOS: {
                        PS1_OBJ(arrayVal).xpos = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTYPOS: {
                        PS1_OBJ(arrayVal).ypos = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTIXPOS: {
                        PS1_OBJ(arrayVal).xpos = scriptEng.operands[i] << 16;
                        break;
                    }
                    case VAR_OBJECTIYPOS: {
                        PS1_OBJ(arrayVal).ypos = scriptEng.operands[i] << 16;
                        break;
                    }
                    case VAR_OBJECTXVEL: {
                        PS1_OBJ(arrayVal).xvel = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTYVEL: {
                        PS1_OBJ(arrayVal).yvel = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTSPEED: {
                        PS1_OBJ(arrayVal).speed = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTSTATE: {
                        PS1_OBJ(arrayVal).state = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTROTATION: {
                        PS1_OBJ(arrayVal).rotation = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTSCALE: {
                        PS1_OBJ(arrayVal).scale = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTPRIORITY: {
                        PS1_OBJ(arrayVal).priority = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTDRAWORDER: {
                        PS1_OBJ(arrayVal).drawOrder = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTDIRECTION: {
                        PS1_OBJ(arrayVal).direction = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTINKEFFECT: {
                        PS1_OBJ(arrayVal).inkEffect = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTALPHA: {
                        PS1_OBJ(arrayVal).alpha = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTFRAME: {
                        PS1_OBJ(arrayVal).frame = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTANIMATION: {
                        PS1_OBJ(arrayVal).animation = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTPREVANIMATION: {
                        PS1_OBJ(arrayVal).prevAnimation = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTANIMATIONSPEED: {
                        PS1_OBJ(arrayVal).animationSpeed = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTANIMATIONTIMER: {
                        PS1_OBJ(arrayVal).animationTimer = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTANGLE: {
                        PS1_OBJ(arrayVal).angle = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTLOOKPOSX: {
                        PS1_OBJ(arrayVal).lookPosX = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTLOOKPOSY: {
                        PS1_OBJ(arrayVal).lookPosY = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTCOLLISIONMODE: {
                        PS1_OBJ(arrayVal).collisionMode = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTCOLLISIONPLANE: {
                        PS1_OBJ(arrayVal).collisionPlane = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTCONTROLMODE: {
                        PS1_OBJ(arrayVal).controlMode = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTCONTROLLOCK: {
                        PS1_OBJ(arrayVal).controlLock = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTPUSHING: {
                        PS1_OBJ(arrayVal).pushing = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVISIBLE: {
                        PS1_OBJ(arrayVal).visible = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTTILECOLLISIONS: {
                        PS1_OBJ(arrayVal).tileCollisions = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTINTERACTION: {
                        PS1_OBJ(arrayVal).objectInteractions = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTGRAVITY: {
                        PS1_OBJ(arrayVal).gravity = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTUP: {
                        PS1_OBJ(arrayVal).up = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTDOWN: {
                        PS1_OBJ(arrayVal).down = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTLEFT: {
                        PS1_OBJ(arrayVal).left = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTRIGHT: {
                        PS1_OBJ(arrayVal).right = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTJUMPPRESS: {
                        PS1_OBJ(arrayVal).jumpPress = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTJUMPHOLD: {
                        PS1_OBJ(arrayVal).jumpHold = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTSCROLLTRACKING: {
                        PS1_OBJ(arrayVal).scrollTracking = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORL: {
                        PS1_OBJ(arrayVal).floorSensors[0] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORC: {
                        PS1_OBJ(arrayVal).floorSensors[1] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORR: {
                        PS1_OBJ(arrayVal).floorSensors[2] = scriptEng.operands[i];
                        break;
                    }
#if !RETRO_REV00
                    case VAR_OBJECTFLOORSENSORLC: {
                        PS1_OBJ(arrayVal).floorSensors[3] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTFLOORSENSORRC: {
                        PS1_OBJ(arrayVal).floorSensors[4] = scriptEng.operands[i];
                        break;
                    }
#endif
                    case VAR_OBJECTCOLLISIONLEFT: {
                        break;
                    }
                    case VAR_OBJECTCOLLISIONTOP: {
                        break;
                    }
                    case VAR_OBJECTCOLLISIONRIGHT: {
                        break;
                    }
                    case VAR_OBJECTCOLLISIONBOTTOM: {
                        break;
                    }
                    case VAR_OBJECTOUTOFBOUNDS: {
                        break;
                    }
                    case VAR_OBJECTSPRITESHEET: {
                        objectScriptList[PS1_OBJ(arrayVal).type].spriteSheetID = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE0: {
                        PS1_OBJ(arrayVal).values[0] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE1: {
                        PS1_OBJ(arrayVal).values[1] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE2: {
                        PS1_OBJ(arrayVal).values[2] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE3: {
                        PS1_OBJ(arrayVal).values[3] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE4: {
                        PS1_OBJ(arrayVal).values[4] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE5: {
                        PS1_OBJ(arrayVal).values[5] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE6: {
                        PS1_OBJ(arrayVal).values[6] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE7: {
                        PS1_OBJ(arrayVal).values[7] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE8: {
                        PS1_OBJ(arrayVal).values[8] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE9: {
                        PS1_OBJ(arrayVal).values[9] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE10: {
                        PS1_OBJ(arrayVal).values[10] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE11: {
                        PS1_OBJ(arrayVal).values[11] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE12: {
                        PS1_OBJ(arrayVal).values[12] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE13: {
                        PS1_OBJ(arrayVal).values[13] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE14: {
                        PS1_OBJ(arrayVal).values[14] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE15: {
                        PS1_OBJ(arrayVal).values[15] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE16: {
                        PS1_OBJ(arrayVal).values[16] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE17: {
                        PS1_OBJ(arrayVal).values[17] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE18: {
                        PS1_OBJ(arrayVal).values[18] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE19: {
                        PS1_OBJ(arrayVal).values[19] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE20: {
                        PS1_OBJ(arrayVal).values[20] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE21: {
                        PS1_OBJ(arrayVal).values[21] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE22: {
                        PS1_OBJ(arrayVal).values[22] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE23: {
                        PS1_OBJ(arrayVal).values[23] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE24: {
                        PS1_OBJ(arrayVal).values[24] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE25: {
                        PS1_OBJ(arrayVal).values[25] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE26: {
                        PS1_OBJ(arrayVal).values[26] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE27: {
                        PS1_OBJ(arrayVal).values[27] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE28: {
                        PS1_OBJ(arrayVal).values[28] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE29: {
                        PS1_OBJ(arrayVal).values[29] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE30: {
                        PS1_OBJ(arrayVal).values[30] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE31: {
                        PS1_OBJ(arrayVal).values[31] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE32: {
                        PS1_OBJ(arrayVal).values[32] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE33: {
                        PS1_OBJ(arrayVal).values[33] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE34: {
                        PS1_OBJ(arrayVal).values[34] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE35: {
                        PS1_OBJ(arrayVal).values[35] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE36: {
                        PS1_OBJ(arrayVal).values[36] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE37: {
                        PS1_OBJ(arrayVal).values[37] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE38: {
                        PS1_OBJ(arrayVal).values[38] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE39: {
                        PS1_OBJ(arrayVal).values[39] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE40: {
                        PS1_OBJ(arrayVal).values[40] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE41: {
                        PS1_OBJ(arrayVal).values[41] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE42: {
                        PS1_OBJ(arrayVal).values[42] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE43: {
                        PS1_OBJ(arrayVal).values[43] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE44: {
                        PS1_OBJ(arrayVal).values[44] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE45: {
                        PS1_OBJ(arrayVal).values[45] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE46: {
                        PS1_OBJ(arrayVal).values[46] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_OBJECTVALUE47: {
                        PS1_OBJ(arrayVal).values[47] = scriptEng.operands[i];
                        break;
                    }
                    case VAR_STAGESTATE: stageMode = scriptEng.operands[i]; break;
                    case VAR_STAGEACTIVELIST:
#if RETRO_REV03 && !RETRO_USE_ORIGINAL_CODE
                        // BONUS_STAGE and SPECIAL_STAGE are swapped on Origins bytecode, so correct it here
                        if (Engine.usingOrigins && !forceUseScripts) {
                            int listSlots[] = { 0, 1, 3, 2 };

                            int listID = scriptEng.operands[i];
                            if (listID <= 3)
                                listID = listSlots[listID];
                            else
                                listID = 2; // BONUS_STAGE

                            activeStageList = listID;
                        }
                        else
                            activeStageList = scriptEng.operands[i];
#else
                        activeStageList = scriptEng.operands[i];
#endif
                        break;
                    case VAR_STAGELISTPOS: stageListPosition = scriptEng.operands[i]; break;
                    case VAR_STAGETIMEENABLED: timeEnabled = scriptEng.operands[i]; break;
                    case VAR_STAGEMILLISECONDS: stageMilliseconds = scriptEng.operands[i]; break;
                    case VAR_STAGESECONDS: stageSeconds = scriptEng.operands[i]; break;
                    case VAR_STAGEMINUTES: stageMinutes = scriptEng.operands[i]; break;
                    case VAR_STAGEACTNUM: actID = scriptEng.operands[i]; break;
                    case VAR_STAGEPAUSEENABLED: pauseEnabled = scriptEng.operands[i]; break;
                    case VAR_STAGELISTSIZE: break;
                    case VAR_STAGENEWXBOUNDARY1: newXBoundary1 = scriptEng.operands[i]; break;
                    case VAR_STAGENEWXBOUNDARY2: newXBoundary2 = scriptEng.operands[i]; break;
                    case VAR_STAGENEWYBOUNDARY1: newYBoundary1 = scriptEng.operands[i]; break;
                    case VAR_STAGENEWYBOUNDARY2: newYBoundary2 = scriptEng.operands[i]; break;
                    case VAR_STAGECURXBOUNDARY1:
                        if (curXBoundary1 != scriptEng.operands[i]) {
                            curXBoundary1 = scriptEng.operands[i];
                            newXBoundary1 = scriptEng.operands[i];
                        }
                        break;
                    case VAR_STAGECURXBOUNDARY2:
                        if (curXBoundary2 != scriptEng.operands[i]) {
                            curXBoundary2 = scriptEng.operands[i];
                            newXBoundary2 = scriptEng.operands[i];
                        }
                        break;
                    case VAR_STAGECURYBOUNDARY1:
                        if (curYBoundary1 != scriptEng.operands[i]) {
                            curYBoundary1 = scriptEng.operands[i];
                            newYBoundary1 = scriptEng.operands[i];
                        }
                        break;
                    case VAR_STAGECURYBOUNDARY2:
                        if (curYBoundary2 != scriptEng.operands[i]) {
                            curYBoundary2 = scriptEng.operands[i];
                            newYBoundary2 = scriptEng.operands[i];
                        }
                        break;
                    case VAR_STAGEDEFORMATIONDATA0: bgDeformationData0[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_STAGEDEFORMATIONDATA1: bgDeformationData1[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_STAGEDEFORMATIONDATA2: bgDeformationData2[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_STAGEDEFORMATIONDATA3: bgDeformationData3[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_STAGEWATERLEVEL: waterLevel = scriptEng.operands[i]; break;
                    case VAR_STAGEACTIVELAYER: activeTileLayers[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_STAGEMIDPOINT: tLayerMidPoint = scriptEng.operands[i]; break;
                    case VAR_STAGEPLAYERLISTPOS: playerListPos = scriptEng.operands[i]; break;
                    case VAR_STAGEDEBUGMODE: debugMode = scriptEng.operands[i]; break;
                    case VAR_STAGEENTITYPOS: objectEntityPos = scriptEng.operands[i]; break;
                    case VAR_SCREENCAMERAENABLED: cameraEnabled = scriptEng.operands[i]; break;
                    case VAR_SCREENCAMERATARGET: cameraTarget = scriptEng.operands[i]; break;
                    case VAR_SCREENCAMERASTYLE: cameraStyle = scriptEng.operands[i]; break;
                    case VAR_SCREENCAMERAX: cameraXPos = scriptEng.operands[i]; break;
                    case VAR_SCREENCAMERAY: cameraYPos = scriptEng.operands[i]; break;
                    case VAR_SCREENDRAWLISTSIZE: drawListEntries[arrayVal].listSize = scriptEng.operands[i]; break;
                    case VAR_SCREENXCENTER: break;
                    case VAR_SCREENYCENTER: break;
                    case VAR_SCREENXSIZE: break;
                    case VAR_SCREENYSIZE: break;
                    case VAR_SCREENXOFFSET: xScrollOffset = scriptEng.operands[i]; break;
                    case VAR_SCREENYOFFSET: yScrollOffset = scriptEng.operands[i]; break;
                    case VAR_SCREENSHAKEX: cameraShakeX = scriptEng.operands[i]; break;
                    case VAR_SCREENSHAKEY: cameraShakeY = scriptEng.operands[i]; break;
                    case VAR_SCREENADJUSTCAMERAY: cameraAdjustY = scriptEng.operands[i]; break;
                    case VAR_TOUCHSCREENDOWN: break;
                    case VAR_TOUCHSCREENXPOS: break;
                    case VAR_TOUCHSCREENYPOS: break;
                    case VAR_MUSICVOLUME: SetMusicVolume(scriptEng.operands[i]); break;
                    case VAR_MUSICCURRENTTRACK: break;
                    case VAR_MUSICPOSITION: break;
#if RETRO_REV03 && !RETRO_USE_ORIGINAL_CODE
                    case VAR_KEYDOWNUP:
                        if (inputCheck)
                            keyDown.up = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNDOWN:
                        if (inputCheck)
                            keyDown.down = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNLEFT:
                        if (inputCheck)
                            keyDown.left = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNRIGHT:
                        if (inputCheck)
                            keyDown.right = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONA:
                        if (inputCheck)
                            keyDown.A = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONB:
                        if (inputCheck)
                            keyDown.B = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONC:
                        if (inputCheck)
                            keyDown.C = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONX:
                        if (inputCheck)
                            keyDown.X = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONY:
                        if (inputCheck)
                            keyDown.Y = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONZ:
                        if (inputCheck)
                            keyDown.Z = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONL:
                        if (inputCheck)
                            keyDown.L = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNBUTTONR:
                        if (inputCheck)
                            keyDown.R = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNSTART:
                        if (inputCheck)
                            keyDown.start = scriptEng.operands[i];
                        break;
                    case VAR_KEYDOWNSELECT:
                        if (inputCheck)
                            keyDown.select = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSUP:
                        if (inputCheck)
                            keyPress.up = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSDOWN:
                        if (inputCheck)
                            keyPress.down = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSLEFT:
                        if (inputCheck)
                            keyPress.left = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSRIGHT:
                        if (inputCheck)
                            keyPress.right = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONA:
                        if (inputCheck)
                            keyPress.A = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONB:
                        if (inputCheck)
                            keyPress.B = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONC:
                        if (inputCheck)
                            keyPress.C = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONX:
                        if (inputCheck)
                            keyPress.X = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONY:
                        if (inputCheck)
                            keyPress.Y = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONZ:
                        if (inputCheck)
                            keyPress.Z = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONL:
                        if (inputCheck)
                            keyPress.L = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSBUTTONR:
                        if (inputCheck)
                            keyPress.R = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSSTART:
                        if (inputCheck)
                            keyPress.start = scriptEng.operands[i];
                        break;
                    case VAR_KEYPRESSSELECT:
                        if (inputCheck)
                            keyPress.select = scriptEng.operands[i];
                        break;
#else
                    case VAR_KEYDOWNUP: keyDown.up = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNDOWN: keyDown.down = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNLEFT: keyDown.left = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNRIGHT: keyDown.right = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONA: keyDown.A = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONB: keyDown.B = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONC: keyDown.C = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONX: keyDown.X = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONY: keyDown.Y = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONZ: keyDown.Z = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONL: keyDown.L = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNBUTTONR: keyDown.R = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNSTART: keyDown.start = scriptEng.operands[i]; break;
                    case VAR_KEYDOWNSELECT: keyDown.select = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSUP: keyPress.up = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSDOWN: keyPress.down = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSLEFT: keyPress.left = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSRIGHT: keyPress.right = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONA: keyPress.A = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONB: keyPress.B = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONC: keyPress.C = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONX: keyPress.X = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONY: keyPress.Y = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONZ: keyPress.Z = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONL: keyPress.L = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSBUTTONR: keyPress.R = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSSTART: keyPress.start = scriptEng.operands[i]; break;
                    case VAR_KEYPRESSSELECT: keyPress.select = scriptEng.operands[i]; break;
#endif
                    case VAR_MENU1SELECTION: gameMenu[0].selection1 = scriptEng.operands[i]; break;
                    case VAR_MENU2SELECTION: gameMenu[1].selection1 = scriptEng.operands[i]; break;
                    case VAR_TILELAYERXSIZE: stageLayouts[arrayVal].xsize = scriptEng.operands[i]; break;
                    case VAR_TILELAYERYSIZE: stageLayouts[arrayVal].ysize = scriptEng.operands[i]; break;
                    case VAR_TILELAYERTYPE: stageLayouts[arrayVal].type = scriptEng.operands[i]; break;
                    case VAR_TILELAYERANGLE: {
                        int angle = scriptEng.operands[i] + 0x200;
                        if (scriptEng.operands[i] >= 0)
                            angle = scriptEng.operands[i];
                        stageLayouts[arrayVal].angle = angle & 0x1FF;
                        break;
                    }
                    case VAR_TILELAYERXPOS: stageLayouts[arrayVal].xpos = scriptEng.operands[i]; break;
                    case VAR_TILELAYERYPOS: stageLayouts[arrayVal].ypos = scriptEng.operands[i]; break;
                    case VAR_TILELAYERZPOS: stageLayouts[arrayVal].zpos = scriptEng.operands[i]; break;
                    case VAR_TILELAYERPARALLAXFACTOR: stageLayouts[arrayVal].parallaxFactor = scriptEng.operands[i]; break;
                    case VAR_TILELAYERSCROLLSPEED: stageLayouts[arrayVal].scrollSpeed = scriptEng.operands[i]; break;
                    case VAR_TILELAYERSCROLLPOS: stageLayouts[arrayVal].scrollPos = scriptEng.operands[i]; break;
                    case VAR_TILELAYERDEFORMATIONOFFSET: stageLayouts[arrayVal].deformationOffset = scriptEng.operands[i]; break;
                    case VAR_TILELAYERDEFORMATIONOFFSETW: stageLayouts[arrayVal].deformationOffsetW = scriptEng.operands[i]; break;
                    case VAR_HPARALLAXPARALLAXFACTOR: hParallax.parallaxFactor[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_HPARALLAXSCROLLSPEED: hParallax.scrollSpeed[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_HPARALLAXSCROLLPOS: hParallax.scrollPos[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_VPARALLAXPARALLAXFACTOR: vParallax.parallaxFactor[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_VPARALLAXSCROLLSPEED: vParallax.scrollSpeed[arrayVal] = scriptEng.operands[i]; break;
                    case VAR_VPARALLAXSCROLLPOS: vParallax.scrollPos[arrayVal] = scriptEng.operands[i]; break;
#if RETRO_PLATFORM == RETRO_PS1
                    case VAR_SCENE3DVERTEXCOUNT:
                        if (scriptEng.operands[i] > g_ps1Scene3DMaxCountV)
                            g_ps1Scene3DMaxCountV = scriptEng.operands[i];
                        vertexCount = PS1_VTX_COUNT(scriptEng.operands[i]);
                        break;
                    case VAR_SCENE3DFACECOUNT:
                        if (scriptEng.operands[i] > g_ps1Scene3DMaxCountF)
                            g_ps1Scene3DMaxCountF = scriptEng.operands[i];
                        faceCount = PS1_FACE_COUNT(scriptEng.operands[i]);
                        break;
#else
                    case VAR_SCENE3DVERTEXCOUNT: vertexCount = PS1_VTX_COUNT(scriptEng.operands[i]); break;
                    case VAR_SCENE3DFACECOUNT: faceCount = PS1_FACE_COUNT(scriptEng.operands[i]); break;
#endif
                    case VAR_SCENE3DPROJECTIONX: projectionX = scriptEng.operands[i]; break;
                    case VAR_SCENE3DPROJECTIONY: projectionY = scriptEng.operands[i]; break;
#if !RETRO_REV00
                    case VAR_SCENE3DFOGCOLOR: fogColor = scriptEng.operands[i]; break;
                    case VAR_SCENE3DFOGSTRENGTH: fogStrength = scriptEng.operands[i]; break;
#endif
                    case VAR_VERTEXBUFFERX: vertexBuffer[PS1_VTX(arrayVal)].x = scriptEng.operands[i]; break;
                    case VAR_VERTEXBUFFERY: vertexBuffer[PS1_VTX(arrayVal)].y = scriptEng.operands[i]; break;
                    case VAR_VERTEXBUFFERZ: vertexBuffer[PS1_VTX(arrayVal)].z = scriptEng.operands[i]; break;
                    case VAR_VERTEXBUFFERU: vertexBuffer[PS1_VTX(arrayVal)].u = scriptEng.operands[i]; break;
                    case VAR_VERTEXBUFFERV: vertexBuffer[PS1_VTX(arrayVal)].v = scriptEng.operands[i]; break;
                    case VAR_FACEBUFFERA: faceBuffer[PS1_FACE(arrayVal)].a = scriptEng.operands[i]; break;
                    case VAR_FACEBUFFERB: faceBuffer[PS1_FACE(arrayVal)].b = scriptEng.operands[i]; break;
                    case VAR_FACEBUFFERC: faceBuffer[PS1_FACE(arrayVal)].c = scriptEng.operands[i]; break;
                    case VAR_FACEBUFFERD: faceBuffer[PS1_FACE(arrayVal)].d = scriptEng.operands[i]; break;
                    case VAR_FACEBUFFERFLAG: faceBuffer[PS1_FACE(arrayVal)].flag = scriptEng.operands[i]; break;
                    case VAR_FACEBUFFERCOLOR: faceBuffer[PS1_FACE(arrayVal)].color = scriptEng.operands[i]; break;
#if RETRO_PLATFORM == RETRO_PS1
                    case VAR_SAVERAM:
                        if ((uint)arrayVal < SAVEDATA_SIZE)
                            saveRAM[arrayVal] = scriptEng.operands[i];
                        else
                            g_ps1SaveRAMClamped = g_ps1SaveRAMClamped + 1;
                        break;
#else
                    case VAR_SAVERAM: saveRAM[arrayVal] = scriptEng.operands[i]; break;
#endif
                    case VAR_ENGINESTATE: Engine.gameMode = scriptEng.operands[i]; break;
#if RETRO_REV00
                    case VAR_ENGINEMESSAGE: break;
#endif
                    case VAR_ENGINELANGUAGE: Engine.language = scriptEng.operands[i]; break;
                    case VAR_ENGINEONLINEACTIVE: Engine.onlineActive = scriptEng.operands[i]; break;
                    case VAR_ENGINESFXVOLUME:
                        sfxVolume = scriptEng.operands[i];
                        SetGameVolumes(bgmVolume, sfxVolume);
                        break;
                    case VAR_ENGINEBGMVOLUME:
                        bgmVolume = scriptEng.operands[i];
                        SetGameVolumes(bgmVolume, sfxVolume);
                        break;
#if RETRO_REV00
                    case VAR_ENGINEPLATFORMID: break;
#endif
                    case VAR_ENGINETRIALMODE: Engine.trialMode = scriptEng.operands[i]; break;
#if !RETRO_REV00
                    case VAR_ENGINEDEVICETYPE: break;
#endif

#if RETRO_REV03
                    // Origins Extras
                    // Due to using regular v4, these don't support array values like origins expects, so its always screen[0]
                    case VAR_SCREENCURRENTID: break;
                    case VAR_CAMERAENABLED:
                        if (arrayVal == 0)
                            cameraEnabled = scriptEng.operands[i];
                        break;
                    case VAR_CAMERATARGET:
                        if (arrayVal == 0)
                            cameraTarget = scriptEng.operands[i];
                        break;
                    case VAR_CAMERASTYLE:
                        if (arrayVal == 0)
                            cameraStyle = scriptEng.operands[i];
                        break;
                    case VAR_CAMERAXPOS:
                        if (arrayVal == 0)
                            cameraXPos = scriptEng.operands[i];
                        break;
                    case VAR_CAMERAYPOS:
                        if (arrayVal == 0)
                            cameraYPos = scriptEng.operands[i];
                        break;
                    case VAR_CAMERAADJUSTY:
                        if (arrayVal == 0)
                            cameraAdjustY = scriptEng.operands[i];
                        break;
#endif

#if RETRO_USE_HAPTICS
                    case VAR_HAPTICSENABLED: Engine.hapticsEnabled = scriptEng.operands[i]; break;
#endif
                }
            }
            else if (opcodeType == SCRIPTVAR_INTCONST) { // int constant
                scriptCodePtr++;
            }
            else if (opcodeType == SCRIPTVAR_STRCONST) { // string constant
                int strLen = scriptCode[scriptCodePtr++];
                for (int c = 0; c < strLen; ++c) {
                    switch (c % 4) {
                        case 0: break;
                        case 1: break;
                        case 2: break;
                        case 3: ++scriptCodePtr; break;
                        default: break;
                    }
                }
                scriptCodePtr++;
            }
        }
    }
}
