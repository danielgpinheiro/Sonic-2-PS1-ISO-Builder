#ifndef ANIMATION_H
#define ANIMATION_H

#define ANIFILE_COUNT     (0x100)
#if RETRO_PLATFORM == RETRO_PS1
#define ANIMATION_COUNT (0x100) // PS1: the five .ani files hold 161 animations (guarded: g_ps1AnimFrameOverflow)
#else
#define ANIMATION_COUNT   (0x400)
#endif
#if RETRO_PLATFORM == RETRO_PS1
// PS1: script frames <= 756 per stage (Zone02, every branch counted) and .ani frames 872 with all four
// characters' files loaded (docs/30 phase 2). LoadAnimationFile past it is counted (g_ps1AnimFrameOverflow).
#define SPRITEFRAME_COUNT (0x400)
#else
#define SPRITEFRAME_COUNT (0x1000)
#endif

#define HITBOX_COUNT     (0x20)
#define HITBOX_DIR_COUNT (0x8)

enum AnimRotationFlags { ROTSTYLE_NONE, ROTSTYLE_FULL, ROTSTYLE_45DEG, ROTSTYLE_STATICFRAMES };

struct AnimationFile {
    char fileName[0x20];
    int animCount;
    int aniListOffset;
    int hitboxListOffset;
};

struct SpriteAnimation {
    char name[16];
    byte frameCount;
    byte speed;
    byte loopPoint;
    byte rotationStyle;
    int frameListOffset;
};

struct SpriteFrame {
#if RETRO_PLATFORM == RETRO_PS1 // PS1: sheet coordinates, sizes and pivots all fit 16 bits (halves 2 x 28 KB)
    short sprX;
    short sprY;
    short width;
    short height;
    short pivotX;
    short pivotY;
#else
    int sprX;
    int sprY;
    int width;
    int height;
    int pivotX;
    int pivotY;
#endif
    byte sheetID;
    byte hitboxID;
};

struct Hitbox {
    sbyte left[HITBOX_DIR_COUNT];
    sbyte top[HITBOX_DIR_COUNT];
    sbyte right[HITBOX_DIR_COUNT];
    sbyte bottom[HITBOX_DIR_COUNT];
};

extern AnimationFile animationFileList[ANIFILE_COUNT];
extern int animationFileCount;

extern SpriteFrame scriptFrames[SPRITEFRAME_COUNT];
extern int scriptFrameCount;

extern SpriteFrame animFrames[SPRITEFRAME_COUNT];
extern int animFrameCount;
extern SpriteAnimation animationList[ANIMATION_COUNT];
extern int animationCount;
extern Hitbox hitboxList[HITBOX_COUNT];
extern int hitboxCount;

void LoadAnimationFile(char *filePath);
void ClearAnimationData();

AnimationFile *AddAnimationFile(char *filePath);

inline AnimationFile *GetDefaultAnimationRef() { return &animationFileList[0]; }

void ProcessObjectAnimation(void *objScr, void *ent);

#endif // !ANIMATION_H
