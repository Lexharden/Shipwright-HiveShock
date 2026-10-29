#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "z64.h"

// How many Ganondorfs and how many beasts HiveShock can have in the game at once. Each one owns a slot with its own
// copy of the state the vanilla actor keeps in file-level variables (see the slots in z_boss_ganon.c and
// z_boss_ganon2.c). More than this wait in the queue like any enemy that does not fit.
#define HIVESHOCK_MAX_GANONDORF 8
#define HIVESHOCK_MAX_GANON_BEAST 8

// How many Ganondorfs and how many beasts HiveShock can have in the game at once. Each one owns a slot with its own
// copy of the state the vanilla actor keeps in file-level variables (see the slots in z_boss_ganon.c and
// z_boss_ganon2.c). More than this wait in the queue like any enemy that does not fit.
#define HIVESHOCK_MAX_GANONDORF 8
#define HIVESHOCK_MAX_GANON_BEAST 8

// Set by HiveShock around the Actor_Spawn of Barinade, Ganondorf and Ganon: tells the vanilla boss it is being
// spawned as an enemy (no cutscenes, no scene flags) and not as the story fight.
extern int gHiveShockBossSpawn;

// How HiveShock's version of Ganondorf and Ganon ends: a blast of fire and sparks where the boss stood, a few
// drops, and the actor is removed. `big` is the beast.
void HiveShock_BossExplode(PlayState* play, Actor* boss, int big);

#ifdef __cplusplus
}
#endif
