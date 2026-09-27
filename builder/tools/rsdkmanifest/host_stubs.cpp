/*
 * Stubs for engine symbols that Scene.cpp / Script.cpp / Object.cpp / Animation.cpp / Sprite.cpp reference but the
 * manifest tool never needs (drawing, audio, collision, 3D, menus, save data, PS1 timing). Generated from the
 * linker's undefined-symbol list + the engine headers' declarations (regenerate if the engine changes).
 */
#include <stdarg.h>
#include "RetroEngine.hpp"

int AddDebugHitbox(byte type, Entity *entity, int left, int top, int right, int bottom) { return {}; }
void AddTextMenuEntry(TextMenu *menu, const char *text) {}
void BoxCollision(Entity *thisEntity, int thisLeft, int thisTop, int thisRight, int thisBottom, Entity *otherEntity, int otherLeft, int otherTop,
                  int otherRight, int otherBottom) {}
void BoxCollision2(Entity *thisEntity, int thisLeft, int thisTop, int thisRight, int thisBottom, Entity *otherEntity, int otherLeft, int otherTop,
                   int otherRight, int otherBottom) {}
void CheckKeyDown(InputData *input) {}
void CheckKeyPress(InputData *input) {}
void ClearScreen(byte index) {}
void Draw3DScene(int spriteSheetID) {}
void DrawAdditiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}
void DrawAlphaBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}
void DrawBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {}
void DrawDebugOverlays() {}
void DrawObjectAnimation(void *objScr, void *ent, int XPos, int YPos) {}
void DrawObjectList(int layer) {}
void DrawRectangle(int XPos, int YPos, int width, int height, int R, int G, int B, int A) {}
void DrawScaledTintMask(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                        int sheetID) {}
void DrawSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int sheetID) {}
void DrawSpriteFlipped(int XPos, int YPos, int width, int height, int sprX, int sprY, int direction, int sheetID) {}
void DrawSpriteRotated(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation,
                       int sheetID) {}
void DrawSpriteRotozoom(int direction, int XPos, int YPos, int pivotX, int pivotY, int sprX, int sprY, int width, int height, int rotation, int scale,
                        int sheetID) {}
void DrawSpriteScaled(int direction, int XPos, int YPos, int pivotX, int pivotY, int scaleX, int scaleY, int width, int height, int sprX, int sprY,
                      int sheetID) {}
void DrawStageGFX() {}
void DrawSubtractiveBlendedSprite(int XPos, int YPos, int width, int height, int sprX, int sprY, int alpha, int sheetID) {}
void DrawTextMenu(void *menu, int XPos, int YPos) {}
void DrawTintRectangle(int XPos, int YPos, int width, int height) {}
void EditTextMenuEntry(TextMenu *menu, const char *text, int rowID) {}
void FadeScreen_Create(void *objPtr) {}
void FadeScreen_Main(void *objPtr) {}
void LoadPalette(const char *filePath, int paletteID, int startPaletteIndex, int startIndex, int endIndex) {}
void LoadSfx(char *filePath, byte sfxID) {}
void LoadTextFile(TextMenu *menu, const char *filePath, byte mapCode) {}
void MatrixInverse(Matrix *matrix) {}
void MatrixMultiply(Matrix *matrixA, Matrix *matrixB) {}
void MatrixRotateX(Matrix *matrix, int rotationX) {}
void MatrixRotateXYZ(Matrix *matrix, short rotationX, short rotationY, short rotationZ) {}
void MatrixRotateY(Matrix *matrix, int rotationY) {}
void MatrixRotateZ(Matrix *matrix, int rotationZ) {}
void MatrixScaleXYZ(Matrix *matrix, int scaleX, int scaleY, int scaleZ) {}
void MatrixTranslateXYZ(Matrix *Matrix, int x, int y, int z) {}
void ObjectFloorCollision(int xOffset, int yOffset, int cPath) {}
void ObjectFloorGrip(int xOffset, int yOffset, int cPath) {}
void ObjectLEntityGrip(int xOffset, int yOffset, int cPath) {}
void ObjectLWallCollision(int xOffset, int yOffset, int cPath) {}
void ObjectLWallGrip(int xOffset, int yOffset, int cPath) {}
void ObjectREntityGrip(int xOffset, int yOffset, int cPath) {}
void ObjectRWallCollision(int xOffset, int yOffset, int cPath) {}
void ObjectRWallGrip(int xOffset, int yOffset, int cPath) {}
void ObjectRoofCollision(int xOffset, int yOffset, int cPath) {}
void ObjectRoofGrip(int xOffset, int yOffset, int cPath) {}
void PlatformCollision(Entity *thisEntity, int thisLeft, int thisTop, int thisRight, int thisBottom, Entity *otherEntity, int otherLeft, int otherTop,
                       int otherRight, int otherBottom) {}
bool PlayMusic(int track, int musStartPos) { return {}; }
void PlaySfx(int sfx, bool loop) {}
void PrintLog(const char *msg, ...) {}
void ProcessTileCollisions(Entity *player) {}
bool ReadSaveRAMData() { return {}; }
void RenderScene() {}
void ResetRenderStates() {}
void SetIdentityMatrix(Matrix *matrix) {}
void SetMusicTrack(const char *filePath, byte trackID, bool loop, uint loopPoint) {}
void SetPaletteFade(byte destPaletteID, byte srcPaletteA, byte srcPaletteB, ushort blendAmount, int startIndex, int endIndex) {}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan) {}
void SetSfxName(const char *sfxName, int sfxID) {}
void PS1SfxChannelStop(int channel) {}
void PS1SfxReleaseStage() {}
void PS1SfxLoadStageBank() {}
void PS1SfxLoadMenuBank() {}
void PS1MusicStop() {}
void PS1MusicPause(bool pause) {}
int currentStreamIndex = 0; // FreeMusInfo (Audio.hpp, PS1)
StreamInfo streamInfo[STREAMFILE_COUNT];
void SetupTextMenu(TextMenu *menu, int rowCount) {}
void Sort3DDrawList() {}
void SwapMusicTrack(const char *filePath, byte trackID, uint loopPoint, uint ratio) {}
void TouchCollision(Entity *thisEntity, int thisLeft, int thisTop, int thisRight, int thisBottom, Entity *otherEntity, int otherLeft, int otherTop,
                    int otherRight, int otherBottom) {}
void TransformVertexBuffer() {}
void TransformVertices(Matrix *matrix, int startIndex, int endIndex) {}
bool WriteSaveRAMData() { return {}; }
decltype(Engine) Engine{};
decltype(SCREEN_CENTERX) SCREEN_CENTERX{};
decltype(SCREEN_XSIZE) SCREEN_XSIZE{};
decltype(activePalette) activePalette{};
decltype(activePalette32) activePalette32{};
decltype(bgmVolume) bgmVolume{};
decltype(debugHitboxCount) debugHitboxCount{};
decltype(drawListEntries) drawListEntries{};
decltype(endLine) endLine{};
static Face s_hostFaces[FACEBUFFER_SIZE]; // the host build keeps the Scene3D arrays (startups write vertices)
Face *faceBuffer = s_hostFaces;
int g_ps1VtxCap = VERTEXBUFFER_SIZE, g_ps1FaceCap = FACEBUFFER_SIZE;
void PS1Scene3DPrepare(const char *) {}
void PS1FsSetLoadFolder(const char *) {}
decltype(faceCount) faceCount{};
decltype(fadeA) fadeA{};
decltype(fadeB) fadeB{};
decltype(fadeG) fadeG{};
decltype(fadeMode) fadeMode{};
decltype(fadeR) fadeR{};
decltype(fogColor) fogColor{};
decltype(fogStrength) fogStrength{};
decltype(forceUseScripts) forceUseScripts{};
decltype(fullPalette) fullPalette{};
decltype(fullPalette32) fullPalette32{};
decltype(gameMenu) gameMenu{};
decltype(gfxDataPosition) gfxDataPosition{};
decltype(gfxLineBuffer) gfxLineBuffer{};
decltype(gfxSurface) gfxSurface{};
decltype(globalSFXCount) globalSFXCount{};
decltype(globalVariableNames) globalVariableNames{};
decltype(globalVariables) globalVariables{};
decltype(globalVariablesCount) globalVariablesCount{};
decltype(keyDown) keyDown{};
decltype(keyPress) keyPress{};
decltype(masterVolume) masterVolume{};
decltype(matTemp) matTemp{};
decltype(matView) matView{};
decltype(matWorld) matWorld{};
decltype(musicEnabled) musicEnabled{};
decltype(musicPosition) musicPosition{};
decltype(musicStatus) musicStatus{};
decltype(nativeFunction) nativeFunction{};
decltype(playerNames) playerNames{};
decltype(projectionX) projectionX{};
decltype(projectionY) projectionY{};
decltype(saveRAM) saveRAM{};
decltype(sfxChannels) sfxChannels{};
decltype(sfxList) sfxList{};
decltype(sfxNames) sfxNames{};
decltype(sfxVolume) sfxVolume{};
decltype(stageSFXCount) stageSFXCount{};
decltype(textMenuSurfaceNo) textMenuSurfaceNo{};
decltype(touchDown) touchDown{};
decltype(touchX) touchX{};
decltype(touchY) touchY{};
decltype(touches) touches{};
decltype(trackID) trackID{};
static Vertex s_hostVertices[VERTEXBUFFER_SIZE];
Vertex *vertexBuffer = s_hostVertices;
decltype(vertexCount) vertexCount{};

// PS1 platform hooks (ps1/video.hh, ps1/loading.hh) the engine calls: timing and the loading icon.
uint32_t PS1Hblanks() { return 0; }
uint32_t PS1HblanksSince(uint32_t) { return 0; }
void PS1LoadingBegin() {}
void PS1LoadingEnd() {}
// Stage-load VRAM uploads (ps1/tileset.hh, ps1/sprite_atlas.hh): no VRAM here; the manifest is what builds atlases.
bool PS1TileSetLoad(const char *, uint8_t *) { return false; }
bool PS1AtlasLoad(const char *, int) { return false; }
// Renderer hooks (ps1/render.cpp) the engine calls: nothing to draw here.
volatile uint32_t g_ps1SurfaceGen = 0;
void PS1BuildTileVisuals() {}
void PS1TileChanged(int) {}
void PS1Copy16x16Tile(int, int) {}
void PS1LoadBGStrips(const char *, int) {}
void PS1PaletteLinesChanged() {}
volatile uint32_t g_ps1SaveRAMClamped = 0;
