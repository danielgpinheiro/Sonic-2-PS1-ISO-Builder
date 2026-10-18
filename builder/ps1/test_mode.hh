#pragma once
/*
 * Regression test mode (`make TEST=1`, docs/45; never in a release build: make dist / export_builder refuse it).
 *
 * The test disc carries TEST.SEL (32 KB, tools from ../tests): zero = no scenario (the EXE plays like the release
 * game), else a compiled scenario (../tests/tools/scn.py): boot target, RNG seed, input track, checkpoints, pokes,
 * end frame. Results go out over the PCSX TTY port (0x1F802080; PCSX-Redux and DuckStation print it), one line per
 * logic frame plus checkpoint dumps, and the run ends with pcsx_exit(status).
 */
#include <stdint.h>

#if PS1_TEST
struct PS1TestScenario;
extern PS1TestScenario *g_ps1Test; // null: no scenario on the disc

void PS1TestInit();              // start(): read TEST.SEL, install the watchdog / crash hooks
bool PS1TestBootStage();         // the scenario boots straight into a stage (not the menus)
const char *PS1TestBootFolder(); // its stage folder / id / player (stage.playerListPos)
const char *PS1TestBootId();
int PS1TestBootPlayer();
int PS1TestBootMode();          // game mode (Sonic CD: Options.GameMode)
void PS1TestAfterInit();         // after Engine.Init: seed, player 2 flag
void PS1TestPreFrame();          // frame(), before RunFrame: input track, pokes
void PS1TestPostFrame();         // frame(), after PS1PerfTick: telemetry, checkpoints, end of run
void PS1TestPhase(uint8_t p);    // hang-diagnosis marker (what the main loop is doing)
#endif
