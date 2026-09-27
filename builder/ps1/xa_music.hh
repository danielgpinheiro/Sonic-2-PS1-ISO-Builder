/*
 * CD-XA music (Sonic CD docs/28 phase 5.2; Sonic 2 docs/30 phase 5.2): plays one track of Data/Music/Music.xa (tools/audio/build_music.py)
 * through the CD drive's XA-ADPCM decoder into the SPU's CD input. No CPU decoding, no SPU RAM.
 * Sonic CD: 68 tracks in 8 channels, back to back per channel; a track starts at its start row, loops
 * at its loop row (the script's loop point, row-aligned at build time) and ends at its end marker.
 *
 * A long-lived psyqo::CDRomDevice::Action (raw commands, like ps1/cdrom_stream.cpp):
 * Setloc -> Setfilter(1, channel) -> Setmode(2x | XA-ADPCM | 2340-byte sectors | filter) ->
 * ReadS. Audio sectors never reach the CPU; data sectors do (INT1): each is DMA'd into a small
 * buffer, and the track's end-marker sector (channel + track id) either loops the track
 * (Setloc to the loop row + ReadS) or pauses.
 * One CD action at a time: stop the music before any file read (cdrom_fs does it as a safety
 * net and counts it).
 */
#pragma once

#include <stdint.h>

bool PS1XaInit();                                   // reads Data/Music/Music.bin + finds Music.xa
int  PS1XaFindTrack(const char *path);              // "Data/Music/JP/R1A.ogg" -> track, -1 if absent
void PS1XaPlay(int track, bool loop);               // (re)start a track from its beginning
void PS1XaPlayAt(int track, bool loop, uint32_t row); // ... from one of its rows (SwapMusicTrack's position)
uint32_t PS1XaPositionRows();                       // the playing position, rows from the track's start (timed)
uint32_t PS1XaTrackRows(int track);                 // a track's audio rows (18.75 per second)
int PS1XaCurrentTrack();                            // the last track played, -1 before any
bool PS1XaCurrentLoop();
void PS1XaStop();                                   // pause the drive and wait until the action ended
void PS1XaPause();                                  // PauseSound: stop reading, keep the position
void PS1XaResume();                                 // ResumeSound: ReadS again from the current position
bool PS1XaPlaying();                                // reading (not ended, not paused/stopped)
bool PS1XaEnded();                                  // a non-looping track reached its end marker

extern volatile uint32_t g_ps1XaStarts;      // tracks started
extern volatile uint32_t g_ps1XaLoops;       // loops (end marker -> Setloc + ReadS)
extern volatile uint32_t g_ps1XaEnds;        // non-looping ends
extern volatile uint32_t g_ps1XaDataSectors; // data sectors seen (end markers of any channel)
extern volatile uint32_t g_ps1XaForcedStops; // stopped by a file read (cdrom_fs safety net)
extern volatile uint32_t g_ps1XaLoopFrame;   // g_ps1PerfFrames at the last loop (gap check)
extern volatile uint32_t g_ps1XaStartFrame;  // g_ps1PerfFrames when the current track started
extern volatile int32_t g_ps1XaChannel;      // channel of the current track
extern volatile int32_t g_ps1XaTrack;        // current track (Music.bin index)
