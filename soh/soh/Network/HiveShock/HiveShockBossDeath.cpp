#include <algorithm>
#include <memory> // z64.h includes it; it must not be pulled in for the first time inside extern "C"

#include "HiveShockBossDeath.h"
#include "soh/Enhancements/game-interactor/GameInteractor_Hooks.h"

extern "C" {
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include "z64.h"
}

namespace {

void Spark(PlayState* play, const Vec3f& at, f32 spread, bool bright) {
    Vec3f pos = { at.x + Rand_CenteredFloat(spread), at.y + Rand_CenteredFloat(spread),
                  at.z + Rand_CenteredFloat(spread) };
    Vec3f vel = { Rand_CenteredFloat(2.0f), Rand_CenteredFloat(2.0f), Rand_CenteredFloat(2.0f) };
    Vec3f accel = { 0.0f, 0.0f, 0.0f };
    Color_RGBA8 prim = bright ? Color_RGBA8{ 255, 200, 255, 255 } : Color_RGBA8{ 190, 60, 255, 255 };
    Color_RGBA8 env = { 110, 0, 200, 0 };
    EffectSsKiraKira_SpawnDispersed(play, &pos, &vel, &accel, &prim, &env, 1500, 12);
}

} // namespace

extern "C" void HiveShock_BossExplode(PlayState* play, Actor* boss, int big) {
    Player* player = GET_PLAYER(play);
    Vec3f center = boss->world.pos;
    center.y += big ? 80.0f : 30.0f;
    Vec3f zero = { 0.0f, 0.0f, 0.0f };
    Vec3f accel = { 0.0f, 0.0f, 0.0f };

    const f32 size = big ? 1.8f : 1.0f;
    for (int i = 0; i < (big ? 12 : 7); i++) {
        Vec3f pos = { center.x + Rand_CenteredFloat(90.0f * size), center.y + Rand_ZeroFloat(70.0f * size),
                      center.z + Rand_CenteredFloat(90.0f * size) };
        EffectSsBomb2_SpawnLayered(play, &pos, &zero, &accel, big ? 200 : 120, 22 + i * 4);
    }
    for (int i = 0; i < (big ? 130 : 70); i++) {
        Vec3f pos = { center.x + Rand_CenteredFloat(140.0f * size), center.y + Rand_CenteredFloat(120.0f * size),
                      center.z + Rand_CenteredFloat(140.0f * size) };
        Vec3f up = { 0.0f, 0.5f, 0.0f };
        EffectSsDeadDb_Spawn(play, &pos, &up, &up, 100, 0, 180, 40, 255, 255, 80, 0, 160, 1, 9, true);
    }
    for (int i = 0; i < (big ? 80 : 40); i++) {
        Spark(play, center, 120.0f * size, i % 2 == 0);
    }
    Audio_PlayActorSound2(boss, NA_SE_IT_BOMB_EXPLOSION);
    Camera_AddQuake(&play->mainCamera, 2, 14, 8);

    if (Math_Vec3f_DistXYZ(&player->actor.world.pos, &boss->world.pos) < 120.0f * size &&
        player->invincibilityTimer <= 0) {
        Actor_SetPlayerKnockbackLarge(play, boss, 8.0f, Math_Vec3f_Yaw(&boss->world.pos, &player->actor.world.pos),
                                      6.0f, 0x10);
    }

    Vec3f drop = boss->world.pos;
    for (int i = 0; i < (big ? 6 : 3); i++) {
        Item_DropCollectibleRandom(play, boss, &drop, 0xB0);
    }
    GameInteractor_ExecuteOnEnemyDefeat(boss);
    Actor_Kill(boss);
}
