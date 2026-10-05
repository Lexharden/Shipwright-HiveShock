#pragma once

// HiveShock spawn admission rules, kept as pure constexpr functions (no game state) so they can be checked at
// compile time with the static_asserts at the bottom.
//
// Two lanes share the game: ordinary enemies use the "pool" (MaxLoad points); elite enemies get a head start of their
// own (eliteReserve points, on top of the pool) so the first ones never wait. The bank is not a ceiling: when it is
// full an elite may overflow ("spill") into the pool's free room, down to the last `ordinaryFloor` points (0 = the
// whole pool). The caller adds the other half of the policy: while an elite waits, the room that frees up as ordinary
// enemies die is reserved for it, so it accumulates kill after kill until the elite fits.
//
// Nothing here ever removes an enemy: when a lane has no room the spawn simply waits.

namespace HiveShockAdmission {

enum class Lane { Ordinary, Elite };

struct Config {
    int maxLoad;       // ordinary pool, in weight points
    int eliteReserve;  // elite bank, in weight points, on top of maxLoad. 0 = no bank (everyone shares the pool)
    int ordinaryFloor; // points of the pool that elite overflow can never take (0 = elites may use all of it)
};

struct Loads {
    int ordinary; // sum of the weights of the live ordinary enemies
    int elite;    // sum of the weights of the live elite enemies
};

constexpr int Max(int a, int b) {
    return a > b ? a : b;
}

// Elite weight that does not fit in the bank and is therefore using the pool.
constexpr int Spill(const Loads& loads, const Config& config) {
    return loads.elite > config.eliteReserve ? loads.elite - config.eliteReserve : 0;
}

// Points of the ordinary pool in use: ordinary enemies plus elite overflow.
constexpr int PoolUsed(const Loads& loads, const Config& config) {
    return loads.ordinary + Spill(loads, config);
}

// The floor never takes more than half of a small pool.
constexpr int EffectiveFloor(const Config& config) {
    return config.ordinaryFloor < config.maxLoad / 2 ? config.ordinaryFloor : config.maxLoad / 2;
}

// True when an enemy of this weight fits in its lane right now. An enemy that would not fit even in an empty game
// is still allowed on its own, so it can never wait forever.
constexpr bool CanAdmit(Lane lane, int weight, const Loads& loads, const Config& config) {
    if (loads.ordinary + loads.elite == 0) {
        return true;
    }
    if (lane == Lane::Ordinary) {
        return PoolUsed(loads, config) + weight <= config.maxLoad;
    }
    const int newElite = loads.elite + weight;
    if (newElite <= config.eliteReserve) {
        return true; // fits in the bank
    }
    const int newSpill = newElite - config.eliteReserve;
    return loads.ordinary + newSpill <= config.maxLoad - EffectiveFloor(config);
}

// Slots kept free for elite enemies in the "alive" and "in the scene" caps, so a field full of ordinary enemies
// cannot block an elite by headcount alone. About one elite per `eliteWeight` points of the bank.
constexpr int EliteSlots(int eliteReserve, int eliteWeight) {
    return eliteReserve <= 0 ? 0 : (eliteReserve + eliteWeight - 1) / eliteWeight;
}

// Headcount caps: elite enemies may use the whole cap, ordinary ones leave `eliteSlots` free.
constexpr bool CanAdmitSlots(Lane lane, int alive, int inScene, int maxAlive, int maxScene, int eliteSlots) {
    const int slots = lane == Lane::Elite ? 0 : eliteSlots;
    return alive < Max(1, maxAlive - slots) && inScene < Max(1, maxScene - slots);
}

// ---- Checks (MaxLoad 14, elite bank 10, no floor) ---------------------------------------------------------------
namespace Checks {
constexpr Config kBank{ 14, 10, 0 };
constexpr Config kNoBank{ 14, 0, 0 };
constexpr Config kFloor{ 14, 10, 4 };

// Seven wolves fill the ordinary pool, yet the first two Iron Knuckles (4 points each) enter at once...
static_assert(CanAdmit(Lane::Elite, 4, Loads{ 14, 0 }, kBank));
static_assert(CanAdmit(Lane::Elite, 4, Loads{ 14, 4 }, kBank));
// ...the third does not fit in the bank (12 > 10) and the pool has no room for its overflow, so it waits.
static_assert(!CanAdmit(Lane::Elite, 4, Loads{ 14, 8 }, kBank));
// The room that frees up as enemies die is reserved for the waiting elite and piles up kill after kill: after a
// 1-point kill it still does not fit (13 + 2 > 14), after a second one it does (12 + 2 <= 14). No need to kill several
// enemies "at once", and no other enemy takes the room in between.
static_assert(!CanAdmit(Lane::Elite, 4, Loads{ 13, 8 }, kBank));
static_assert(CanAdmit(Lane::Elite, 4, Loads{ 12, 8 }, kBank));
// The bank is not a ceiling: with it full, elites keep coming in as long as the pool has room.
static_assert(CanAdmit(Lane::Elite, 4, Loads{ 8, 10 }, kBank));
static_assert(CanAdmit(Lane::Elite, 4, Loads{ 2, 16 }, kBank));
// A configurable floor still protects the last points of the ordinary pool.
static_assert(!CanAdmit(Lane::Elite, 4, Loads{ 9, 8 }, kFloor));
static_assert(CanAdmit(Lane::Elite, 4, Loads{ 8, 8 }, kFloor));
// Ordinary enemies are not affected by a full bank...
static_assert(CanAdmit(Lane::Ordinary, 2, Loads{ 12, 10 }, kBank));
// ...and still cannot exceed their own pool.
static_assert(!CanAdmit(Lane::Ordinary, 2, Loads{ 14, 0 }, kBank));
// Elite overflow counts against the pool.
static_assert(CanAdmit(Lane::Ordinary, 6, Loads{ 6, 12 }, kBank));
static_assert(!CanAdmit(Lane::Ordinary, 8, Loads{ 6, 12 }, kBank));
// Without a bank everything shares the pool, exactly like before.
static_assert(CanAdmit(Lane::Elite, 4, Loads{ 10, 0 }, kNoBank));
static_assert(!CanAdmit(Lane::Elite, 4, Loads{ 12, 0 }, kNoBank));
static_assert(CanAdmit(Lane::Ordinary, 4, Loads{ 6, 4 }, kNoBank));
static_assert(!CanAdmit(Lane::Ordinary, 6, Loads{ 6, 4 }, kNoBank));
// An enemy heavier than everything is still allowed alone.
static_assert(CanAdmit(Lane::Elite, 40, Loads{ 0, 0 }, kBank));
static_assert(CanAdmit(Lane::Ordinary, 40, Loads{ 0, 0 }, kBank));

static_assert(EliteSlots(10, 3) == 4);
static_assert(EliteSlots(0, 3) == 0);
// 32 alive: ordinary enemies stop at 28, elite ones can use all 32.
static_assert(!CanAdmitSlots(Lane::Ordinary, 28, 10, 32, 40, 4));
static_assert(CanAdmitSlots(Lane::Elite, 28, 10, 32, 40, 4));
static_assert(!CanAdmitSlots(Lane::Elite, 32, 10, 32, 40, 4));
} // namespace Checks

} // namespace HiveShockAdmission
