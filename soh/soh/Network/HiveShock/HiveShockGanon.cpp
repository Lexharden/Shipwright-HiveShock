#include "HiveShockGanon.h"

#if !defined(__SWITCH__) && !defined(__WIIU__)

#include "soh/ActorDB.h"
#include "soh/frame_interpolation.h"

#include <algorithm>
#include <cstddef>
#include <memory> // z64.h includes it; it must not be pulled in for the first time inside extern "C"

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
#include <objects/object_ganon/object_ganon.h>
#include <objects/object_ganon2/object_ganon2.h>
#include <objects/object_ganon_anime1/object_ganon_anime1.h>
#include <overlays/ovl_Boss_Ganon/ovl_Boss_Ganon.h>
extern PlayState* gPlayState;
}

namespace {

constexpr s16 kHealth = 30;
constexpr s16 kEnrageHealth = 14;
constexpr int kMaxBalls = 8;
constexpr f32 kBallSpeed = 7.0f;
constexpr f32 kBallSpeedEnraged = 9.0f;
constexpr f32 kReflectSpeed = 12.0f;
constexpr s16 kBallLife = 140;
constexpr f32 kHoverHeight = 115.0f; // above the floor and above Link's feet
constexpr f32 kPreferredDistance = 230.0f;
constexpr s16 kFallMaxFrames = 90;
constexpr f32 kBlastRadius = 120.0f;
constexpr s16 kBeastHealth = 40;
constexpr f32 kBeastWalkSpeed = 4.5f;
constexpr f32 kBeastSwingRange = 150.0f;
constexpr f32 kBeastStopDistance = 85.0f;
constexpr s16 kBeastAttackRadius = 125;

// Spawn params: which Ganon this is. They are separate spawns of the same actor.
enum Form : s16 {
    FORM_GANONDORF = 0, // floats, throws energy balls, falls and explodes when beaten
    FORM_BEAST = 1,     // the second phase: walks, swings two swords, collapses and explodes
};

enum State : u8 {
    ST_APPEAR,
    ST_FIGHT,
    ST_FALL,
    ST_B_RISE,
    ST_B_WALK,
    ST_B_SWING,
    ST_B_STUN,
    ST_B_DEATH,
};

struct Ball {
    bool active;
    bool reflected;
    s16 life;
    Vec3f pos;
    Vec3f vel;
};

struct HsGanon {
    Actor actor;
    SkelAnime skelAnime;
    ColliderCylinder collider;
    u8 state;
    bool beast;      // draws and fights as the beast
    bool leftSwing;  // which sword swings next
    u8 swingCount;   // swings since the last rest
    u8 stunStage;    // 0 falling over, 1 down, 2 getting up
    bool attackOn;   // the sword is dangerous this frame
    bool animDone;   // the animation finished this frame (only for play-once animations)
    s16 swingCooldown;
    s16 acCooldown;  // frames the beast ignores hits after deflecting one
    bool enraged;
    s16 timer;       // generic frame counter of the current state
    s16 chargeTimer; // > 0 while charging an attack
    s16 cooldown;    // frames until the next charge
    s16 strafeTimer;
    s8 strafeDir;
    s16 bobPhase;
    u8 anim;           // Ganondorf: 0 floating, 1 charging, 2 throwing
    f32 orbScale;      // size of the ball he holds while charging
    s16 orbSpin;
    Vec3f handPos;     // right hand / left sword hand, as of the last draw
    Vec3f swordPos;
    Ball balls[kMaxBalls];
};

ColliderCylinderInit sCylinderInit = {
    {
        COLTYPE_HIT3,
        AT_ON | AT_TYPE_ENEMY,
        AC_ON | AC_TYPE_PLAYER,
        OC1_ON | OC1_TYPE_ALL,
        OC2_TYPE_1,
        COLSHAPE_CYLINDER,
    },
    {
        ELEMTYPE_UNK0,
        { 0xFFCFFFFF, 0x00, 0x10 },
        { 0xFFCFFFFE, 0x00, 0x00 },
        TOUCH_ON | TOUCH_SFX_NORMAL,
        BUMP_ON | BUMP_HOOKABLE,
        OCELEM_ON,
    },
    { 30, 90, -40, { 0, 0, 0 } },
};

DamageTable sDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, 0x0),
    /* Deku stick    */ DMG_ENTRY(1, 0x0),
    /* Slingshot     */ DMG_ENTRY(0, 0x0),
    /* Explosive     */ DMG_ENTRY(4, 0x0),
    /* Boomerang     */ DMG_ENTRY(0, 0x0),
    /* Normal arrow  */ DMG_ENTRY(2, 0x0),
    /* Hammer swing  */ DMG_ENTRY(4, 0x0),
    /* Hookshot      */ DMG_ENTRY(0, 0x0),
    /* Kokiri sword  */ DMG_ENTRY(1, 0x0),
    /* Master sword  */ DMG_ENTRY(2, 0x0),
    /* Giant's Knife */ DMG_ENTRY(4, 0x0),
    /* Fire arrow    */ DMG_ENTRY(2, 0x0),
    /* Ice arrow     */ DMG_ENTRY(2, 0x0),
    /* Light arrow   */ DMG_ENTRY(6, 0x0),
    /* Unk arrow 1   */ DMG_ENTRY(2, 0x0),
    /* Unk arrow 2   */ DMG_ENTRY(2, 0x0),
    /* Unk arrow 3   */ DMG_ENTRY(2, 0x0),
    /* Fire magic    */ DMG_ENTRY(2, 0x0),
    /* Ice magic     */ DMG_ENTRY(2, 0x0),
    /* Light magic   */ DMG_ENTRY(4, 0x0),
    /* Shield        */ DMG_ENTRY(0, 0x0),
    /* Mirror Ray    */ DMG_ENTRY(0, 0x0),
    /* Kokiri spin   */ DMG_ENTRY(2, 0x0),
    /* Giant spin    */ DMG_ENTRY(4, 0x0),
    /* Master spin   */ DMG_ENTRY(3, 0x0),
    /* Kokiri jump   */ DMG_ENTRY(2, 0x0),
    /* Giant jump    */ DMG_ENTRY(6, 0x0),
    /* Master jump   */ DMG_ENTRY(4, 0x0),
    /* Unknown 1     */ DMG_ENTRY(0, 0x0),
    /* Unblockable   */ DMG_ENTRY(0, 0x0),
    /* Hammer jump   */ DMG_ENTRY(6, 0x0),
    /* Unknown 2     */ DMG_ENTRY(0, 0x0),
};

// The beast shrugs hits off while he is guarding; only a stunned one takes damage. Light (arrow, magic) stuns at
// once, which is what the effect code 1 marks.
DamageTable sBeastDamageTable = {
    /* Deku nut      */ DMG_ENTRY(0, 0x0),
    /* Deku stick    */ DMG_ENTRY(1, 0x0),
    /* Slingshot     */ DMG_ENTRY(0, 0x0),
    /* Explosive     */ DMG_ENTRY(3, 0x0),
    /* Boomerang     */ DMG_ENTRY(0, 0x0),
    /* Normal arrow  */ DMG_ENTRY(1, 0x0),
    /* Hammer swing  */ DMG_ENTRY(3, 0x0),
    /* Hookshot      */ DMG_ENTRY(0, 0x0),
    /* Kokiri sword  */ DMG_ENTRY(1, 0x0),
    /* Master sword  */ DMG_ENTRY(2, 0x0),
    /* Giant's Knife */ DMG_ENTRY(4, 0x0),
    /* Fire arrow    */ DMG_ENTRY(1, 0x0),
    /* Ice arrow     */ DMG_ENTRY(1, 0x0),
    /* Light arrow   */ DMG_ENTRY(5, 0x1),
    /* Unk arrow 1   */ DMG_ENTRY(1, 0x0),
    /* Unk arrow 2   */ DMG_ENTRY(1, 0x0),
    /* Unk arrow 3   */ DMG_ENTRY(1, 0x0),
    /* Fire magic    */ DMG_ENTRY(1, 0x0),
    /* Ice magic     */ DMG_ENTRY(1, 0x0),
    /* Light magic   */ DMG_ENTRY(4, 0x1),
    /* Shield        */ DMG_ENTRY(0, 0x0),
    /* Mirror Ray    */ DMG_ENTRY(0, 0x0),
    /* Kokiri spin   */ DMG_ENTRY(2, 0x0),
    /* Giant spin    */ DMG_ENTRY(4, 0x0),
    /* Master spin   */ DMG_ENTRY(3, 0x0),
    /* Kokiri jump   */ DMG_ENTRY(2, 0x0),
    /* Giant jump    */ DMG_ENTRY(5, 0x0),
    /* Master jump   */ DMG_ENTRY(4, 0x0),
    /* Unknown 1     */ DMG_ENTRY(0, 0x0),
    /* Unblockable   */ DMG_ENTRY(0, 0x0),
    /* Hammer jump   */ DMG_ENTRY(5, 0x0),
    /* Unknown 2     */ DMG_ENTRY(0, 0x0),
};

void Spark(PlayState* play, const Vec3f& at, f32 spread, bool bright) {
    Vec3f pos = { at.x + Rand_CenteredFloat(spread), at.y + Rand_CenteredFloat(spread),
                  at.z + Rand_CenteredFloat(spread) };
    Vec3f vel = { Rand_CenteredFloat(2.0f), Rand_CenteredFloat(2.0f), Rand_CenteredFloat(2.0f) };
    Vec3f accel = { 0.0f, 0.0f, 0.0f };
    Color_RGBA8 prim = bright ? Color_RGBA8{ 255, 200, 255, 255 } : Color_RGBA8{ 190, 60, 255, 255 };
    Color_RGBA8 env = { 110, 0, 200, 0 };
    EffectSsKiraKira_SpawnDispersed(play, &pos, &vel, &accel, &prim, &env, 1500, 12);
}

// Where the energy balls leave: in front of the chest.
Vec3f HandPos(const HsGanon* self) { // set from the right hand limb when drawn
    return self->handPos;
}

void ThrowBall(HsGanon* self, PlayState* play, s16 yawOffset) {
    Player* player = GET_PLAYER(play);
    for (Ball& ball : self->balls) {
        if (ball.active) {
            continue;
        }
        ball.active = true;
        ball.reflected = false;
        ball.life = kBallLife;
        ball.pos = HandPos(self);
        Vec3f target = player->actor.world.pos;
        target.y += 35.0f;
        const s16 yaw = Math_Vec3f_Yaw(&ball.pos, &target) + yawOffset;
        const f32 flat = Math_Vec3f_DistXZ(&ball.pos, &target);
        const f32 speed = self->enraged ? kBallSpeedEnraged : kBallSpeed;
        const f32 rise = (target.y - ball.pos.y) / std::max(flat, 1.0f);
        ball.vel.x = Math_SinS(yaw) * speed;
        ball.vel.z = Math_CosS(yaw) * speed;
        ball.vel.y = rise * speed;
        return;
    }
}

void HurtPlayer(HsGanon* self, PlayState* play, s16 yaw, f32 push) {
    Player* player = GET_PLAYER(play);
    if (player->invincibilityTimer > 0) {
        return;
    }
    Actor_SetPlayerKnockbackLarge(play, &self->actor, push, yaw, 6.0f, 0x10);
}

void Hurt(HsGanon* self, int damage) {
    self->actor.colChkInfo.health = static_cast<u8>(std::max<int>(0, self->actor.colChkInfo.health - damage));
    Actor_SetColorFilter(&self->actor, 0x4000, 255, 0, 12);
    Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_DAMAGE1);
}

void UpdateBalls(HsGanon* self, PlayState* play) {
    Player* player = GET_PLAYER(play);
    Vec3f link = player->actor.world.pos;
    link.y += 30.0f;
    Vec3f chest = self->actor.world.pos;
    chest.y += 40.0f;

    for (Ball& ball : self->balls) {
        if (!ball.active) {
            continue;
        }
        ball.pos.x += ball.vel.x;
        ball.pos.y += ball.vel.y;
        ball.pos.z += ball.vel.z;
        Spark(play, ball.pos, 6.0f, ball.reflected);
        Spark(play, ball.pos, 14.0f, false);
        if (--ball.life <= 0) {
            ball.active = false;
            continue;
        }

        if (ball.reflected) {
            if (Math_Vec3f_DistXYZ(&ball.pos, &chest) < 55.0f && self->state == ST_FIGHT) {
                // Batted back: this is how Ganondorf is beaten in the tower too.
                Hurt(self, 4);
                for (int i = 0; i < 6; i++) {
                    Spark(play, ball.pos, 30.0f, true);
                }
                Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_HIT_THUNDER);
                ball.active = false;
            }
            continue;
        }

        if (Math_Vec3f_DistXYZ(&ball.pos, &link) < 34.0f) {
            const bool facing = ABS(static_cast<s16>(player->actor.shape.rot.y -
                                                     Math_Vec3f_Yaw(&player->actor.world.pos, &ball.pos))) < 0x3000;
            if ((player->stateFlags1 & PLAYER_STATE1_SHIELDING) && facing) {
                // Shield up and facing it: send it back at Ganon, faster.
                const s16 yaw = Math_Vec3f_Yaw(&ball.pos, &chest);
                const f32 flat = Math_Vec3f_DistXZ(&ball.pos, &chest);
                ball.reflected = true;
                ball.life = kBallLife;
                ball.vel.x = Math_SinS(yaw) * kReflectSpeed;
                ball.vel.z = Math_CosS(yaw) * kReflectSpeed;
                ball.vel.y = ((chest.y - ball.pos.y) / std::max(flat, 1.0f)) * kReflectSpeed;
                Audio_PlayActorSound2(&player->actor, NA_SE_IT_SHIELD_REFLECT_MG);
            } else {
                HurtPlayer(self, play, Math_Vec3f_Yaw(&ball.pos, &link), 4.0f);
                Audio_PlayActorSound2(&player->actor, NA_SE_EN_GANON_HIT_THUNDER);
                ball.active = false;
            }
        }
    }
}

void Explode(HsGanon* self, PlayState* play, bool big) {
    Player* player = GET_PLAYER(play);
    Vec3f center = self->actor.world.pos;
    center.y += 30.0f;
    Vec3f zero = { 0.0f, 0.0f, 0.0f };
    Vec3f accel = { 0.0f, 0.0f, 0.0f };

    const f32 size = big ? 1.8f : 1.0f;
    if (big) {
        center.y += 50.0f;
    }
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
    Audio_PlayActorSound2(&self->actor, NA_SE_IT_BOMB_EXPLOSION);
    Camera_AddQuake(&play->mainCamera, 2, 14, 8);

    if (Math_Vec3f_DistXYZ(&player->actor.world.pos, &self->actor.world.pos) < kBlastRadius * size) {
        HurtPlayer(self, play, Math_Vec3f_Yaw(&self->actor.world.pos, &player->actor.world.pos), 8.0f);
    }

    Vec3f drop = self->actor.world.pos;
    for (int i = 0; i < (big ? 6 : 3); i++) {
        Item_DropCollectibleRandom(play, &self->actor, &drop, 0xB0);
    }
    Actor_Kill(&self->actor);
}

void Init(Actor* thisx, PlayState* play) {
    HsGanon* self = reinterpret_cast<HsGanon*>(thisx);

    self->beast = (self->actor.params == FORM_BEAST);
    Actor_SetScale(&self->actor, 0.0f);
    self->handPos = self->actor.world.pos;
    self->handPos.y += 90.0f;
    self->swordPos = self->handPos;
    self->actor.colChkInfo.mass = MASS_IMMOVABLE;
    self->actor.naviEnemyId = 0x3E;
    ActorShape_Init(&self->actor.shape, 0.0f, ActorShadow_DrawCircle, self->beast ? 90.0f : 45.0f);
    Collider_InitCylinder(play, &self->collider);
    Collider_SetCylinder(play, &self->collider, &self->actor, &sCylinderInit);

    if (self->beast) {
        self->actor.gravity = -2.0f;
        self->actor.colChkInfo.health = kBeastHealth;
        self->actor.colChkInfo.damageTable = &sBeastDamageTable;
        SkelAnime_InitFlex(play, &self->skelAnime, (FlexSkeletonHeader*)gGanonSkel,
                           (AnimationHeader*)gGanonGuardIdleAnim, nullptr, nullptr, 0);
        self->collider.dim.radius = 45;
        self->collider.dim.height = 170;
        self->collider.dim.yShift = 0;
        self->collider.info.toucher.damage = 0x20; // the swords: two hearts
        self->state = ST_B_RISE;
        self->timer = 45;
        self->swingCooldown = 30;
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_LAUGH);
    } else {
        self->actor.gravity = 0.0f;
        self->actor.colChkInfo.health = kHealth;
        self->actor.colChkInfo.damageTable = &sDamageTable;
        SkelAnime_InitFlex(play, &self->skelAnime, (FlexSkeletonHeader*)gGanondorfSkel, nullptr, nullptr, nullptr, 0);
        Animation_PlayLoop(&self->skelAnime, (AnimationHeader*)gGanondorfFloatAnim);
        self->state = ST_APPEAR;
        self->timer = 30;
        self->cooldown = 50;
        self->strafeDir = (Rand_ZeroOne() < 0.5f) ? -1 : 1;
        self->strafeTimer = 60;
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_LAUGH);
    }
}

void Destroy(Actor* thisx, PlayState* play) {
    HsGanon* self = reinterpret_cast<HsGanon*>(thisx);

    SkelAnime_Free(&self->skelAnime, play);
    Collider_DestroyCylinder(play, &self->collider);
}

void UpdateAppear(HsGanon* self, PlayState* play) {
    Math_ApproachF(&self->actor.scale.x, 0.01f, 0.25f, 0.0012f);
    self->actor.scale.y = self->actor.scale.z = self->actor.scale.x;
    Spark(play, self->actor.world.pos, 70.0f, false);
    Math_SmoothStepToS(&self->actor.shape.rot.y, self->actor.yawTowardsPlayer, 2, 0x800, 0x80);
    if (--self->timer <= 0) {
        Actor_SetScale(&self->actor, 0.01f);
        self->state = ST_FIGHT;
    }
}

void UpdateFight(HsGanon* self, PlayState* play) {
    Player* player = GET_PLAYER(play);

    if (!self->enraged && self->actor.colChkInfo.health <= kEnrageHealth) {
        self->enraged = true;
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_LAUGH);
        Actor_SetColorFilter(&self->actor, 0x4000, 255, 0, 20);
    }

    // Circle Link at a distance, bobbing in the air.
    const f32 dist = self->actor.xzDistToPlayer;
    const s16 toLink = self->actor.yawTowardsPlayer;
    const f32 forward = std::clamp((dist - kPreferredDistance) * 0.03f, -3.5f, 3.5f);
    const f32 strafe = self->strafeDir * (self->enraged ? 3.2f : 2.2f) * (self->chargeTimer > 0 ? 0.3f : 1.0f);
    self->actor.world.pos.x += Math_SinS(toLink) * forward + Math_SinS(toLink + 0x4000) * strafe;
    self->actor.world.pos.z += Math_CosS(toLink) * forward + Math_CosS(toLink + 0x4000) * strafe;
    if (--self->strafeTimer <= 0) {
        self->strafeDir = -self->strafeDir;
        self->strafeTimer = 60 + static_cast<s16>(Rand_ZeroFloat(60.0f));
    }

    self->bobPhase += 0x400;
    const f32 target = std::max(player->actor.world.pos.y, self->actor.floorHeight) + kHoverHeight +
                       Math_SinS(self->bobPhase) * 10.0f;
    Math_ApproachF(&self->actor.world.pos.y, target, 0.1f, 4.0f);
    Math_SmoothStepToS(&self->actor.shape.rot.y, toLink, 3, 0x700, 0x80);
    self->actor.world.rot.y = self->actor.shape.rot.y;

    if (self->anim == 2 && self->animDone) {
        self->anim = 0;
        Animation_MorphToLoop(&self->skelAnime, (AnimationHeader*)gGanondorfFloatAnim, -8.0f);
    }
    Math_ApproachF(&self->orbScale, (self->chargeTimer > 0) ? 7.0f : 0.0f, 0.4f, 0.9f);
    self->orbSpin += 0x1800;

    // Charge, then throw: three balls in a fan once it is angry.
    if (self->chargeTimer > 0) {
        if (--self->chargeTimer == 0) {
            self->anim = 2;
            Animation_MorphToPlayOnce(&self->skelAnime, (AnimationHeader*)gGanondorfThrowAnim, -3.0f);
            if (self->enraged) {
                ThrowBall(self, play, -0x1000);
                ThrowBall(self, play, 0);
                ThrowBall(self, play, 0x1000);
            } else {
                ThrowBall(self, play, 0);
            }
            Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_THROW_MASIC);
            self->cooldown = self->enraged ? 45 : 80;
        }
    } else if (--self->cooldown <= 0) {
        self->chargeTimer = self->enraged ? 18 : 28;
        self->anim = 1;
        Animation_MorphToPlayOnce(&self->skelAnime, (AnimationHeader*)gGanondorfChargeLightBallAnim, -3.0f);
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_SPARK);
    }

    // Hits from Link.
    if (self->collider.base.acFlags & AC_HIT) {
        self->collider.base.acFlags &= ~AC_HIT;
        if (self->actor.colChkInfo.damage > 0) {
            Hurt(self, self->actor.colChkInfo.damage);
            self->chargeTimer = 0;
            self->anim = 0;
            Animation_MorphToLoop(&self->skelAnime, (AnimationHeader*)gGanondorfFloatAnim, -6.0f);
            self->cooldown = std::max<s16>(self->cooldown, 25);
        }
    }

    if (self->actor.colChkInfo.health == 0) {
        // Defeated: stop fighting and drop.
        self->state = ST_FALL;
        self->orbScale = 0.0f;
        self->timer = 0;
        self->actor.velocity.y = 0.0f;
        self->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
        Actor_SetColorFilter(&self->actor, 0x4000, 255, 0, 40);
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_DAMAGE1);
    }
}

void UpdateFall(HsGanon* self, PlayState* play) {
    self->timer++;
    self->actor.velocity.y = std::max(self->actor.velocity.y - 1.6f, -22.0f);
    self->actor.world.pos.y += self->actor.velocity.y;
    self->actor.shape.rot.y += 0x1400;
    self->actor.shape.rot.x = static_cast<s16>(std::min<int>(self->actor.shape.rot.x + 0x300, 0x3000));
    Spark(play, self->actor.world.pos, 60.0f, self->timer % 2 == 0);
    if (self->timer % 3 == 0) {
        Vec3f pos = { self->actor.world.pos.x + Rand_CenteredFloat(50.0f), self->actor.world.pos.y + 40.0f,
                      self->actor.world.pos.z + Rand_CenteredFloat(50.0f) };
        Vec3f up = { 0.0f, 0.5f, 0.0f };
        EffectSsDeadDb_Spawn(play, &pos, &up, &up, 60, 0, 255, 120, 255, 255, 80, 0, 160, 1, 9, false);
    }

    if (self->actor.world.pos.y <= self->actor.floorHeight + 5.0f || self->timer >= kFallMaxFrames) {
        Explode(self, play, false);
    }
}

void SetBeastAnim(HsGanon* self, const char* anim, bool loop, f32 morph) {
    AnimationHeader* header = (AnimationHeader*)anim;
    if (loop) {
        Animation_MorphToLoop(&self->skelAnime, header, morph);
    } else {
        Animation_MorphToPlayOnce(&self->skelAnime, header, morph);
    }
}

void BeastStartWalk(HsGanon* self, s16 cooldown) {
    SetBeastAnim(self, gGanonGuardWalkAnim, true, -6.0f);
    self->state = ST_B_WALK;
    self->swingCooldown = cooldown;
    self->attackOn = false;
}

void BeastStartStun(HsGanon* self) {
    SetBeastAnim(self, gGanonStunStartAnim, false, -2.0f);
    self->state = ST_B_STUN;
    self->stunStage = 0;
    self->attackOn = false;
    self->actor.speedXZ = 0.0f;
    Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_DOWN);
}

void UpdateBeastRise(HsGanon* self, PlayState* play) {
    Math_ApproachF(&self->actor.scale.x, 0.01f, 0.2f, 0.0006f);
    self->actor.scale.y = self->actor.scale.z = self->actor.scale.x;
    Spark(play, self->actor.world.pos, 90.0f, false);
    Math_SmoothStepToS(&self->actor.shape.rot.y, self->actor.yawTowardsPlayer, 2, 0x400, 0x40);
    if (--self->timer <= 0) {
        Actor_SetScale(&self->actor, 0.01f);
        BeastStartWalk(self, 20);
    }
}

void UpdateBeastWalk(HsGanon* self, PlayState* play) {
    const f32 dist = self->actor.xzDistToPlayer;
    const s16 toLink = self->actor.yawTowardsPlayer;

    Math_SmoothStepToS(&self->actor.shape.rot.y, toLink, 3, 0x500, 0x40);
    self->actor.world.rot.y = self->actor.shape.rot.y;
    self->actor.speedXZ = (dist > kBeastStopDistance) ? kBeastWalkSpeed : 0.0f;
    if (self->swingCooldown > 0) {
        self->swingCooldown--;
    }
    if (self->actor.speedXZ > 0.0f && (play->gameplayFrames % 22) == 0) {
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_MGANON_WALK);
        Actor_RequestQuakeAndRumble(&self->actor, play, 2, 10);
    }

    if (dist < kBeastSwingRange && self->swingCooldown <= 0 &&
        ABS(static_cast<s16>(self->actor.shape.rot.y - toLink)) < 0x2000) {
        self->leftSwing = !self->leftSwing;
        const char* swing = self->leftSwing ? gGanonLeftSwordSwingAnim : gGanonRightSwordSwingAnim;
        SetBeastAnim(self, swing, false, -4.0f);
        self->timer = Animation_GetLastFrame((void*)swing);
        self->state = ST_B_SWING;
        self->actor.speedXZ = 0.0f;
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_THROW);
    }
}

void UpdateBeastSwing(HsGanon* self, PlayState* play) {
    self->actor.speedXZ = 0.0f;
    const f32 frame = self->skelAnime.curFrame;
    self->attackOn = frame > self->timer * 0.3f && frame < self->timer * 0.75f;
    if (self->attackOn) {
        Spark(play, self->swordPos, 40.0f, true);
        Spark(play, self->handPos, 40.0f, false);
    }
    if (self->animDone) {
        self->attackOn = false;
        // After two swings he is winded now and then: the moment to hit him.
        if (++self->swingCount >= 2) {
            self->swingCount = 0;
            if (Rand_ZeroOne() < 0.65f) {
                BeastStartStun(self);
                return;
            }
            BeastStartWalk(self, 40);
        } else {
            BeastStartWalk(self, 10);
        }
    }
}

void UpdateBeastStun(HsGanon* self, PlayState* play) {
    self->actor.speedXZ = 0.0f;
    switch (self->stunStage) {
        case 0:
            if (self->animDone) {
                self->stunStage = 1;
                self->timer = 110;
                SetBeastAnim(self, gGanonStunLoopAnim, true, 0.0f);
            }
            break;
        case 1:
            Spark(play, self->actor.world.pos, 60.0f, true);
            if (--self->timer <= 0) {
                self->stunStage = 2;
                SetBeastAnim(self, gGanonStunEndAnim, false, -5.0f);
            }
            break;
        case 2:
            if (self->animDone) {
                BeastStartWalk(self, 30);
            }
            break;
    }
}

void UpdateBeastDeath(HsGanon* self, PlayState* play) {
    self->actor.speedXZ = 0.0f;
    self->timer++;
    if (self->timer == 1) {
        SetBeastAnim(self, gGanonDeadStartAnim, false, -2.0f);
    } else if (self->animDone && self->timer < 70) {
        SetBeastAnim(self, gGanonDeadLoopAnim, true, 0.0f);
    }
    if (self->timer % 2 == 0) {
        Vec3f at = self->actor.world.pos;
        at.y += 90.0f;
        Spark(play, at, 110.0f, self->timer % 4 == 0);
    }
    if (self->timer % 10 == 0) {
        Actor_RequestQuakeAndRumble(&self->actor, play, 2, 10);
    }
    if (self->timer >= 80) {
        Explode(self, play, true);
    }
}

// Hits on the beast.
void BeastTakeHits(HsGanon* self, PlayState* play) {
    if (!(self->collider.base.acFlags & AC_HIT)) {
        return;
    }
    self->collider.base.acFlags &= ~AC_HIT;
    const int damage = self->actor.colChkInfo.damage;
    const bool light = self->actor.colChkInfo.damageEffect == 1;
    const bool down = self->state == ST_B_STUN && self->stunStage == 1;

    if (down) {
        if (damage > 0) {
            Hurt(self, damage);
        }
    } else if (light) {
        BeastStartStun(self);
        Hurt(self, std::max(damage, 3));
    } else {
        // Guarding: the blow glances off.
        Vec3f at = self->actor.world.pos;
        at.y += 70.0f;
        for (int i = 0; i < 4; i++) {
            Spark(play, at, 30.0f, true);
        }
        Audio_PlayActorSound2(&self->actor, NA_SE_IT_SHIELD_REFLECT_SW);
        self->acCooldown = 8;
    }

    if (self->actor.colChkInfo.health == 0 && self->state != ST_B_DEATH) {
        self->state = ST_B_DEATH;
        self->timer = 0;
        self->attackOn = false;
        self->actor.flags &= ~(ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE);
        Audio_PlayActorSound2(&self->actor, NA_SE_EN_GANON_DEAD);
    }
}

void Update(Actor* thisx, PlayState* play) {
    HsGanon* self = reinterpret_cast<HsGanon*>(thisx);

    self->animDone = SkelAnime_Update(&self->skelAnime) != 0;
    if (self->acCooldown > 0) {
        self->acCooldown--;
    }

    switch (self->state) {
        case ST_APPEAR:
            UpdateAppear(self, play);
            break;
        case ST_FIGHT:
            UpdateFight(self, play);
            break;
        case ST_FALL:
            UpdateFall(self, play);
            break;
        case ST_B_RISE:
            UpdateBeastRise(self, play);
            break;
        case ST_B_WALK:
            UpdateBeastWalk(self, play);
            BeastTakeHits(self, play);
            break;
        case ST_B_SWING:
            UpdateBeastSwing(self, play);
            BeastTakeHits(self, play);
            break;
        case ST_B_STUN:
            UpdateBeastStun(self, play);
            BeastTakeHits(self, play);
            break;
        case ST_B_DEATH:
            UpdateBeastDeath(self, play);
            break;
    }
    if (self->actor.update == nullptr) {
        return; // exploded this frame
    }

    if (self->beast) {
        Actor_MoveXZGravity(&self->actor);
    } else {
        UpdateBalls(self, play);
    }
    Actor_UpdateBgCheckInfo(play, &self->actor, 20.0f, 40.0f, 60.0f, 0x1D);
    self->actor.focus.pos = self->actor.world.pos;
    self->actor.focus.pos.y += self->beast ? 100.0f : 60.0f;

    if (self->state == ST_FIGHT) {
        Collider_UpdateCylinder(&self->actor, &self->collider);
        CollisionCheck_SetOC(play, &play->colChkCtx, &self->collider.base);
        CollisionCheck_SetAT(play, &play->colChkCtx, &self->collider.base);
        if (self->actor.colorFilterTimer == 0) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &self->collider.base);
        }
    } else if (self->beast && self->state != ST_B_RISE && self->state != ST_B_DEATH) {
        self->collider.dim.radius = self->attackOn ? kBeastAttackRadius : 45;
        Collider_UpdateCylinder(&self->actor, &self->collider);
        CollisionCheck_SetOC(play, &play->colChkCtx, &self->collider.base);
        if (self->attackOn) {
            CollisionCheck_SetAT(play, &play->colChkCtx, &self->collider.base);
        }
        if (self->actor.colorFilterTimer == 0 && self->acCooldown == 0) {
            CollisionCheck_SetAC(play, &play->colChkCtx, &self->collider.base);
        }
    }
}

} // namespace

// Outside the anonymous namespace on purpose: OPEN_DISPS declares FrameInterpolation_* inside the function, and a
// block-scope declaration binds to the enclosing namespace, which here would make them internal and unresolvable.
static s32 HsGanon_BeastOverride(PlayState* play, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot, void* thisx) {
    OPEN_DISPS(play->state.gfxCtx);

    // Mane, cape and tails: the same grey the original uses (the hands and their swords are kept).
    if (limbIndex >= 42) {
        gDPSetEnvColor(POLY_OPA_DISP++, 191, 191, 191, 255);
    }

    CLOSE_DISPS(play->state.gfxCtx);
    return 0;
}

static void HsGanon_BeastPost(PlayState* play, s32 limbIndex, Gfx** dList, Vec3s* rot, void* thisx) {
    HsGanon* self = reinterpret_cast<HsGanon*>(thisx);
    static Vec3f origin = { 0.0f, 0.0f, 0.0f };
    static Vec3f tip = { 0.0f, 0.0f, 14000.0f };

    if (limbIndex == 7) {
        Matrix_MultVec3f(&origin, &self->handPos);
        Matrix_MultVec3f(&tip, &self->swordPos);
    } else if (limbIndex == 13 && self->leftSwing) {
        Matrix_MultVec3f(&origin, &self->handPos);
        Matrix_MultVec3f(&tip, &self->swordPos);
    }
}

static s32 HsGanon_DorfOverride(PlayState* play, s32 limbIndex, Gfx** dList, Vec3f* pos, Vec3s* rot, void* thisx) {
    // The legs hang loose while he floats.
    if (limbIndex == 20 || limbIndex == 22 || limbIndex == 23 || limbIndex == 25) {
        rot->z += 0x500;
    }
    return 0;
}

static void HsGanon_DorfPost(PlayState* play, s32 limbIndex, Gfx** dList, Vec3s* rot, void* thisx) {
    HsGanon* self = reinterpret_cast<HsGanon*>(thisx);
    static Vec3f origin = { 0.0f, 0.0f, 0.0f };
    static Vec3f palm = { 1300.0f, 0.0f, 0.0f };

    if (limbIndex == 10) {
        Matrix_MultVec3f(&palm, &self->handPos);
    } else if (limbIndex == 11) {
        OPEN_DISPS(play->state.gfxCtx);

        gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
        gSPDisplayList(POLY_XLU_DISP++, (Gfx*)gGanondorfEyesDL);

        CLOSE_DISPS(play->state.gfxCtx);
    }
}

// A glowing ball of energy: the one he holds while charging and the ones he throws.
static void HsGanon_DrawOrb(PlayState* play, const Vec3f& at, f32 scale, s16 spin, bool bright) {
    OPEN_DISPS(play->state.gfxCtx);

    Matrix_Translate(at.x, at.y, at.z, MTXMODE_NEW);
    Matrix_ReplaceRotation(&play->billboardMtxF);
    Matrix_Scale(scale, scale, scale, MTXMODE_APPLY);
    Matrix_RotateZ(spin * (M_PI / 32768.0f), MTXMODE_APPLY);
    gSPMatrix(POLY_XLU_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_XLU_DISP++, (Gfx*)gGanondorfSquareDL);
    (void)bright;

    CLOSE_DISPS(play->state.gfxCtx);
}

static void HsGanon_DrawOrbs(HsGanon* self, PlayState* play) {
    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Xlu(play->state.gfxCtx);
    gSPDisplayList(POLY_XLU_DISP++, (Gfx*)gGanondorfLightBallMaterialDL);

    if (self->orbScale > 0.1f) {
        FrameInterpolation_RecordOpenChild("HiveShock Ganon hand orb", 0);
        gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 220, 255, 255);
        gDPSetEnvColor(POLY_XLU_DISP++, 160, 0, 255, 0);
        HsGanon_DrawOrb(play, self->handPos, self->orbScale, self->orbSpin, false);
        FrameInterpolation_RecordCloseChild();
    }
    for (int i = 0; i < kMaxBalls; i++) {
        const Ball& ball = self->balls[i];
        if (!ball.active) {
            continue;
        }
        FrameInterpolation_RecordOpenChild("HiveShock Ganon ball", i);
        if (ball.reflected) {
            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 255, 255, 255);
            gDPSetEnvColor(POLY_XLU_DISP++, 255, 230, 60, 0);
        } else {
            gDPSetPrimColor(POLY_XLU_DISP++, 0, 0, 255, 200, 255, 255);
            gDPSetEnvColor(POLY_XLU_DISP++, 170, 0, 255, 0);
        }
        HsGanon_DrawOrb(play, ball.pos, ball.reflected ? 6.5f : 5.0f, static_cast<s16>(ball.life * 0x1400), ball.reflected);
        FrameInterpolation_RecordCloseChild();
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

static void HsGanon_Draw(Actor* thisx, PlayState* play) {
    HsGanon* self = reinterpret_cast<HsGanon*>(thisx);

    OPEN_DISPS(play->state.gfxCtx);

    Gfx_SetupDL_25Opa(play->state.gfxCtx);
    Gfx_SetupDL_25Xlu(play->state.gfxCtx);
    if (self->beast) {
        gSPSegment(POLY_OPA_DISP++, 0x08, SEGMENTED_TO_VIRTUAL((uintptr_t)gGanonEyeOpenTex));
        SkelAnime_DrawSkeletonOpa(play, &self->skelAnime, HsGanon_BeastOverride, HsGanon_BeastPost, &self->actor);
    } else {
        gSPSegment(POLY_OPA_DISP++, 0x08, SEGMENTED_TO_VIRTUAL((uintptr_t)gGanondorfNormalEyeTex));
        gSPSegment(POLY_XLU_DISP++, 0x08, SEGMENTED_TO_VIRTUAL((uintptr_t)gGanondorfNormalEyeTex));
        SkelAnime_DrawSkeletonOpa(play, &self->skelAnime, HsGanon_DorfOverride, HsGanon_DorfPost, &self->actor);
        HsGanon_DrawOrbs(self, play);
    }

    CLOSE_DISPS(play->state.gfxCtx);
}

namespace {

bool sRegistered = false;
int sActorId = -1;

} // namespace

int HiveShockGanon_ActorId() {
    if (!sRegistered) {
        ActorDBInit entry;
        entry.name = "HiveShock_Ganon";
        entry.desc = "HiveShock Ganon";
        entry.id = -1;
        entry.category = ACTORCAT_ENEMY;
        entry.flags = ACTOR_FLAG_ATTENTION_ENABLED | ACTOR_FLAG_HOSTILE | ACTOR_FLAG_UPDATE_CULLING_DISABLED |
                      ACTOR_FLAG_DRAW_CULLING_DISABLED;
        entry.objectId = OBJECT_GANON;
        entry.instanceSize = sizeof(HsGanon);
        entry.init = Init;
        entry.destroy = Destroy;
        entry.update = Update;
        entry.draw = HsGanon_Draw;
        sActorId = ActorDB::Instance->AddEntry(entry).entry.id;
        sRegistered = true;
    }
    return sActorId;
}

#else

int HiveShockGanon_ActorId() {
    return -1;
}

#endif
