/*
 * rsdkmanifest — host-side sprite manifest for the RSDKv4 (Sonic 2) PS1 port (docs/30 phase 3.1), after the Sonic CD
 * port's tool (RSDKv3-ps1/tools/rsdkmanifest).
 *
 * Sonic 2 ships its scripts as bytecode only, so the frames a stage can draw come from running them: for every stage
 * of the GameConfig stage lists and every player variant this tool runs the engine's own LoadStageFiles
 * (RSDKv4/Scene.cpp: global + stage bytecode, act layout, ProcessStartupObjects; SpriteFrame records frames only
 * there) with the PS1 configuration, and writes what the console will need in VRAM:
 *
 *   <out>/<folder>.txt   (list positions sharing a folder, and both options.region values, append their own `stage` block)
 *     stage <list> <position> <folder> <id> <playerListPos> <options.region>
 *     sheet <sheetID> <path under Data/Sprites/>
 *     frame <sheetID> <x> <y> <w> <h> obj <object name>          script frames (the object's final sheet)
 *     frame <sheetID> <x> <y> <w> <h> ani <file>                  animation (.ani) frames
 *
 * Also loaded, as the scripts do later: SuperSonic.ani (Sonic's transformation, the Ending). Frames scripts change at
 * runtime (EditFrame with variables) are not here: tools/atlas adds their regions.
 * Sheets are only registered (Sprite.cpp AddGraphicsFile: names), never decoded.
 * Usage: rsdkmanifest <disc tree: build/iso, with Data/ and Bytecode/> <out dir> [player list positions, default 0]
 *        (0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails)
 */
#include <set>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "RetroEngine.hpp"

extern volatile uint32_t g_ps1ScriptOverflow;   // Script.cpp (PS1 array guards)
extern volatile uint32_t g_ps1ScriptBadOpcode;

static std::string readStr()
{
    byte len = 0;
    FileRead(&len, 1);
    char buf[0x100];
    FileRead(buf, len);
    buf[len] = 0;
    return buf;
}

// RetroEngine::LoadGameConfig (RetroEngine.cpp): the parts stage loading depends on (global variables' initial values,
// player names, the stage lists with the special / bonus swap).
static bool loadGameConfig()
{
    FileInfo info;
    if (!LoadFile("Data/Game/GameConfig.bin", &info))
        return false;
    readStr(), readStr(); // window text, description
    byte rgb[3];
    for (int c = 0; c < 0x60; ++c) {
        FileRead(rgb, 3);
        SetPaletteEntry(-1, c, rgb[0], rgb[1], rgb[2]);
    }
    byte n = 0;
    FileRead(&n, 1);
    for (int i = 0; i < n; ++i) readStr(); // object names (LoadStageFiles reads them again)
    for (int i = 0; i < n; ++i) readStr(); // script paths
    FileRead(&n, 1);
    globalVariablesCount = n;
    for (int v = 0; v < n; ++v) {
        std::string name = readStr();
        StrCopy(globalVariableNames[v], name.c_str());
        byte b[4];
        FileRead(b, 4);
        globalVariables[v] = b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24);
    }
    FileRead(&n, 1);
    for (int i = 0; i < n * 2; ++i) readStr(); // sfx names, paths
    globalSFXCount = n;
    FileRead(&n, 1);
    for (int p = 0; p < n; ++p) StrCopy(playerNames[p], readStr().c_str());
    for (int c = 0; c < 4; ++c) {
        int cat     = c == 2 ? 3 : c == 3 ? 2 : c;
        byte count  = 0;
        FileRead(&count, 1);
        stageListCount[cat] = count;
        for (int s = 0; s < count; ++s) {
            SceneInfo scratch;
            SceneInfo *si = s < STAGELIST_ENTRY_COUNT ? &stageList[cat][s] : &scratch;
            StrCopy(si->folder, readStr().c_str());
            StrCopy(si->id, readStr().c_str());
            StrCopy(si->name, readStr().c_str());
            byte hl = 0;
            FileRead(&hl, 1);
            si->highlighted = hl;
        }
        if (stageListCount[cat] > STAGELIST_ENTRY_COUNT)
            stageListCount[cat] = STAGELIST_ENTRY_COUNT;
    }
    CloseFile();
    return true;
}

static void setGlobal(const char *name, int value) // SetGlobalVariableByName (RetroEngine.cpp)
{
    for (int v = 0; v < globalVariablesCount; ++v)
        if (StrComp(name, globalVariableNames[v]))
            globalVariables[v] = value;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: rsdkmanifest <root containing Data/ and Bytecode/> <out dir> [player list positions]\n");
        return 1;
    }
    std::vector<int> players;
    for (int a = 3; a < argc; ++a) players.push_back(atoi(argv[a]));
    if (players.empty())
        players.push_back(0);
    std::string out = argv[2];
    char cwd[0x400];
    if (out[0] != '/' && getcwd(cwd, sizeof(cwd)))
        out = std::string(cwd) + "/" + out;
    for (size_t i = 1; i <= out.size(); ++i) // mkdir -p
        if (i == out.size() || out[i] == '/')
            mkdir(out.substr(0, i).c_str(), 0755);
    if (chdir(argv[1])) {
        perror(argv[1]);
        return 1;
    }
    activePalette   = fullPalette[0]; // the engine points these at palette bank 0 at init
    activePalette32 = fullPalette32[0];
    Engine.usingBytecode = true;
    if (!loadGameConfig()) {
        fprintf(stderr, "no Data/Game/GameConfig.bin\n");
        return 1;
    }
    int stages = 0;
    std::set<std::string> written; // list positions sharing a folder: append (the atlas takes the union)
    for (int list = 0; list < STAGELIST_MAX; ++list) {
        for (int pos = 0; pos < stageListCount[list]; ++pos) {
            char bytecode[0x80]; // stages left off the disc (Egg Gauntlet, docs/30) have no bytecode there: skip
            snprintf(bytecode, sizeof(bytecode), "Bytecode/%s.bin", stageList[list][pos].folder);
            if (access(bytecode, R_OK)) {
                printf("skip  %-8s (no %s)\n", stageList[list][pos].folder, bytecode);
                continue;
            }
            // Each variant with options.region 0 and 1 (the Title's code toggles it at runtime; startups pick frames
            // by it, e.g. Act Finish / Special Finish "TAILS" vs "MILES"), Sonic & Tails also as the later loads see it:
            // the Player Object's startup turns playerListPos 3 into 0 + stage.player2Enabled. The atlas takes the union.
            std::vector<std::pair<int, int>> runs; // (variant, playerListPos)
            for (int player : players) {
                runs.push_back({player, player});
                if (player == 3)
                    runs.push_back({player, 0});
            }
            for (int run = 0; run < (int)runs.size() * 2; ++run) {
                int player = runs[run >> 1].first, region = run & 1;
                setGlobal("options.region", region);
                activeStageList   = list;
                stageListPosition = pos;
                playerListPos     = runs[run >> 1].second;
                setGlobal("stage.player2Enabled", player == 3); // as the PS1 boot (main.cpp) / the save-file start do
                stageMode         = STAGEMODE_LOAD;
                StrCopy(currentStageFolder, ""); // force a full load (CheckCurrentStageFolder)
                uint32_t overflow = g_ps1ScriptOverflow, bad = g_ps1ScriptBadOpcode;
                LoadStageFiles();
                if (g_ps1ScriptOverflow != overflow || g_ps1ScriptBadOpcode != bad) {
                    fprintf(stderr, "ERROR %s: bytecode does not fit the PS1 arrays or has a bad opcode\n", stageList[list][pos].folder);
                    return 1;
                }
                // Loaded after the startups by the scripts themselves: Super Sonic (GlobalCode function 56, when Sonic
                // transforms: every stage with the Player Object and Sonic; the Ending's setup always).
                bool hasPlayer = false;
                for (int o = 0; o < OBJECT_COUNT; ++o) hasPlayer |= StrComp(typeNames[o], "Player Object");
                if ((hasPlayer && (playerListPos == 0 || playerListPos == 3)) || StrComp(stageList[list][pos].folder, "Ending"))
                    AddAnimationFile("SuperSonic.ani");
                if (!written.count(out + "/" + stageList[list][pos].folder + ".txt")) {
                    // docs/30 phase 7.3: what LoadStageChunks / LoadStageCollisions left (the PS1 arrays, this same
                    // code) as 128x128Tiles.ps1 / CollisionMasks.ps1 beside the originals: the console reads them in.
                    auto pack = [&](const char *bin, const char *name, const char *tag, const void *data, size_t size) {
                        std::string dir = std::string("Data/Stages/") + stageList[list][pos].folder + "/";
                        if (access((dir + bin).c_str(), R_OK))
                            return;
                        FILE *pf = fopen((dir + name).c_str(), "wb");
                        if (!pf || fwrite(tag, 1, 4, pf) != 4 || fwrite(data, 1, size, pf) != size) {
                            perror((dir + name).c_str());
                            exit(1);
                        }
                        fclose(pf);
                    };
                    // up to the last non-zero byte (docs/30 9.3: the special stage's 96 KB are 92 KB of zeros); the
                    // console zero-fills the rest (RSDKv4/Scene.cpp PS1LoadStagePacked)
                    size_t ctSize = sizeof(tiles128x128.ps1Tile) + sizeof(tiles128x128.ps1Collision);
                    const uint8_t *ct = (const uint8_t *)tiles128x128.ps1Tile;
                    while (ctSize > 4 && !ct[ctSize - 1]) --ctSize;
                    ctSize = (ctSize + 3) & ~(size_t)3;
                    pack("128x128Tiles.bin", "128x128Tiles.ps1", "PCT1", tiles128x128.ps1Tile, ctSize);
                    // Path B as the tiles where it differs from path A ("PCM2", RSDKv4/Scene.cpp PS1LoadCollisionMasks):
                    // raw both paths ("PCM1") only if that were smaller.
                    std::vector<uint8_t> diff;
                    uint32_t n = 0;
                    const CollisionMasks &A = collisionMasks[0], &B = collisionMasks[1];
                    for (int t = 0; t < TILE_COUNT; ++t) {
                        bool same = !memcmp(&A.floorMasks[t * TILE_SIZE], &B.floorMasks[t * TILE_SIZE], TILE_SIZE)
                                    && !memcmp(&A.lWallMasks[t * TILE_SIZE], &B.lWallMasks[t * TILE_SIZE], TILE_SIZE)
                                    && !memcmp(&A.rWallMasks[t * TILE_SIZE], &B.rWallMasks[t * TILE_SIZE], TILE_SIZE)
                                    && !memcmp(&A.roofMasks[t * TILE_SIZE], &B.roofMasks[t * TILE_SIZE], TILE_SIZE)
                                    && A.angles[t] == B.angles[t] && A.flags[t] == B.flags[t];
                        if (same)
                            continue;
                        ++n;
                        diff.push_back(t & 0xFF), diff.push_back(t >> 8);
                        for (const sbyte *m : { B.floorMasks, B.lWallMasks, B.rWallMasks, B.roofMasks })
                            diff.insert(diff.end(), (const uint8_t *)&m[t * TILE_SIZE], (const uint8_t *)&m[t * TILE_SIZE] + TILE_SIZE);
                        diff.insert(diff.end(), (const uint8_t *)&B.angles[t], (const uint8_t *)&B.angles[t] + 4);
                        diff.push_back(B.flags[t]);
                    }
                    if (4 + diff.size() < sizeof(CollisionMasks)) {
                        std::vector<uint8_t> blob((const uint8_t *)&A, (const uint8_t *)&A + sizeof(CollisionMasks));
                        blob.insert(blob.end(), (const uint8_t *)&n, (const uint8_t *)&n + 4);
                        blob.insert(blob.end(), diff.begin(), diff.end());
                        pack("CollisionMasks.bin", "CollisionMasks.ps1", "PCM2", blob.data(), blob.size());
                        // self-check: path B rebuilt as the console does (copy of A, the records patched) == the engine's
                        static CollisionMasks rb;
                        memcpy(&rb, &A, sizeof(rb));
                        for (size_t o = 0; o < diff.size(); o += 2 + 4 * TILE_SIZE + 5) {
                            int t = diff[o] | diff[o + 1] << 8;
                            memcpy(&rb.floorMasks[t * TILE_SIZE], &diff[o + 2], TILE_SIZE);
                            memcpy(&rb.lWallMasks[t * TILE_SIZE], &diff[o + 2 + TILE_SIZE], TILE_SIZE);
                            memcpy(&rb.rWallMasks[t * TILE_SIZE], &diff[o + 2 + 2 * TILE_SIZE], TILE_SIZE);
                            memcpy(&rb.roofMasks[t * TILE_SIZE], &diff[o + 2 + 3 * TILE_SIZE], TILE_SIZE);
                            memcpy(&rb.angles[t], &diff[o + 2 + 4 * TILE_SIZE], 4);
                            rb.flags[t] = diff[o + 2 + 4 * TILE_SIZE + 4];
                        }
                        if (memcmp(&rb, &B, sizeof(rb))) {
                            fprintf(stderr, "ERROR %s: CollisionMasks PCM2 does not rebuild path B\n", stageList[list][pos].folder);
                            return 1;
                        }
                    }
                    if (const char *dd = getenv("PS1_MASK_DUMP_DIR")) { // raw arrays for the console A/B (docs/30 9.3)
                        std::string dp = std::string(dd) + "/" + stageList[list][pos].folder + ".masks";
                        if (FILE *df = fopen(dp.c_str(), "wb")) {
                            fwrite(collisionMasks, 1, sizeof(collisionMasks), df);
                            fclose(df);
                        }
                    }
                    else
                        pack("CollisionMasks.bin", "CollisionMasks.ps1", "PCM1", collisionMasks, sizeof(collisionMasks));
                }
                std::string path = out + "/" + stageList[list][pos].folder + ".txt";
                FILE *f          = fopen(path.c_str(), written.count(path) ? "a" : "w");
                if (!f) {
                    perror(path.c_str());
                    return 1;
                }
                written.insert(path);
                fprintf(f, "stage %d %d %s %s %d %d\n", list, pos, stageList[list][pos].folder, stageList[list][pos].id,
                        runs[run >> 1].second, region);
                for (int s = 0; s < SURFACE_COUNT; ++s)
                    if (gfxSurface[s].fileName[0])
                        fprintf(f, "sheet %d %s\n", s, gfxSurface[s].fileName + 13); // strip "Data/Sprites/"
                int frames = 0;
                for (int o = 0; o < OBJECT_COUNT; ++o) {
                    ObjectScript *os = &objectScriptList[o];
                    for (int i = 0; i < os->frameCount; ++i) {
                        SpriteFrame *fr = &scriptFrames[os->frameListOffset + i];
                        fprintf(f, "frame %d %d %d %d %d obj %s\n", os->spriteSheetID, fr->sprX, fr->sprY, fr->width, fr->height,
                                typeNames[o]);
                        ++frames;
                    }
                }
                for (int a = 0; a < animationFileCount; ++a) {
                    AnimationFile *af = &animationFileList[a];
                    for (int an = 0; an < af->animCount; ++an) {
                        SpriteAnimation *anim = &animationList[af->aniListOffset + an];
                        // ROTSTYLE_STATICFRAMES: LoadAnimationFile halves frameCount; the second half holds the
                        // pre-rotated frames DrawObjectAnimation draws as frame + frameCount.
                        int n = anim->frameCount * (anim->rotationStyle == ROTSTYLE_STATICFRAMES ? 2 : 1);
                        for (int i = 0; i < n; ++i) {
                            SpriteFrame *fr = &animFrames[anim->frameListOffset + i];
                            fprintf(f, "frame %d %d %d %d %d ani %s\n", fr->sheetID, fr->sprX, fr->sprY, fr->width, fr->height,
                                    af->fileName);
                            ++frames;
                        }
                    }
                }
                fclose(f);
                printf("%-5s %-8s p%d(%d) r%d %4d frames\n", list == 0 ? "pres" : list == 1 ? "reg" : list == 2 ? "bonus" : "spec",
                       stageList[list][pos].folder, player, runs[run >> 1].second, region, frames);
            }
            ++stages;
        }
    }
    printf("%d stages\n", stages);
    return 0;
}
