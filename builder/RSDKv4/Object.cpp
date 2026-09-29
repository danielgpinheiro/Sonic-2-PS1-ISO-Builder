#include "RetroEngine.hpp"

// Native Objects
int nativeEntityPos;

int activeEntityList[NATIVEENTITY_COUNT];
byte objectRemoveFlag[NATIVEENTITY_COUNT];
NativeEntity objectEntityBank[NATIVEENTITY_COUNT];
int nativeEntityCount = 0;

int nativeEntityCountBackup = 0;
int backupEntityList[NATIVEENTITY_COUNT];
NativeEntity objectEntityBackup[NATIVEENTITY_COUNT];

int nativeEntityCountBackupS = 0;
int backupEntityListS[NATIVEENTITY_COUNT];
NativeEntity objectEntityBackupS[NATIVEENTITY_COUNT];

// Game Objects
int objectEntityPos = 0;
int curObjectType   = 0;
#if RETRO_PLATFORM == RETRO_PS1
// PS1: no "storage" half (Sonic 2's scripts never CopyObject into it); slot ENTITY_COUNT is the blank scratch
// entity script accesses past the array get (Script.cpp PS1_OBJ).
Entity objectEntityList[ENTITY_COUNT + 1];
#else
Entity objectEntityList[ENTITY_COUNT * 2]; //"regular" list & "storage" list
#endif
int processObjectFlag[ENTITY_COUNT];
TypeGroupList objectTypeGroupList[TYPEGROUP_COUNT];
#if RETRO_PLATFORM == RETRO_PS1
// PS1 (docs/30 phase 2): the type/group lists share one pool instead of ENTITY_COUNT refs each (0x103 lists x
// 4.7 KB = 1.2 MB upstream). Every interacting entity joins at most 3 lists (its custom group, its type, "all"),
// so the pool holds 3 x ENTITY_COUNT refs. Two passes (count, then place) give each list the same entities in
// the same (ascending slot) order as upstream's single pass.
static short s_ps1TypeGroupPool[3 * ENTITY_COUNT];
// Only the groups that have members are cleared and placed (a list of them, docs/30 7.2b): the other lists of the 259
// keep listSize 0 (a script walking one reads no refs); each list gets the same entities in the same order.
static short s_ps1UsedGroups[TYPEGROUP_COUNT];
static int s_ps1UsedGroupCount = 0;
static inline void PS1TypeGroupsClear()
{
    for (int i = 0; i < s_ps1UsedGroupCount; ++i) objectTypeGroupList[s_ps1UsedGroups[i]].listSize = 0;
    s_ps1UsedGroupCount = 0;
}
static inline void PS1TypeGroupCount(int group)
{
    if (objectTypeGroupList[group].listSize++ == 0)
        s_ps1UsedGroups[s_ps1UsedGroupCount++] = (short)group;
}
static inline void PS1TypeGroupsPlace()
{
    int offset = 0;
    for (int i = 0; i < s_ps1UsedGroupCount; ++i) {
        TypeGroupList &list = objectTypeGroupList[s_ps1UsedGroups[i]];
        list.entityRefs     = &s_ps1TypeGroupPool[offset];
        offset += list.listSize;
        list.listSize = 0;
    }
}
static inline void PS1TypeGroupsAdd(int slot)
{
    Entity *entity = &objectEntityList[slot];
    if (entity->groupID >= OBJECT_COUNT) {
        TypeGroupList *listCustom                      = &objectTypeGroupList[entity->groupID];
        listCustom->entityRefs[listCustom->listSize++] = slot;
    }
    TypeGroupList *listType                    = &objectTypeGroupList[entity->type];
    listType->entityRefs[listType->listSize++] = slot;
    TypeGroupList *listAll                   = &objectTypeGroupList[GROUP_ALL];
    listAll->entityRefs[listAll->listSize++] = slot;
}
static void PS1BuildTypeGroups()
{
    static short s_active[ENTITY_COUNT]; // pass 1's qualifying slots, ascending: pass 2 walks only these
    int active = 0;
    PS1TypeGroupsClear();
    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        Entity *entity = &objectEntityList[objectEntityPos];
        if (processObjectFlag[objectEntityPos] && entity->objectInteractions) {
            if (entity->groupID >= OBJECT_COUNT)
                PS1TypeGroupCount(entity->groupID);
            PS1TypeGroupCount(entity->type);
            PS1TypeGroupCount(GROUP_ALL);
            s_active[active++] = objectEntityPos;
        }
    }
    PS1TypeGroupsPlace();
    for (int a = 0; a < active; ++a) PS1TypeGroupsAdd(s_active[a]);
    objectEntityPos = ENTITY_COUNT; // as upstream's loops leave it
}
// The same lists from a candidate slot list (ascending; ProcessObjects' flagged slots): each list gets the same
// entities in the same order as the full walk, which only ever finds flagged slots.
#if PS1_GAME == 1 && !defined(RETRO_PS1_HOST_TOOL)
bool PS1S1TopSub(int ptr); // Script.cpp: a sub that is one native opcode, run without ProcessScript
#endif
static void PS1BuildTypeGroupsFrom(const short *cand, int n)
{
    static short s_active[ENTITY_COUNT];
    int active = 0;
    PS1TypeGroupsClear();
    for (int c = 0; c < n; ++c) {
        int slot       = cand[c];
        Entity *entity = &objectEntityList[slot];
        if (processObjectFlag[slot] && entity->objectInteractions) {
            if (entity->groupID >= OBJECT_COUNT)
                PS1TypeGroupCount(entity->groupID);
            PS1TypeGroupCount(entity->type);
            PS1TypeGroupCount(GROUP_ALL);
            s_active[active++] = slot;
        }
    }
    PS1TypeGroupsPlace();
    for (int a = 0; a < active; ++a) PS1TypeGroupsAdd(s_active[a]);
    objectEntityPos = ENTITY_COUNT; // as upstream's loops leave it
}
#endif

#if RETRO_PLATFORM == RETRO_PS1
char typeNames[OBJECT_COUNT][0x20];
#else
char typeNames[OBJECT_COUNT][0x40];
#endif

int OBJECT_BORDER_X1 = 0x80;
int OBJECT_BORDER_X2 = SCREEN_XSIZE + 0x80;
int OBJECT_BORDER_X3 = 0x20;
int OBJECT_BORDER_X4 = SCREEN_XSIZE + 0x20;

const int OBJECT_BORDER_Y1 = 0x100;
const int OBJECT_BORDER_Y2 = SCREEN_YSIZE + 0x100;
const int OBJECT_BORDER_Y3 = 0x80;
const int OBJECT_BORDER_Y4 = SCREEN_YSIZE + 0x80;

int playerListPos = 0;

#if RETRO_PLATFORM == RETRO_PS1
// GDB hook before each object type's startup sub (load-time profiles: break here, print the vblank counter; docs/30 7.3)
__attribute__((noinline)) void PS1StartupTypeHook(int type) { asm volatile("" : : "r"(type)); }
#endif
void ProcessStartupObjects()
{
    scriptFrameCount = 0;
    ClearAnimationData();
    scriptEng.arrayPosition[8] = TEMPENTITY_START;
    OBJECT_BORDER_X1           = 0x80;
    OBJECT_BORDER_X3           = 0x20;
    OBJECT_BORDER_X2           = SCREEN_XSIZE + 0x80;
    OBJECT_BORDER_X4           = SCREEN_XSIZE + 0x20;
    Entity *entity             = &objectEntityList[TEMPENTITY_START];
    // Dunno what this is meant for, but it's here in the original code so...
    objectEntityList[TEMPENTITY_START + 1].type = objectEntityList[0].type;

    memset(foreachStack, -1, sizeof(foreachStack));
    memset(jumpTableStack, 0, sizeof(jumpTableStack));

    for (int i = 0; i < OBJECT_COUNT; ++i) {
        ObjectScript *scriptInfo    = &objectScriptList[i];
        objectEntityPos             = TEMPENTITY_START;
        curObjectType               = i;
        scriptInfo->frameListOffset = scriptFrameCount;
        scriptInfo->spriteSheetID   = 0;
        entity->type                = i;

#if RETRO_PLATFORM == RETRO_PS1
        PS1StartupTypeHook(i);
#endif
        if (scriptCode[scriptInfo->eventStartup.scriptCodePtr] > 0)
            ProcessScript(scriptInfo->eventStartup.scriptCodePtr, scriptInfo->eventStartup.jumpTablePtr, EVENT_SETUP);
        scriptInfo->frameCount = scriptFrameCount - scriptInfo->frameListOffset;
    }
    entity->type  = 0;
    curObjectType = 0;
}

#if RETRO_PLATFORM == RETRO_PS1
#include "../ps1/video.hh"
// Per-object-type CPU profile of the last frame (root counter 2 ticks, 0.24 us; GDB + tools/obj_profile.py):
// update subs (ProcessObjects) and draw subs (DrawObjectList), with call counts.
uint32_t g_ps1TypeTicks[OBJECT_COUNT], g_ps1TypeCalls[OBJECT_COUNT];
uint32_t g_ps1TypeDrawTicks[OBJECT_COUNT], g_ps1TypeDrawCalls[OBJECT_COUNT];
uint32_t g_ps1TypeOps[OBJECT_COUNT]; // VM instructions of the update subs, last frame
extern uint32_t g_ps1VmOps;
volatile uint32_t g_ps1ObjSlotsWalked = 0;
#endif

#if RETRO_PLATFORM == RETRO_PS1
// PS1 ProcessObjects (docs/30 CPU work): upstream's loop with (1) the activity bounds kept in locals, refreshed after
// every script run (a script may move the camera or the borders), instead of re-reading five globals for each of
// the ~400 live slots, and (2) the slots whose process flag it sets recorded in order: only this loop sets the flag,
// so the type groups are built from that list (PS1BuildTypeGroupsFrom) instead of another walk of all slots.
static short s_ps1Processed[ENTITY_COUNT];
void ProcessObjects()
{
#if PS1_PROFILE
    memset(g_ps1TypeTicks, 0, sizeof(g_ps1TypeTicks));
    memset(g_ps1TypeCalls, 0, sizeof(g_ps1TypeCalls));
    memset(g_ps1TypeOps, 0, sizeof(g_ps1TypeOps));
#endif
    for (int i = 0; i < DRAWLAYER_COUNT; ++i) drawListEntries[i].listSize = 0;

    int processed = 0;
    int bx1, bx2, by1, by2, sx3, sx4, sy3, sy4;
    auto bounds = [&]() {
        bx1 = xScrollOffset - OBJECT_BORDER_X1;
        bx2 = xScrollOffset + OBJECT_BORDER_X2;
        by1 = yScrollOffset - OBJECT_BORDER_Y1;
        by2 = yScrollOffset + OBJECT_BORDER_Y2;
        sx3 = xScrollOffset - OBJECT_BORDER_X3;
        sx4 = OBJECT_BORDER_X4 + xScrollOffset;
        sy3 = yScrollOffset - OBJECT_BORDER_Y3;
        sy4 = yScrollOffset + OBJECT_BORDER_Y4;
    };
    bounds();
    // The slot index is a local (the global objectEntityPos, which scripts read, is set only around their runs and left
    // at ENTITY_COUNT as upstream's loop leaves it): no global load / store for each of the 896 slots (docs/30 7.2b).
    Entity *entity = objectEntityList;
    for (int pos = 0; pos < ENTITY_COUNT; ++pos, ++entity) {
        processObjectFlag[pos] = false;
        // A blank slot does nothing below (the priority switch only decides whether a typed object runs; its
        // process flag stays false; XBOUNDS_DESTROY would blank it again): skip it (Sonic CD port, docs/28).
        if (entity->type == OBJ_TYPE_BLANKOBJECT)
            continue;
        objectEntityPos = pos;
        int x = entity->xpos >> 16;
        int y = entity->ypos >> 16;
        bool flag = false;
        switch (entity->priority) {
            case PRIORITY_BOUNDS: flag = x > bx1 && x < bx2 && y > by1 && y < by2; break;
            case PRIORITY_ACTIVE:
            case PRIORITY_ALWAYS:
            case PRIORITY_ACTIVE_SMALL: flag = true; break;
            case PRIORITY_XBOUNDS: flag = x > bx1 && x < bx2; break;
            case PRIORITY_XBOUNDS_DESTROY:
                flag = x > bx1 && x < bx2;
                if (!flag)
                    entity->type = OBJ_TYPE_BLANKOBJECT;
                break;
            case PRIORITY_INACTIVE: flag = false; break;
            case PRIORITY_BOUNDS_SMALL: flag = x > sx3 && x < sx4 && y > sy3 && y < sy4; break;
            default: break;
        }
        processObjectFlag[pos] = flag;
        if (flag)
            s_ps1Processed[processed++] = pos;

        if (flag && entity->type > OBJ_TYPE_BLANKOBJECT) {
            ObjectScript *scriptInfo = &objectScriptList[entity->type];
#if PS1_PROFILE
            int ps1Type     = entity->type;
            uint32_t ps1Op0 = g_ps1VmOps;
            uint32_t ps1T0  = PS1ProfTicks();
#endif
            if (scriptCode[scriptInfo->eventUpdate.scriptCodePtr] > 0) {
#if PS1_GAME == 1 && !defined(RETRO_PS1_HOST_TOOL)
                if (!PS1S1TopSub(scriptInfo->eventUpdate.scriptCodePtr)) // one native opcode: no ProcessScript (Script.cpp)
#endif
                ProcessScript(scriptInfo->eventUpdate.scriptCodePtr, scriptInfo->eventUpdate.jumpTablePtr, EVENT_MAIN);
                bounds();
            }
#if PS1_PROFILE
            g_ps1TypeTicks[ps1Type] += (PS1ProfTicks() - ps1T0) & 0xFFFF;
            g_ps1TypeCalls[ps1Type]++;
            g_ps1TypeOps[ps1Type] += g_ps1VmOps - ps1Op0;
#endif

            if (entity->drawOrder < DRAWLAYER_COUNT)
                drawListEntries[entity->drawOrder].entityRefs[drawListEntries[entity->drawOrder].listSize++] = pos;
        }
    }
    objectEntityPos = ENTITY_COUNT;

    PS1BuildTypeGroupsFrom(s_ps1Processed, processed);
}
#else
void ProcessObjects()
{
#if RETRO_PLATFORM == RETRO_PS1
    memset(g_ps1TypeTicks, 0, sizeof(g_ps1TypeTicks));
    memset(g_ps1TypeCalls, 0, sizeof(g_ps1TypeCalls));
    memset(g_ps1TypeOps, 0, sizeof(g_ps1TypeOps));
#endif
    for (int i = 0; i < DRAWLAYER_COUNT; ++i) drawListEntries[i].listSize = 0;

    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        processObjectFlag[objectEntityPos] = false;
        int x = 0, y = 0;
        Entity *entity = &objectEntityList[objectEntityPos];
#if RETRO_PLATFORM == RETRO_PS1
        // A blank slot does nothing below (the priority switch only decides whether a typed object runs; its
        // process flag stays false; XBOUNDS_DESTROY would blank it again): skip it (Sonic CD port, docs/28).
        if (entity->type == OBJ_TYPE_BLANKOBJECT)
            continue;
#endif
        x              = entity->xpos >> 16;
        y              = entity->ypos >> 16;

        switch (entity->priority) {
            case PRIORITY_BOUNDS:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X1 && x < xScrollOffset + OBJECT_BORDER_X2
                                                     && y > yScrollOffset - OBJECT_BORDER_Y1 && y < yScrollOffset + OBJECT_BORDER_Y2;
                break;

            case PRIORITY_ACTIVE:
            case PRIORITY_ALWAYS:
            case PRIORITY_ACTIVE_SMALL: processObjectFlag[objectEntityPos] = true; break;

            case PRIORITY_XBOUNDS:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X1 && x < OBJECT_BORDER_X2 + xScrollOffset;
                break;

            case PRIORITY_XBOUNDS_DESTROY:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X1 && x < xScrollOffset + OBJECT_BORDER_X2;
                if (!processObjectFlag[objectEntityPos]) {
                    processObjectFlag[objectEntityPos] = false;
                    entity->type                       = OBJ_TYPE_BLANKOBJECT;
                }
                break;

            case PRIORITY_INACTIVE: processObjectFlag[objectEntityPos] = false; break;
            case PRIORITY_BOUNDS_SMALL:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X3 && x < OBJECT_BORDER_X4 + xScrollOffset
                                                     && y > yScrollOffset - OBJECT_BORDER_Y3 && y < yScrollOffset + OBJECT_BORDER_Y4;
                break;

            default: break;
        }

        if (processObjectFlag[objectEntityPos] && entity->type > OBJ_TYPE_BLANKOBJECT) {
            ObjectScript *scriptInfo = &objectScriptList[entity->type];
#if RETRO_PLATFORM == RETRO_PS1
            int ps1Type    = entity->type;
            uint32_t ps1Op0 = g_ps1VmOps;
            uint32_t ps1T0 = PS1ProfTicks();
#endif
            if (scriptCode[scriptInfo->eventUpdate.scriptCodePtr] > 0)
                ProcessScript(scriptInfo->eventUpdate.scriptCodePtr, scriptInfo->eventUpdate.jumpTablePtr, EVENT_MAIN);
#if RETRO_PLATFORM == RETRO_PS1
            g_ps1TypeTicks[ps1Type] += (PS1ProfTicks() - ps1T0) & 0xFFFF;
            g_ps1TypeCalls[ps1Type]++;
            g_ps1TypeOps[ps1Type] += g_ps1VmOps - ps1Op0;
#endif

            if (entity->drawOrder < DRAWLAYER_COUNT)
                drawListEntries[entity->drawOrder].entityRefs[drawListEntries[entity->drawOrder].listSize++] = objectEntityPos;
        }
    }

#if RETRO_PLATFORM == RETRO_PS1
    PS1BuildTypeGroups();
#else
    for (int i = 0; i < TYPEGROUP_COUNT; ++i) objectTypeGroupList[i].listSize = 0;

    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        Entity *entity = &objectEntityList[objectEntityPos];
        if (processObjectFlag[objectEntityPos] && entity->objectInteractions) {
            // Custom Group
            if (entity->groupID >= OBJECT_COUNT) {
                TypeGroupList *listCustom                      = &objectTypeGroupList[objectEntityList[objectEntityPos].groupID];
                listCustom->entityRefs[listCustom->listSize++] = objectEntityPos;
            }

            // Type-Specific list
            TypeGroupList *listType                    = &objectTypeGroupList[objectEntityList[objectEntityPos].type];
            listType->entityRefs[listType->listSize++] = objectEntityPos;

            // All Entities list
            TypeGroupList *listAll                   = &objectTypeGroupList[GROUP_ALL];
            listAll->entityRefs[listAll->listSize++] = objectEntityPos;
        }
    }
#endif
}
#endif
void ProcessPausedObjects()
{
    for (int i = 0; i < DRAWLAYER_COUNT; ++i) drawListEntries[i].listSize = 0;

    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        Entity *entity = &objectEntityList[objectEntityPos];

        if (entity->priority == PRIORITY_ALWAYS && entity->type > OBJ_TYPE_BLANKOBJECT) {
            ObjectScript *scriptInfo = &objectScriptList[entity->type];
            if (scriptCode[scriptInfo->eventUpdate.scriptCodePtr] > 0)
                ProcessScript(scriptInfo->eventUpdate.scriptCodePtr, scriptInfo->eventUpdate.jumpTablePtr, EVENT_MAIN);

            if (entity->drawOrder < DRAWLAYER_COUNT && entity->drawOrder >= 0)
                drawListEntries[entity->drawOrder].entityRefs[drawListEntries[entity->drawOrder].listSize++] = objectEntityPos;
        }
    }
}
void ProcessFrozenObjects()
{
    for (int i = 0; i < DRAWLAYER_COUNT; ++i) drawListEntries[i].listSize = 0;

    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        processObjectFlag[objectEntityPos] = false;
        int x = 0, y = 0;
        Entity *entity = &objectEntityList[objectEntityPos];
        x              = entity->xpos >> 16;
        y              = entity->ypos >> 16;

        switch (entity->priority) {
            case PRIORITY_BOUNDS:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X1 && x < xScrollOffset + OBJECT_BORDER_X2
                                                     && y > yScrollOffset - OBJECT_BORDER_Y1 && y < yScrollOffset + OBJECT_BORDER_Y2;
                break;

            case PRIORITY_ACTIVE:
            case PRIORITY_ALWAYS:
            case PRIORITY_ACTIVE_SMALL: processObjectFlag[objectEntityPos] = true; break;

            case PRIORITY_XBOUNDS:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X1 && x < OBJECT_BORDER_X2 + xScrollOffset;
                break;

            case PRIORITY_XBOUNDS_DESTROY:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X1 && x < xScrollOffset + OBJECT_BORDER_X2;
                if (!processObjectFlag[objectEntityPos]) {
                    processObjectFlag[objectEntityPos] = false;
                    entity->type                       = OBJ_TYPE_BLANKOBJECT;
                }
                break;

            case PRIORITY_INACTIVE: processObjectFlag[objectEntityPos] = false; break;

            case PRIORITY_BOUNDS_SMALL:
                processObjectFlag[objectEntityPos] = x > xScrollOffset - OBJECT_BORDER_X3 && x < OBJECT_BORDER_X4 + xScrollOffset
                                                     && y > yScrollOffset - OBJECT_BORDER_Y3 && y < yScrollOffset + OBJECT_BORDER_Y4;
                break;

            default: break;
        }

        if (processObjectFlag[objectEntityPos] && entity->type > OBJ_TYPE_BLANKOBJECT) {
            ObjectScript *scriptInfo = &objectScriptList[entity->type];
            if (scriptCode[scriptInfo->eventUpdate.scriptCodePtr] > 0 && entity->priority == PRIORITY_ALWAYS)
                ProcessScript(scriptInfo->eventUpdate.scriptCodePtr, scriptInfo->eventUpdate.jumpTablePtr, EVENT_MAIN);

            if (entity->drawOrder < DRAWLAYER_COUNT && entity->drawOrder >= 0)
                drawListEntries[entity->drawOrder].entityRefs[drawListEntries[entity->drawOrder].listSize++] = objectEntityPos;
        }
    }

#if RETRO_PLATFORM == RETRO_PS1
    PS1BuildTypeGroups();
#else
    for (int i = 0; i < TYPEGROUP_COUNT; ++i) objectTypeGroupList[i].listSize = 0;

    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        Entity *entity = &objectEntityList[objectEntityPos];
        if (processObjectFlag[objectEntityPos] && entity->objectInteractions) {
            // Custom Group
            if (entity->groupID >= OBJECT_COUNT) {
                TypeGroupList *listCustom                      = &objectTypeGroupList[objectEntityList[objectEntityPos].groupID];
                listCustom->entityRefs[listCustom->listSize++] = objectEntityPos;
            }

            // Type-Specific list
            TypeGroupList *listType                    = &objectTypeGroupList[objectEntityList[objectEntityPos].type];
            listType->entityRefs[listType->listSize++] = objectEntityPos;

            // All Entities list
            TypeGroupList *listAll                   = &objectTypeGroupList[GROUP_ALL];
            listAll->entityRefs[listAll->listSize++] = objectEntityPos;
        }
    }
#endif
}
#if !RETRO_REV00
void Process2PObjects()
{
    for (int i = 0; i < DRAWLAYER_COUNT; ++i) drawListEntries[i].listSize = 0;

    int boundX1 = -(0x200 << 16);
    int boundX2 = (0x200 << 16);
    int boundX3 = -(0x180 << 16);
    int boundX4 = (0x180 << 16);

    int boundY1 = -(0x180 << 16);
    int boundY2 = (0x180 << 16);
    int boundY3 = -(0x100 << 16);
    int boundY4 = (0x100 << 16);

    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        processObjectFlag[objectEntityPos] = false;
        int x = 0, y = 0;

        Entity *entity = &objectEntityList[objectEntityPos];
        x              = entity->xpos;
        y              = entity->ypos;

        // Set these here, they could (and prolly are) updated after objects
        Entity *entityP1 = &objectEntityList[0];
        int XPosP1       = entityP1->xpos;
        int YPosP1       = entityP1->ypos;
        Entity *entityP2 = &objectEntityList[1];
        int XPosP2       = entityP2->xpos;
        int YPosP2       = entityP2->ypos;

        switch (entity->priority) {
            case PRIORITY_BOUNDS:
                processObjectFlag[objectEntityPos] = x > XPosP1 + boundX1 && x < XPosP1 + boundX2 && y > YPosP1 + boundY1 && y < YPosP1 + boundY2;
                if (!processObjectFlag[objectEntityPos]) {
                    processObjectFlag[objectEntityPos] = x > XPosP2 + boundX1 && x < XPosP2 + boundX2 && y > YPosP2 + boundY1 && y < YPosP2 + boundY2;
                }
                break;

            case PRIORITY_ACTIVE:
            case PRIORITY_ALWAYS:
            case PRIORITY_ACTIVE_SMALL: processObjectFlag[objectEntityPos] = true; break;

            case PRIORITY_XBOUNDS:
                processObjectFlag[objectEntityPos] = x > XPosP1 + boundX1 && x < XPosP1 + boundX2;
                if (!processObjectFlag[objectEntityPos]) {
                    processObjectFlag[objectEntityPos] = x > XPosP2 + boundX1 && x < XPosP2 + boundX2;
                }
                break;

            case PRIORITY_XBOUNDS_DESTROY:
                processObjectFlag[objectEntityPos] = x > XPosP1 + boundX1 && x < XPosP1 + boundX2;
                if (!processObjectFlag[objectEntityPos]) {
                    processObjectFlag[objectEntityPos] = x > XPosP2 + boundX1 && x < XPosP2 + boundX2;
                }

                if (!processObjectFlag[objectEntityPos])
                    entity->type = OBJ_TYPE_BLANKOBJECT;
                break;

            case PRIORITY_INACTIVE: processObjectFlag[objectEntityPos] = false; break;
            case PRIORITY_BOUNDS_SMALL:
                processObjectFlag[objectEntityPos] = x > XPosP1 + boundX3 && x < XPosP1 + boundX4 && y > YPosP1 + boundY3 && y < YPosP1 + boundY4;
                if (!processObjectFlag[objectEntityPos]) {
                    processObjectFlag[objectEntityPos] = x > XPosP2 + boundX3 && x < XPosP2 + boundX4 && y > YPosP2 + boundY3 && y < YPosP2 + boundY4;
                }
                break;

            default: break;
        }

        if (processObjectFlag[objectEntityPos] && entity->type > OBJ_TYPE_BLANKOBJECT) {
            ObjectScript *scriptInfo = &objectScriptList[entity->type];
            if (scriptCode[scriptInfo->eventUpdate.scriptCodePtr] > 0)
                ProcessScript(scriptInfo->eventUpdate.scriptCodePtr, scriptInfo->eventUpdate.jumpTablePtr, EVENT_MAIN);

            if (entity->drawOrder < DRAWLAYER_COUNT && entity->drawOrder >= 0)
                drawListEntries[entity->drawOrder].entityRefs[drawListEntries[entity->drawOrder].listSize++] = objectEntityPos;
        }
    }

#if RETRO_PLATFORM == RETRO_PS1
    PS1BuildTypeGroups();
#else
    for (int i = 0; i < TYPEGROUP_COUNT; ++i) objectTypeGroupList[i].listSize = 0;

    for (objectEntityPos = 0; objectEntityPos < ENTITY_COUNT; ++objectEntityPos) {
        Entity *entity = &objectEntityList[objectEntityPos];
        if (processObjectFlag[objectEntityPos] && entity->objectInteractions) {
            // Custom Group
            if (entity->groupID >= OBJECT_COUNT) {
                TypeGroupList *listCustom                      = &objectTypeGroupList[objectEntityList[objectEntityPos].groupID];
                listCustom->entityRefs[listCustom->listSize++] = objectEntityPos;
            }

            // Type-Specific list
            TypeGroupList *listType                    = &objectTypeGroupList[objectEntityList[objectEntityPos].type];
            listType->entityRefs[listType->listSize++] = objectEntityPos;

            // All Entities list
            TypeGroupList *listAll                   = &objectTypeGroupList[GROUP_ALL];
            listAll->entityRefs[listAll->listSize++] = objectEntityPos;
        }
    }
#endif
}
#endif

void SetObjectTypeName(const char *objectName, int objectID)
{
    int objPos  = 0;
    int typePos = 0;
    while (objectName[objPos]) {
        if (objectName[objPos] != ' ')
            typeNames[objectID][typePos++] = objectName[objPos];
        ++objPos;
    }
    typeNames[objectID][typePos] = 0;
    PrintLog("Set Object (%d) name to: %s", objectID, objectName);
}

void ProcessObjectControl(Entity *entity)
{
    if (entity->controlMode == 0) {
        entity->up   = keyDown.up;
        entity->down = keyDown.down;
        if (!keyDown.left || !keyDown.right) {
            entity->left  = keyDown.left;
            entity->right = keyDown.right;
        }
        else {
            entity->left  = false;
            entity->right = false;
        }
        entity->jumpHold  = keyDown.C || keyDown.B || keyDown.A;
        entity->jumpPress = keyPress.C || keyPress.B || keyPress.A;
    }
}

void InitNativeObjectSystem()
{
    InitLocalizedStrings();

    nativeEntityCount = 0;
    memset(activeEntityList, 0, sizeof(activeEntityList));
    memset(objectRemoveFlag, 0, sizeof(objectRemoveFlag));
    memset(objectEntityBank, 0, sizeof(objectEntityBank));

    nativeEntityCountBackup = 0;
    memset(backupEntityList, 0, sizeof(backupEntityList));
    memset(objectEntityBackup, 0, sizeof(objectEntityBackup));

    nativeEntityCountBackupS = 0;
    memset(backupEntityListS, 0, sizeof(backupEntityListS));
    memset(objectEntityBackupS, 0, sizeof(objectEntityBackupS));

    ReadSaveRAMData();

    SaveGame *saveGame = (SaveGame *)saveRAM;
    if (!saveGame->saveInitialized) {
        saveGame->saveInitialized = true;
        saveGame->musVolume       = MAX_VOLUME;
        saveGame->sfxVolume       = MAX_VOLUME;
        saveGame->spindashEnabled = true;
        saveGame->boxRegion       = 0;
        saveGame->vDPadSize       = 64;
        saveGame->vDPadOpacity    = 160;
        saveGame->vDPadX_Move     = 56;
        saveGame->vDPadY_Move     = 184;
        saveGame->vDPadX_Jump     = -56;
        saveGame->vDPadY_Jump     = 188;
        saveGame->tailsUnlocked   = Engine.gameType != GAME_SONIC1;
        saveGame->knuxUnlocked    = Engine.gameType != GAME_SONIC1;
        saveGame->unlockedActs    = 0;
#if RETRO_PLATFORM != RETRO_PS1 // PS1: no card write at boot; the first real save (a file, options, a record) creates it
        WriteSaveRAMData();
#endif
    }
#if !RETRO_USE_ORIGINAL_CODE
    else if (Engine.gameType == GAME_SONIC2) {
        // ensure tails and knuckles are unlocked in sonic 2
        // they weren't automatically unlocked in older versions of the decomp
        saveGame->tailsUnlocked = true;
        saveGame->knuxUnlocked  = true;
#if RETRO_PLATFORM != RETRO_PS1
        WriteSaveRAMData();
#endif
    }
#endif
    saveGame->musVolume = bgmVolume;
    saveGame->sfxVolume = sfxVolume;

    if (!saveGame->musVolume)
        musicEnabled = false;

    if (!saveGame->vDPadX_Move) {
        saveGame->vDPadX_Move = 60;
        saveGame->vDPadY_Move = 176;
        saveGame->vDPadX_Jump = -56;
        saveGame->vDPadY_Jump = 180;
    }

    Engine.globalBoxRegion = saveGame->boxRegion;
    SetGameVolumes(saveGame->musVolume, saveGame->sfxVolume);
#if RETRO_PLATFORM == RETRO_PS1
    // No native objects on PS1: RetroEngine::RunFrame does RetroGameLoop's work (skipStartMenu path).
#else
#if !RETRO_USE_ORIGINAL_CODE
    if (skipStartMenu) {
        CREATE_ENTITY(RetroGameLoop);
        if (Engine.gameDeviceType == RETRO_MOBILE)
            CREATE_ENTITY(VirtualDPad);
    }
    else
#endif
        CREATE_ENTITY(SegaSplash);
#endif
}
NativeEntity *CreateNativeObject(void (*create)(void *objPtr), void (*main)(void *objPtr))
{
    if (!nativeEntityCount) {
        memset(objectEntityBank, 0, sizeof(objectEntityBank));
        NativeEntity *entity = &objectEntityBank[0];
        entity->eventCreate  = create;
        entity->eventMain    = main;
        activeEntityList[0]  = 0;
        nativeEntityCount++;
        if (entity->eventCreate)
            entity->eventCreate(entity);
        return entity;
    }
    else if (nativeEntityCount >= NATIVEENTITY_COUNT) {
        // TODO, probably never
        return NULL;
    }
    else {
        int slot = 0;
        for (; slot < NATIVEENTITY_COUNT; ++slot) {
            if (!objectEntityBank[slot].eventMain)
                break;
        }
        NativeEntity *entity = &objectEntityBank[slot];
        memset(entity, 0, sizeof(NativeEntity));
        entity->slotID                        = slot;
        entity->objectID                      = nativeEntityCount;
        entity->eventCreate                   = create;
        entity->eventMain                     = main;
        activeEntityList[nativeEntityCount++] = slot;
        if (entity->eventCreate)
            entity->eventCreate(entity);
        return entity;
    }
}
void RemoveNativeObject(NativeEntityBase *entity)
{
#if !RETRO_USE_ORIGINAL_CODE
    if (!entity)
        return;
    memmove(&activeEntityList[entity->objectID], &activeEntityList[entity->objectID + 1], sizeof(int) * (NATIVEENTITY_COUNT - (entity->objectID + 2)));
    --nativeEntityCount;
    for (int i = entity->slotID; objectEntityBank[i].eventMain; ++i) objectEntityBank[i].objectID--;
#else
    // this actually behaves COMPLETELY improperly, duplicating the deleted one instead
    // the above code is my attempt to make a proper version
    if (nativeEntityCount <= 0) {
        objectRemoveFlag[entity->slotID] = true;
    }
    else {
        memset(objectRemoveFlag, 0, nativeEntityCount);
        int slotStore                    = 0;
        objectRemoveFlag[entity->slotID] = true;
        int s                            = 0;
        do {
            if (!objectRemoveFlag[s]) {
                if (s != slotStore) {
                    int store                   = activeEntityList[s];
                    objectRemoveFlag[slotStore] = false;
                    activeEntityList[slotStore] = store;
                }
                ++slotStore;
            }
            ++s;
        } while (s != nativeEntityCount);
        nativeEntityCount = s - 1;
    }
#endif
}
void ResetNativeObject(NativeEntityBase *obj, void (*create)(void *objPtr), void (*main)(void *objPtr))
{
    int slotID = obj->slotID;
    int objID  = obj->objectID;
    memset(&objectEntityBank[slotID], 0, sizeof(NativeEntity));
    obj->slotID      = slotID;
    obj->eventMain   = main;
    obj->eventCreate = create;
    obj->objectID    = objID;
    if (create)
        create(obj);
}
void ProcessNativeObjects()
{
    ResetRenderStates();
    for (nativeEntityPos = 0; nativeEntityPos < nativeEntityCount; ++nativeEntityPos) {
        NativeEntity *entity = &objectEntityBank[activeEntityList[nativeEntityPos]];
        entity->eventMain(entity);
    }
    RenderScene();
}

void RestoreNativeObjects()
{
    memcpy(activeEntityList, backupEntityList, sizeof(activeEntityList));
    memcpy(objectEntityBank, objectEntityBackup, sizeof(objectEntityBank));
    nativeEntityCount = nativeEntityCountBackup;

    CREATE_ENTITY(FadeScreen)->state = FADESCREEN_STATE_MENUFADEIN;
}

void RestoreNativeObjectsNoFade()
{
    memcpy(activeEntityList, backupEntityList, sizeof(activeEntityList));
    memcpy(objectEntityBank, objectEntityBackup, sizeof(objectEntityBank));
    nativeEntityCount = nativeEntityCountBackup;
}
void RestoreNativeObjectsSettings()
{
    memcpy(activeEntityList, backupEntityListS, sizeof(activeEntityList));
    memcpy(objectEntityBank, objectEntityBackupS, sizeof(objectEntityBank));
    nativeEntityCount = nativeEntityCountBackupS;
}
