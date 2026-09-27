#ifndef AUDIO_H
#define AUDIO_H

#include <stdlib.h>

// PS1 port: no Ogg/SDL at runtime (music is CD-XA, SFX are SPU-ADPCM; docs/30 phase 5). These stand-ins
// keep the data structures and the inline helpers compiling (as in the Sonic CD port).
#if RETRO_PLATFORM == RETRO_PS1
#include <stdint.h>
typedef int16_t Sint16;
typedef int32_t Sint32;
typedef uint8_t Uint8;
typedef uint16_t Uint16;
typedef struct {
    int unused;
} OggVorbis_File;
#define ov_clear(f) ((void)0)
#endif

#define TRACK_COUNT (0x10)
#if RETRO_PLATFORM == RETRO_PS1
#define SFX_COUNT (0x60) // Sonic 2: 44 global + at most 23 stage SFX (Zone08; Egg Gauntlet is not on the disc)
#else
#define SFX_COUNT (0x100)
#endif
#if !RETRO_USE_ORIGINAL_CODE
#define CHANNEL_COUNT (0x10) // 4 in the original, 16 for convenience
#else
#define CHANNEL_COUNT (0x4)
#endif

#define MAX_VOLUME (100)

#define MUSBUFFER_SIZE   (0x200000)
#define STREAMFILE_COUNT (2)

#define MIX_BUFFER_SAMPLES (256)

#if RETRO_USING_SDL1 || RETRO_USING_SDL2

#define LockAudioDevice()   SDL_LockAudio()
#define UnlockAudioDevice() SDL_UnlockAudio()

#else
#define LockAudioDevice()   ;
#define UnlockAudioDevice() ;
#endif

struct TrackInfo {
    char fileName[0x40];
    bool trackLoop;
    uint loopPoint;
};

struct StreamInfo {
    OggVorbis_File vorbisFile;
    int vorbBitstream;
#if RETRO_USING_SDL1
    SDL_AudioSpec spec;
#endif
#if RETRO_USING_SDL2
    SDL_AudioStream *stream;
#endif
    Sint16 buffer[MIX_BUFFER_SAMPLES];
    bool trackLoop;
    uint loopPoint;
    bool loaded;
};

struct SFXInfo {
#if RETRO_PLATFORM == RETRO_PS1
    char name[0x20]; // Sonic 2's longest SFX path has 26 chars
#else
    char name[0x40];
#endif
    Sint16 *buffer;
    size_t length;
    bool loaded;
#if RETRO_PLATFORM == RETRO_PS1
    uint32_t spuAddr; // SPU-ADPCM sample in SPU RAM (the resident / stage / menu banks, tools/audio/build_sfx.py)
    uint32_t rate;
#endif
};

struct ChannelInfo {
    size_t sampleLength;
    Sint16 *samplePtr;
    int sfxID;
    byte loopSFX;
    sbyte pan;
};

struct StreamFile {
    byte buffer[MUSBUFFER_SIZE];
    int fileSize;
    int filePos;
};

enum MusicStatuses {
    MUSIC_STOPPED = 0,
    MUSIC_PLAYING = 1,
    MUSIC_PAUSED  = 2,
    MUSIC_LOADING = 3,
    MUSIC_READY   = 4,
};

extern int globalSFXCount;
extern int stageSFXCount;

extern int masterVolume;
extern int trackID;
extern int sfxVolume;
extern int bgmVolume;
extern bool audioEnabled;

extern bool musicEnabled;
extern int musicStatus;
extern int musicStartPos;
extern int musicPosition;
extern int musicRatio;
extern TrackInfo musicTracks[TRACK_COUNT];

extern int currentStreamIndex;
extern StreamFile streamFile[STREAMFILE_COUNT];
extern StreamInfo streamInfo[STREAMFILE_COUNT];
extern StreamFile *streamFilePtr;
extern StreamInfo *streamInfoPtr;

extern SFXInfo sfxList[SFX_COUNT];
#if RETRO_PLATFORM == RETRO_PS1
extern char sfxNames[SFX_COUNT][0x20]; // PS1: Sonic 2's longest SFX name has 16 chars
#else
extern char sfxNames[SFX_COUNT][0x40];
#endif

extern ChannelInfo sfxChannels[CHANNEL_COUNT];

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
extern SDL_AudioSpec audioDeviceFormat;
#endif

int InitAudioPlayback();
void LoadGlobalSfx();

#if RETRO_USING_SDL1 || RETRO_USING_SDL2
#if !RETRO_USE_ORIGINAL_CODE
// These functions did exist, but with different signatures
void ProcessMusicStream(Sint32 *stream, size_t bytes_wanted);
void ProcessAudioPlayback(void *data, Uint8 *stream, int len);
void ProcessAudioMixing(Sint32 *dst, const Sint16 *src, int len, int volume, sbyte pan);
#endif

#if !RETRO_USE_ORIGINAL_CODE
inline void FreeMusInfo()
{
    LockAudioDevice();

#if RETRO_USING_SDL2
    if (streamInfo[currentStreamIndex].stream)
        SDL_FreeAudioStream(streamInfo[currentStreamIndex].stream);
    streamInfo[currentStreamIndex].stream = NULL;
#endif

    ov_clear(&streamInfo[currentStreamIndex].vorbisFile);

#if RETRO_USING_SDL2
    streamInfo[currentStreamIndex].stream = nullptr;
#endif

    UnlockAudioDevice();
}
#endif
#else
// inline: defined in a header (upstream had plain definitions here, one per including file).
inline void ProcessMusicStream() {}
inline void ProcessAudioPlayback() {}
inline void ProcessAudioMixing() {}

#if RETRO_PLATFORM == RETRO_PS1
void PS1MusicStop(); // stops the CD-XA track (ps1/audio.cpp, ps1/xa_music)
void PS1MusicPause(bool pause);
inline void FreeMusInfo()
{
    streamInfo[currentStreamIndex].loaded = false;
    PS1MusicStop();
}
#elif !RETRO_USE_ORIGINAL_CODE
inline void FreeMusInfo() { ov_clear(&streamInfo[currentStreamIndex].vorbisFile); }
#endif
#endif

#if RETRO_PLATFORM == RETRO_PS1
// ps1/audio.cpp: RSDK channel c plays on SPU voice c + 1; samples come from banks (docs/30 phase 5.1): the resident
// one (the pause menu's sounds), the stage's (LoadStageFiles) or the menus' (PS1SfxLoadMenuBank).
void PS1SfxChannelStop(int channel);
void PS1SfxReleaseStage();
void PS1SfxLoadStageBank(); // Data/Stages/<folder>/PS1Sfx.bin, after the stage's LoadSfx calls
void PS1SfxLoadMenuBank();  // Data/Game/PS1SfxMenu.bin into the stage area (the menus run no stage)
void PS1AudioUpdate();      // once per frame: frees channels whose sample ended (PC mixer semantics), music volume
void PS1MusicHold();        // before a file read under music (the menus): stop, remembering the track and its row
void PS1MusicRestore();     // after it: the same track from that row
#endif

void LoadMusic(void *userdata);
void SetMusicTrack(const char *filePath, byte trackID, bool loop, uint loopPoint);
void SwapMusicTrack(const char *filePath, byte trackID, uint loopPoint, uint ratio);
bool PlayMusic(int track, int musStartPos);
inline void StopMusic(bool setStatus)
{
    if (setStatus)
        musicStatus = MUSIC_STOPPED;
    musicPosition = 0;

#if !RETRO_USE_ORIGINAL_CODE
    LockAudioDevice();
    FreeMusInfo();
    UnlockAudioDevice();
#endif
}

void LoadSfx(char *filePath, byte sfxID);
void PlaySfx(int sfx, bool loop);
inline void StopSfx(int sfx)
{
    for (int i = 0; i < CHANNEL_COUNT; ++i) {
        if (sfxChannels[i].sfxID == sfx) {
#if RETRO_PLATFORM == RETRO_PS1
            PS1SfxChannelStop(i);
#endif
            MEM_ZERO(sfxChannels[i]);
            sfxChannels[i].sfxID = -1;
        }
    }
}
void SetSfxAttributes(int sfx, int loopCount, sbyte pan);

void SetSfxName(const char *sfxName, int sfxID);

#if !RETRO_USE_ORIGINAL_CODE
// Helper Funcs
inline bool PlaySfxByName(const char *sfx, sbyte loopCnt)
{
    char buffer[0x40];
    int pos = 0;
    while (*sfx) {
        if (*sfx != ' ')
            buffer[pos++] = *sfx;
        sfx++;
    }
    buffer[pos] = 0;

    for (int s = 0; s < globalSFXCount + stageSFXCount; ++s) {
        if (StrComp(sfxNames[s], buffer)) {
            PlaySfx(s, loopCnt);
            return true;
        }
    }
    return false;
}
inline bool StopSFXByName(const char *sfx)
{
    char buffer[0x40];
    int pos = 0;
    while (*sfx) {
        if (*sfx != ' ')
            buffer[pos++] = *sfx;
        sfx++;
    }
    buffer[pos] = 0;

    for (int s = 0; s < globalSFXCount + stageSFXCount; ++s) {
        if (StrComp(sfxNames[s], buffer)) {
            StopSfx(s);
            return true;
        }
    }
    return false;
}
#endif

inline void SetMusicVolume(int volume)
{
    if (volume < 0)
        volume = 0;
    if (volume > MAX_VOLUME)
        volume = MAX_VOLUME;

    masterVolume = volume;
}

inline void SetGameVolumes(int bgmVol, int sfxVol)
{
    bgmVolume = bgmVol;
    sfxVolume = sfxVol;

    if (bgmVolume < 0)
        bgmVolume = 0;
    if (bgmVolume > MAX_VOLUME)
        bgmVolume = MAX_VOLUME;

    if (sfxVolume < 0)
        sfxVolume = 0;
    if (sfxVolume > MAX_VOLUME)
        sfxVolume = MAX_VOLUME;
}

inline bool PauseSound()
{
    if (musicStatus == MUSIC_PLAYING) {
        musicStatus = MUSIC_PAUSED;
#if RETRO_PLATFORM == RETRO_PS1
        PS1MusicPause(true);
#endif
        return true;
    }
    return false;
}

inline void ResumeSound()
{
    if (musicStatus == MUSIC_PAUSED) {
        musicStatus = MUSIC_PLAYING;
#if RETRO_PLATFORM == RETRO_PS1
        PS1MusicPause(false);
#endif
    }
}

inline void StopAllSfx()
{
#if !RETRO_USE_ORIGINAL_CODE
    LockAudioDevice();
#endif

    for (int i = 0; i < CHANNEL_COUNT; ++i) {
#if RETRO_PLATFORM == RETRO_PS1
        PS1SfxChannelStop(i);
#endif
        sfxChannels[i].sfxID = -1;
    }

#if !RETRO_USE_ORIGINAL_CODE
    UnlockAudioDevice();
#endif
}
inline void ReleaseGlobalSfx()
{
    for (int i = globalSFXCount - 1; i >= 0; --i) {
        if (sfxList[i].loaded) {
            StrCopy(sfxList[i].name, "");
            StrCopy(sfxNames[i], "");
            if (sfxList[i].buffer)
                free(sfxList[i].buffer);

            sfxList[i].buffer = NULL;
            sfxList[i].length = 0;
            sfxList[i].loaded = false;
        }
    }

    globalSFXCount = 0;
}
inline void ReleaseStageSfx()
{
    for (int i = (stageSFXCount + globalSFXCount) - 1; i >= globalSFXCount; --i) {
        if (sfxList[i].loaded) {
            StrCopy(sfxList[i].name, "");
            StrCopy(sfxNames[i], "");
            if (sfxList[i].buffer)
                free(sfxList[i].buffer);

            sfxList[i].buffer = NULL;
            sfxList[i].length = 0;
            sfxList[i].loaded = false;
        }
    }

    stageSFXCount = 0;
#if RETRO_PLATFORM == RETRO_PS1
    PS1SfxReleaseStage();
#endif
}

inline void ReleaseAudioDevice()
{
    StopMusic(true);
    StopAllSfx();
    ReleaseStageSfx();
    ReleaseGlobalSfx();
}

#endif // !AUDIO_H
