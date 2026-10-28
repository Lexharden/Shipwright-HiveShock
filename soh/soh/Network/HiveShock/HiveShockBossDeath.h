#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "z64.h"

// Set by HiveShock around the Actor_Spawn of Barinade, Ganondorf and Ganon: tells the vanilla boss it is being
// spawned as an enemy (no cutscenes, no scene flags) and not as the story fight.
extern int gHiveShockBossSpawn;

// How HiveShock's version of Ganondorf and Ganon ends: a blast of fire and sparks where the boss stood, a few
// drops, and the actor is removed. `big` is the beast.
void HiveShock_BossExplode(PlayState* play, Actor* boss, int big);

#ifdef __cplusplus
}
#endif
