// soh/soh/Network/HiveShock/HiveShock.cpp
//
// Native TCP listen server compiled into soh.exe for the HiveShock companion.
// Accepts JSON commands (one object per line) on 127.0.0.1:43000 and applies
// them on the game thread.

#if !defined(__SWITCH__) && !defined(__WIIU__) && defined(_WIN32)
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif
#endif

#include "HiveShock.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/Enhancements/nametag.h"
#include "soh/SaveManager.h"
#include "soh/ShipInit.hpp"
#include "soh/cvar_prefixes.h"
#include "overlays/actors/ovl_En_Niw/z_en_niw.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/ConsoleWindow.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(__SWITCH__) && !defined(__WIIU__)

#ifndef _WIN32
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

extern "C" {
#include <z64.h>
#include "variables.h"
#include "functions.h"
#include "macros.h"
extern PlayState* gPlayState;
extern u16 gTimeSpeed;
}

#define CVAR_NAME CVAR_REMOTE_HIVESHOCK("Enabled")
#define CVAR_ENABLED CVarGetInteger(CVAR_NAME, 0)

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr int kDefaultPort = 43000;
constexpr uint16_t kEventPort = 43002;
constexpr int kPollIntervalMs = 200;
constexpr size_t kMaxLineBytes = 8192;
constexpr size_t kMaxQueueSize = 128;
constexpr size_t kMaxPendingSpawns = 128;
constexpr size_t kMaxNameTagChars = 18;
constexpr s16 kMinHealthAfterDamage = 4;
constexpr float kDefaultShockDamage = 8.0f;
// PC collider lists are 150/180/150; keep a margin for the scene + Link.
constexpr s32 kMaxBridgeEnemies = 32;
constexpr s32 kMaxSceneEnemies = 40;
constexpr s32 kSpawnsPerFrame = 4;
// Gameplay runs at 20 frames/s. A spawn held back by a fight/cutscene is retried every half second for up to a minute.
constexpr u32 kDeferRecheckFrames = 10;
constexpr u32 kMaxSpawnWaitFrames = 60 * 20;
constexpr u32 kCuccoArmyFrameGap = 4;
constexpr f32 kDefaultSpeedMult = 2.0f;
constexpr f32 kDefaultSlowMult = 0.5f;
constexpr f32 kDefaultDamageMult = 3.0f;
constexpr float kDefaultBuffSeconds = 20.0f;

#ifdef _WIN32
using SocketHandle = SOCKET;
constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
using SocketHandle = int;
constexpr SocketHandle kInvalidSocket = -1;
#endif

// Applied: done. Deferred: not possible right now, try again later. Skipped: failed this time, may retry a few times.
// Rejected: never valid here (e.g. a boss room), drop it.
enum class ApplyResult { Applied, Deferred, Skipped, Rejected };

enum class BridgeBuff : u8 {
    Speed = 0,
    Invincible,
    InfiniteMagic,
    DamageUp,
    Count,
};

struct BuffState {
    bool active = false;
    Clock::time_point until{};
    f32 strength = 1.0f;
};

struct SpawnDef {
    const char* name;
    s16 actorId;
    s32 params;
    bool useSpawnActor;
};

struct WarpAlias {
    const char* name;
    s32 entrance;
};

struct PendingSpawn {
    std::string name;
    std::string user;
    u32 earliestFrame = 0;
    u32 deadlineFrame = 0; // set the first time a spawn is deferred; 0 = not deferred yet
    u8 retries = 0;
};

struct TrackedEnemy {
    Actor* actor = nullptr;
    u32 spawnFrame = 0;
};

std::mutex gQueueMutex;
std::deque<json> gCommandQueue;

std::atomic<bool> gServerRun{ false };
std::atomic<bool> gListening{ false };
std::atomic<bool> gClientConnected{ false };
std::atomic<int> gDeathCount{ 0 };
std::thread gServerThread;
std::atomic<uint16_t> gPort{ kDefaultPort };

bool gDeathCountedThisCycle = false;
u16 gSavedTimeSpeed = 0;
bool gTimeIsFrozen = false;
bool gTimeFreezeHasDeadline = false;
Clock::time_point gTimeFrozenUntil{};
bool gInvertControls = false;
Clock::time_point gInvertUntil{};
BuffState gBuffs[static_cast<size_t>(BridgeBuff::Count)]{};

std::deque<PendingSpawn> gPendingSpawns;
std::vector<TrackedEnemy> gTrackedEnemies;

constexpr SpawnDef kSpawnTable[] = {
    { "enemy", ACTOR_EN_RR, 0, false },
    { "like_like", ACTOR_EN_RR, 0, false },
    { "guay", ACTOR_EN_CROW, 0, false },
    { "keese", ACTOR_EN_FIREFLY, 2, false },
    { "fire_keese", ACTOR_EN_FIREFLY, 1, false },
    { "ice_keese", ACTOR_EN_FIREFLY, 4, false },
    { "chuchu", ACTOR_EN_TITE, 0, false },
    { "redead", ACTOR_EN_RD, 0, false },
    { "gibdo", ACTOR_EN_RD, 32766, false },
    { "wolfos", ACTOR_EN_WF, (0xFF << 8), false },
    { "white_wolfos", ACTOR_EN_WF, (0xFF << 8) | 1, false },
    { "stalchild", ACTOR_EN_SKB, 0, false },
    { "stalfos", ACTOR_EN_TEST, 2, false },
    { "bubble", ACTOR_EN_BB, 0, false },
    { "skulltula", ACTOR_EN_ST, 0, false },
    { "bat", ACTOR_EN_FIREFLY, 2, false },
    { "tektite", ACTOR_EN_TITE, 0, false },
    { "blue_tektite", ACTOR_EN_TITE, -1, false },
    { "freezard", ACTOR_EN_FZ, 0, false },
    { "cucco", ACTOR_EN_NIW, 0, true },
    { "dinolfos", ACTOR_EN_ZF, (0xFF << 8) | 0xFE, false },
    { "lizalfos", ACTOR_EN_ZF, (0xFF << 8) | 0xFF, false },
    { "iron_knuckle", ACTOR_EN_IK, 2, false },
    { "garo", ACTOR_EN_TEST, 2, false },
    { "garo_master", ACTOR_EN_IK, 2, false },
    { "wallmaster", ACTOR_EN_WALLMAS, 0, false },
    { "floormaster", ACTOR_EN_FLOORMAS, 0, false },
    { "eyegore", ACTOR_EN_IK, 2, false },
    { "dark_link", ACTOR_EN_TORCH2, 0, false },
    { "arwing", ACTOR_EN_CLEAR_TAG, 1, false },
    { "fairy", ACTOR_EN_ELF, 0, true },
    { "bombchu", ACTOR_EN_BOM_CHU, 0, true },
};

constexpr const char* kRandomEnemies[] = {
    "guay",      "keese",    "fire_keese", "ice_keese", "tektite",  "redead",   "wolfos",       "stalchild",   "bubble",
    "skulltula", "freezard", "like_like",  "stalfos",   "dinolfos", "lizalfos", "iron_knuckle", "floormaster",
};

constexpr WarpAlias kWarpAliases[] = {
    { "home", ENTR_LINKS_HOUSE_CHILD_SPAWN },
    { "links_house", ENTR_LINKS_HOUSE_CHILD_SPAWN },
    { "minuet", ENTR_SACRED_FOREST_MEADOW_WARP_PAD },
    { "forest", ENTR_SACRED_FOREST_MEADOW_WARP_PAD },
    { "woodfall", ENTR_SACRED_FOREST_MEADOW_WARP_PAD },
    { "sacred_forest_meadow", ENTR_SACRED_FOREST_MEADOW_WARP_PAD },
    { "bolero", ENTR_DEATH_MOUNTAIN_CRATER_WARP_PAD },
    { "crater", ENTR_DEATH_MOUNTAIN_CRATER_WARP_PAD },
    { "snowhead", ENTR_DEATH_MOUNTAIN_CRATER_WARP_PAD },
    { "death_mountain", ENTR_DEATH_MOUNTAIN_CRATER_WARP_PAD },
    { "serenade", ENTR_LAKE_HYLIA_WARP_PAD },
    { "lake", ENTR_LAKE_HYLIA_WARP_PAD },
    { "lake_hylia", ENTR_LAKE_HYLIA_WARP_PAD },
    { "great_bay", ENTR_LAKE_HYLIA_WARP_PAD },
    { "requiem", ENTR_DESERT_COLOSSUS_WARP_PAD },
    { "desert", ENTR_DESERT_COLOSSUS_WARP_PAD },
    { "colossus", ENTR_DESERT_COLOSSUS_WARP_PAD },
    { "ikana", ENTR_DESERT_COLOSSUS_WARP_PAD },
    { "stone_tower", ENTR_DESERT_COLOSSUS_WARP_PAD },
    { "nocturne", ENTR_GRAVEYARD_WARP_PAD },
    { "graveyard", ENTR_GRAVEYARD_WARP_PAD },
    { "prelude", ENTR_TEMPLE_OF_TIME_WARP_PAD },
    { "temple", ENTR_TEMPLE_OF_TIME_WARP_PAD },
    { "temple_of_time", ENTR_TEMPLE_OF_TIME_WARP_PAD },
    { "clock_town", ENTR_TEMPLE_OF_TIME_WARP_PAD },
};

constexpr s32 kRandomWarps[] = {
    ENTR_LINKS_HOUSE_CHILD_SPAWN, ENTR_SACRED_FOREST_MEADOW_WARP_PAD, ENTR_DEATH_MOUNTAIN_CRATER_WARP_PAD,
    ENTR_LAKE_HYLIA_WARP_PAD,     ENTR_DESERT_COLOSSUS_WARP_PAD,      ENTR_GRAVEYARD_WARP_PAD,
    ENTR_TEMPLE_OF_TIME_WARP_PAD,
};

const SpawnDef* FindSpawnDef(const std::string& name);

float GetNumber(const json& cmd, float defaultValue) {
    if (cmd.contains("strength") && cmd["strength"].is_number()) {
        return cmd["strength"].get<float>();
    }
    if (cmd.contains("value") && cmd["value"].is_number()) {
        return cmd["value"].get<float>();
    }
    return defaultValue;
}

float GetDuration(const json& cmd) {
    if (cmd.contains("duration") && cmd["duration"].is_number()) {
        return std::max(0.0f, cmd["duration"].get<float>());
    }
    return 0.0f;
}

std::string GetStringField(const json& cmd, const char* key) {
    if (cmd.contains(key) && cmd[key].is_string()) {
        return cmd[key].get<std::string>();
    }
    return "";
}

void LogUser(const json& cmd, const std::string& action) {
    std::string user = GetStringField(cmd, "user");
    if (!user.empty()) {
        SPDLOG_INFO("[HiveShock] action '{}' from '{}'", action, user);
    }
}

const char* BuffName(BridgeBuff buff) {
    switch (buff) {
        case BridgeBuff::Speed:
            return "speed";
        case BridgeBuff::Invincible:
            return "invincible";
        case BridgeBuff::InfiniteMagic:
            return "infinite_magic";
        case BridgeBuff::DamageUp:
            return "damage_up";
        default:
            return "buff";
    }
}

bool IsBuffActive(BridgeBuff buff) {
    const BuffState& state = gBuffs[static_cast<size_t>(buff)];
    return state.active && Clock::now() < state.until;
}

f32 GetBuffStrength(BridgeBuff buff, f32 fallback) {
    if (!IsBuffActive(buff)) {
        return fallback;
    }
    return gBuffs[static_cast<size_t>(buff)].strength;
}

float CurrentOutgoingDamageMultiplier() {
    return GetBuffStrength(BridgeBuff::DamageUp, 1.0f);
}

void CloseSocket(SocketHandle socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

// Initializes the platform socket layer for the lifetime of the object (Winsock refcounts per call).
struct NetSession {
    bool ok = true;
#ifdef _WIN32
    NetSession() {
        WSADATA wsaData;
        ok = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    }
    ~NetSession() {
        if (ok) {
            WSACleanup();
        }
    }
#endif
};

int LastSocketError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool LastSocketErrorIsTransient() {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEINTR || err == WSAEWOULDBLOCK;
#else
    return errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNABORTED;
#endif
}

bool ConnectInProgress() {
#ifdef _WIN32
    int err = WSAGetLastError();
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
    return errno == EINPROGRESS || errno == EINTR;
#endif
}

// Per-socket options. macOS/BSD have no MSG_NOSIGNAL, so a write to a closed peer would raise SIGPIPE and
// kill the whole game; SO_NOSIGPIPE turns that into a plain EPIPE error instead.
void ConfigureSocket(SocketHandle sock) {
#ifdef SO_NOSIGPIPE
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)sock;
#endif
}

void SetNonBlocking(SocketHandle sock, bool enable) {
#ifdef _WIN32
    u_long mode = enable ? 1 : 0;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sock, F_SETFL, enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
    }
#endif
}

// Waits until the socket is readable (or, if !forRead, writable). poll() has no FD_SETSIZE limit,
// unlike select(), which matters on macOS where fds can exceed 1024 in a game process.
bool WaitForSocket(SocketHandle sock, bool forRead, int timeoutMs) {
#ifdef _WIN32
    WSAPOLLFD pfd{};
    pfd.fd = sock;
    pfd.events = forRead ? POLLRDNORM : POLLWRNORM;
    return WSAPoll(&pfd, 1, timeoutMs) > 0;
#else
    pollfd pfd{};
    pfd.fd = sock;
    pfd.events = forRead ? POLLIN : POLLOUT;
    return poll(&pfd, 1, timeoutMs) > 0;
#endif
}

void SendAll(SocketHandle sock, const std::string& data) {
#ifdef MSG_NOSIGNAL
    constexpr int flags = MSG_NOSIGNAL;
#else
    constexpr int flags = 0;
#endif
    size_t sent = 0;
    while (sent < data.size()) {
        auto n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), flags);
        if (n <= 0) {
            return;
        }
        sent += static_cast<size_t>(n);
    }
}

sockaddr_in LoopbackAddress(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

void PushCommand(json cmd) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gCommandQueue.size() >= kMaxQueueSize) {
        SPDLOG_WARN("[HiveShock] queue full ({}), dropping oldest command", kMaxQueueSize);
        gCommandQueue.pop_front();
    }
    gCommandQueue.push_back(std::move(cmd));
}

void PushCommandFront(json cmd) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gCommandQueue.size() >= kMaxQueueSize) {
        SPDLOG_WARN("[HiveShock] queue full ({}), dropping newest command", kMaxQueueSize);
        gCommandQueue.pop_back();
    }
    gCommandQueue.push_front(std::move(cmd));
}

bool PopCommand(json& out) {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    if (gCommandQueue.empty()) {
        return false;
    }
    out = std::move(gCommandQueue.front());
    gCommandQueue.pop_front();
    return true;
}

void ClearCommandQueue() {
    std::lock_guard<std::mutex> lock(gQueueMutex);
    gCommandQueue.clear();
}

ApplyResult FromGi(GameInteractionEffectQueryResult result) {
    if (result == GameInteractionEffectQueryResult::Possible) {
        return ApplyResult::Applied;
    }
    if (result == GameInteractionEffectQueryResult::TemporarilyNotPossible) {
        return ApplyResult::Deferred;
    }
    return ApplyResult::Skipped;
}

bool IsDangerousAction(const std::string& action) {
    if (FindSpawnDef(action) != nullptr) {
        return true;
    }
    return action == "bomb" || action == "knockback" || action == "impulse" || action == "damage" ||
           action == "insta_kill" || action == "instakill" || action == "kill" || action == "random_enemy" ||
           action == "cucco_army" || action == "blast" || action == "rupee_rain" || action == "freeze" ||
           action == "shock" || action == "fire" || action == "delete_save" || action == "new_run" ||
           action == "clear_enemies" || action == "spawn" || action == "spawn_enemy" || action == "spawn_actor";
}

bool IsSceneChangeInProgress() {
    if (gPlayState == nullptr) {
        return true;
    }
    if (gPlayState->transitionTrigger != TRANS_TRIGGER_OFF) {
        return true;
    }
    if (gPlayState->transitionMode != TRANS_MODE_OFF) {
        return true;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player != nullptr && (player->stateFlags1 & PLAYER_STATE1_DEAD)) {
        return true;
    }
    return false;
}

bool ShouldDeferDangerousEffects() {
    return !GameInteractor::IsPlayerInControl() || IsSceneChangeInProgress();
}

void ActivateBuff(BridgeBuff buff, float durationSeconds, f32 strength) {
    durationSeconds = std::clamp(durationSeconds, 1.0f, 120.0f);
    BuffState& state = gBuffs[static_cast<size_t>(buff)];
    state.active = true;
    state.until =
        Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(durationSeconds));
    state.strength = strength;
    SPDLOG_INFO("[HiveShock] buff '{}' {:.1f}s (strength={:.2f})", BuffName(buff), durationSeconds, strength);
}

void ClearExpiredBuffs() {
    const auto now = Clock::now();
    for (size_t i = 0; i < static_cast<size_t>(BridgeBuff::Count); i++) {
        if (gBuffs[i].active && now >= gBuffs[i].until) {
            BridgeBuff buff = static_cast<BridgeBuff>(i);
            gBuffs[i].active = false;
            if (buff == BridgeBuff::Speed) {
                GameInteractor::State::MovementSpeedMultiplier = 1.0f;
            }
            if (buff == BridgeBuff::Invincible) {
                GameInteractor::RawAction::SetPlayerInvincibility(false);
            }
            SPDLOG_INFO("[HiveShock] buff '{}' ended", BuffName(buff));
        }
    }
}

float BuffDurationFromCmd(const json& cmd) {
    float duration = GetDuration(cmd);
    if (duration <= 0.0f) {
        duration = kDefaultBuffSeconds;
    }
    return duration;
}

void TickInfiniteMagic() {
    if (!IsBuffActive(BridgeBuff::InfiniteMagic) || !gSaveContext.isMagicAcquired) {
        return;
    }
    int8_t cap = static_cast<int8_t>((gSaveContext.isDoubleMagicAcquired + 1) * MAGIC_NORMAL_METER);
    gSaveContext.magic = cap;
}

void TickInvincible() {
    if (IsBuffActive(BridgeBuff::Invincible) && gPlayState != nullptr) {
        GameInteractor::RawAction::SetPlayerInvincibility(true);
    }
}

void ResumeTime() {
    if (gSavedTimeSpeed != 0) {
        gTimeSpeed = gSavedTimeSpeed;
    } else if (gPlayState != nullptr && gPlayState->envCtx.timeIncrement != 0) {
        gTimeSpeed = gPlayState->envCtx.timeIncrement;
    } else {
        gTimeSpeed = 1;
    }
    gTimeIsFrozen = false;
    gTimeFreezeHasDeadline = false;
    SPDLOG_INFO("[HiveShock] time resumed (gTimeSpeed={})", (int)gTimeSpeed);
}

void FreezeTime(float durationSeconds) {
    if (!gTimeIsFrozen) {
        gSavedTimeSpeed = gTimeSpeed;
    }
    gTimeSpeed = 0;
    gTimeIsFrozen = true;
    if (durationSeconds > 0.0f) {
        gTimeFrozenUntil =
            Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(durationSeconds));
        gTimeFreezeHasDeadline = true;
        SPDLOG_INFO("[HiveShock] time frozen for {:.1f}s", durationSeconds);
    } else {
        gTimeFreezeHasDeadline = false;
        SPDLOG_INFO("[HiveShock] time frozen until resume_time");
    }
}

// Sends one JSON line to the companion's event port. Fire-and-forget on a short-lived thread.
void SendEventToBridge(const json& event) {
    std::string line = event.dump() + "\n";
    std::thread([line = std::move(line)]() {
        [[maybe_unused]] NetSession session; // WSAStartup/WSACleanup on Windows, no-op elsewhere
        SocketHandle sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock == kInvalidSocket) {
            return;
        }
        ConfigureSocket(sock);
        SetNonBlocking(sock, true);

        sockaddr_in addr = LoopbackAddress(kEventPort);
        int result = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (result != 0 && !ConnectInProgress()) {
            CloseSocket(sock);
            return;
        }
        if (result != 0 && !WaitForSocket(sock, false, 200)) {
            CloseSocket(sock);
            return;
        }
        int soError = 0;
        socklen_t soLen = sizeof(soError);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &soLen) != 0 || soError != 0) {
            CloseSocket(sock);
            return;
        }

        SetNonBlocking(sock, false);
        SendAll(sock, line);
        CloseSocket(sock);
    }).detach();
}

void NotifyDeathCount() {
    json event = { { "event", "player_death" }, { "deaths", gDeathCount.load() } };
    SendEventToBridge(event);
}

void NotifyDeathsReset(const char* reason) {
    json event = { { "event", reason }, { "deaths", 0 } };
    SendEventToBridge(event);
}

void RegisterDeath(const char* reason) {
    int deaths = ++gDeathCount;
    gDeathCountedThisCycle = true;
    SPDLOG_INFO("[HiveShock] {} #{}", reason, deaths);
    NotifyDeathCount();
}

void ClampMinHealthAfterHit() {
    if (gSaveContext.health < kMinHealthAfterDamage) {
        gSaveContext.health = kMinHealthAfterDamage;
    }
}

std::string TruncateDonorName(const std::string& user) {
    if (user.empty()) {
        return "";
    }
    std::string out;
    out.reserve(kMaxNameTagChars);
    for (unsigned char c : user) {
        if (out.size() >= kMaxNameTagChars) {
            break;
        }
        if (c >= 32 && c < 127) {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

std::string NormalizeActionName(std::string name) {
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (c == '-' || c == ' ') {
            out.push_back('_');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_') {
            out.push_back(static_cast<char>(c));
        }
    }
    // Common companion aliases that do not match the table name 1:1.
    if (out == "likelike" || out == "like") {
        return "like_like";
    }
    if (out == "darklink" || out == "shadowlink" || out == "shadow_link") {
        return "dark_link";
    }
    if (out == "ironknuckle" || out == "knuckle") {
        return "iron_knuckle";
    }
    if (out == "firekeese") {
        return "fire_keese";
    }
    if (out == "icekeese") {
        return "ice_keese";
    }
    if (out == "whitewolfos") {
        return "white_wolfos";
    }
    if (out == "bluetektite") {
        return "blue_tektite";
    }
    if (out == "randomenemy" || out == "random") {
        return "random_enemy";
    }
    if (out == "cuccoarmy" || out == "cuccostorm" || out == "cucco_storm") {
        return "cucco_army";
    }
    return out;
}

const SpawnDef* FindSpawnDef(const std::string& name) {
    std::string key = NormalizeActionName(name);
    if (key.rfind("spawn_", 0) == 0) {
        key = key.substr(6);
    }
    for (const SpawnDef& def : kSpawnTable) {
        if (key == def.name) {
            return &def;
        }
    }
    return nullptr;
}

bool IsTrackedSpawnName(const std::string& name) {
    return name != "fairy" && name != "bombchu";
}

bool ActorStillLoaded(Actor* actor) {
    if (actor == nullptr || actor->update == nullptr || gPlayState == nullptr) {
        return false;
    }
    if (actor->category >= ACTORCAT_MAX) {
        return false;
    }
    for (Actor* it = gPlayState->actorCtx.actorLists[actor->category].head; it != nullptr; it = it->next) {
        if (it == actor) {
            return true;
        }
    }
    return false;
}

s32 CountSceneEnemies() {
    if (gPlayState == nullptr) {
        return 0;
    }
    return gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].length;
}

void PruneTrackedEnemies() {
    gTrackedEnemies.erase(std::remove_if(gTrackedEnemies.begin(), gTrackedEnemies.end(),
                                         [](const TrackedEnemy& t) { return !ActorStillLoaded(t.actor); }),
                          gTrackedEnemies.end());
}

size_t CountLiveTrackedEnemies() {
    PruneTrackedEnemies();
    return gTrackedEnemies.size();
}

void DeleteOldestBridgeEnemy() {
    PruneTrackedEnemies();
    if (gTrackedEnemies.empty()) {
        return;
    }
    auto oldest =
        std::min_element(gTrackedEnemies.begin(), gTrackedEnemies.end(),
                         [](const TrackedEnemy& a, const TrackedEnemy& b) { return a.spawnFrame < b.spawnFrame; });
    if (ActorStillLoaded(oldest->actor)) {
        Actor_Kill(oldest->actor);
    }
    gTrackedEnemies.erase(oldest);
}

void EnsureActorHeadroom() {
    PruneTrackedEnemies();
    while (!gTrackedEnemies.empty() &&
           (CountLiveTrackedEnemies() >= kMaxBridgeEnemies || CountSceneEnemies() >= kMaxSceneEnemies)) {
        DeleteOldestBridgeEnemy();
    }
}

void TrackBridgeEnemy(Actor* actor) {
    if (actor == nullptr) {
        return;
    }
    EnsureActorHeadroom();
    u32 now = gPlayState->gameplayFrames;
    gTrackedEnemies.push_back(TrackedEnemy{ actor, now });
}

void ClearBridgeEnemies() {
    PruneTrackedEnemies();
    for (TrackedEnemy& t : gTrackedEnemies) {
        if (ActorStillLoaded(t.actor)) {
            Actor_Kill(t.actor);
        }
    }
    gTrackedEnemies.clear();
    SPDLOG_INFO("[HiveShock] cleared bridge enemies");
}

void ResetBridgeActorTracking() {
    gTrackedEnemies.clear();
}

void EnqueueEnemySpawn(std::string name, std::string user, u32 delayFrames) {
    while (gPendingSpawns.size() >= kMaxPendingSpawns) {
        SPDLOG_WARN("[HiveShock] pending spawn queue full, dropping oldest");
        gPendingSpawns.pop_front();
    }
    PendingSpawn pending;
    pending.name = std::move(name);
    pending.user = std::move(user);
    pending.earliestFrame = (gPlayState != nullptr) ? (gPlayState->gameplayFrames + delayFrames) : delayFrames;
    gPendingSpawns.push_back(std::move(pending));
}

Actor* SpawnActorInAnyRoom(s16 actorId, f32 x, f32 y, f32 z, s16 rotX, s16 rotY, s16 rotZ, s32 params) {
    // Actor_Spawn refuses enemies in a cleared room, so drop the flag for the duration of the call. The bit is
    // toggled directly (not via Flags_Set/UnsetClear) so no scene-flag hooks fire: the randomizer tracker and
    // similar listeners must not see a fake "room cleared" event for every spawn.
    s32 roomNum = gPlayState->roomCtx.curRoom.num;
    const bool roomInRange = roomNum >= 0 && roomNum < 32;
    const u32 roomBit = roomInRange ? (1u << roomNum) : 0;
    const bool wasCleared = roomInRange && (gPlayState->actorCtx.flags.clear & roomBit) != 0;
    if (wasCleared) {
        gPlayState->actorCtx.flags.clear &= ~roomBit;
    }

    Actor* spawned = Actor_Spawn(&gPlayState->actorCtx, gPlayState, actorId, x, y, z, rotX, rotY, rotZ, params);

    if (wasCleared) {
        gPlayState->actorCtx.flags.clear |= roomBit;
    }
    return spawned;
}

bool CanSpawnInCurrentScene(const SpawnDef& def) {
    int16_t sceneNum = gPlayState->sceneNum;
    int16_t roomNum = gPlayState->roomCtx.curRoom.num;

    if (sceneNum == SCENE_FOREST_TEMPLE && (roomNum == 12 || roomNum == 13 || roomNum == 16)) {
        return false;
    }
    if (def.actorId == ACTOR_EN_CLEAR_TAG) {
        if (sceneNum == SCENE_DODONGOS_CAVERN_BOSS || sceneNum == SCENE_WATER_TEMPLE_BOSS ||
            sceneNum == SCENE_SPIRIT_TEMPLE_BOSS || sceneNum == SCENE_GANONDORF_BOSS ||
            sceneNum == SCENE_FISHING_POND || sceneNum == SCENE_GANON_BOSS) {
            return false;
        }
    }
    return true;
}

// Actors live in one intrusive linked list per category, and Actor_Spawn inserts new ones at the HEAD. Some
// vanilla enemies find their partner by blindly casting actor.prev / actor.next (the two mini-boss Lizalfos of
// Dodongo's Cavern do this to hand each other their "attack now" timer). An injected actor at the head becomes the
// neighbour of the first native one, so that code wrote into the wrong actor and the real partner never got its
// timer: the Lizalfos stayed frozen even after the injected enemy died. Moving injected actors to the tail keeps
// the natives' adjacency exactly as the game laid it out.
void MoveActorToListTail(Actor* actor) {
    if (actor == nullptr || actor->next == nullptr || actor->category >= ACTORCAT_MAX) {
        return;
    }
    ActorListEntry& list = gPlayState->actorCtx.actorLists[actor->category];

    if (actor->prev != nullptr) {
        actor->prev->next = actor->next;
    } else {
        list.head = actor->next;
    }
    actor->next->prev = actor->prev;

    Actor* tail = actor->next;
    while (tail->next != nullptr) {
        tail = tail->next;
    }
    tail->next = actor;
    actor->prev = tail;
    actor->next = nullptr;
}

// The non-mini-boss Lizalfos/Dinolfos overwrite a static shared with the Dodongo's Cavern mini-boss pair (which of
// the two is attacking / whether the fight is over) and, when they die, poke their list neighbour as if it were
// the partner. Injecting one during that fight would end it early or corrupt memory, so hold it until it is over.
bool IsLizalfosMinibossActive() {
    for (Actor* it = gPlayState->actorCtx.actorLists[ACTORCAT_ENEMY].head; it != nullptr; it = it->next) {
        if (it->id == ACTOR_EN_ZF && it->update != nullptr && it->params >= 0 && it->colChkInfo.health > 0) {
            return true;
        }
    }
    return false;
}

ApplyResult SpawnDefNow(const SpawnDef& def, const std::string& user) {
    if (gPlayState == nullptr) {
        return ApplyResult::Deferred;
    }
    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return ApplyResult::Deferred;
    }
    if (!CanSpawnInCurrentScene(def)) {
        return ApplyResult::Rejected;
    }
    // Room still loading, or a cutscene is running: wait rather than spawn into a half-initialised scene.
    if (gPlayState->roomCtx.status != 0 || gPlayState->csCtx.state != CS_STATE_IDLE) {
        return ApplyResult::Deferred;
    }
    if (def.actorId == ACTOR_EN_ZF && IsLizalfosMinibossActive()) {
        return ApplyResult::Deferred;
    }

    EnsureActorHeadroom();
    if (IsTrackedSpawnName(def.name) &&
        (CountLiveTrackedEnemies() >= kMaxBridgeEnemies || CountSceneEnemies() >= kMaxSceneEnemies)) {
        SPDLOG_WARN("[HiveShock] spawn cap reached, skipping {}", def.name);
        return ApplyResult::Skipped;
    }

    // Try several nearby points. A single raycast into void (indoors / cliffs)
    // used to return Deferred forever and stall the whole pending spawn queue.
    Vec3f pos = player->actor.world.pos;
    bool foundFloor = false;
    // Some enemies only wake up when Link is within a short range (Stalchild: 60 units, Redead/Gibdo: 150),
    // so spawn those closer to have them attack right away.
    f32 radii[] = { 80.0f, 120.0f, 40.0f, 160.0f };
    if (def.actorId == ACTOR_EN_SKB) {
        radii[0] = 45.0f;
        radii[1] = 35.0f;
        radii[2] = 50.0f;
        radii[3] = 30.0f;
    } else if (def.actorId == ACTOR_EN_RD) {
        radii[0] = 90.0f;
        radii[1] = 120.0f;
        radii[2] = 60.0f;
        radii[3] = 130.0f;
    }
    const s16 yawBase = player->actor.shape.rot.y; // in front of Link
    for (f32 radius : radii) {
        for (s32 i = 0; i < 8 && !foundFloor; i++) {
            s16 yaw = yawBase + (s16)(i * 0x2000);
            Vec3f candidate;
            candidate.x = player->actor.world.pos.x + Math_SinS(yaw) * radius;
            candidate.y = player->actor.world.pos.y + 50.0f;
            candidate.z = player->actor.world.pos.z + Math_CosS(yaw) * radius;

            CollisionPoly poly;
            Vec3f rayPos = candidate;
            f32 floorY = BgCheck_AnyRaycastFloor1(&gPlayState->colCtx, &poly, &rayPos);
            if (floorY > BGCHECK_Y_MIN) {
                pos = candidate;
                pos.y = floorY;
                foundFloor = true;
            }
        }
    }
    if (!foundFloor) {
        // Last resort: spawn at Link's feet so the command still does something.
        pos = player->actor.world.pos;
        CollisionPoly poly;
        Vec3f rayPos = pos;
        rayPos.y += 50.0f;
        f32 floorY = BgCheck_AnyRaycastFloor1(&gPlayState->colCtx, &poly, &rayPos);
        if (floorY > BGCHECK_Y_MIN) {
            pos.y = floorY;
        }
    }
    if (def.actorId == ACTOR_EN_CLEAR_TAG || def.actorId == ACTOR_EN_FIREFLY) {
        pos.y += 100.0f;
    }
    if (def.actorId == ACTOR_EN_NIW) {
        pos.y = player->actor.world.pos.y + 80.0f;
    }

    s16 faceYaw = Math_Vec3f_Yaw(&pos, &player->actor.world.pos);
    Actor* spawned = SpawnActorInAnyRoom(def.actorId, pos.x, pos.y, pos.z, 0, faceYaw, 0, def.params);
    if (spawned == nullptr) {
        EnsureActorHeadroom();
        spawned = SpawnActorInAnyRoom(def.actorId, pos.x, pos.y, pos.z, 0, faceYaw, 0, def.params);
    }
    if (spawned == nullptr) {
        // Fall back beside Link if the chosen point is invalid for Actor_Spawn.
        pos = player->actor.world.pos;
        pos.x += Math_SinS(yawBase) * 40.0f;
        pos.z += Math_CosS(yawBase) * 40.0f;
        spawned = SpawnActorInAnyRoom(def.actorId, pos.x, pos.y, pos.z, 0, faceYaw, 0, def.params);
    }
    if (spawned == nullptr) {
        SPDLOG_WARN("[HiveShock] Actor_Spawn({}) returned NULL (actors={})", def.name, gPlayState->actorCtx.total);
        return ApplyResult::Skipped;
    }

    MoveActorToListTail(spawned);

    spawned->shape.rot.y = faceYaw;
    spawned->world.rot.y = faceYaw;

    if (def.actorId == ACTOR_EN_NIW) {
        reinterpret_cast<EnNiw*>(spawned)->actionFunc = func_80AB70A0_nocutscene;
    }

    if (!user.empty() && CVarGetInteger(CVAR_REMOTE_HIVESHOCK("EnemyNameTags"), 1)) {
        std::string tag = user;
        if (tag.size() > kMaxNameTagChars) {
            tag.resize(kMaxNameTagChars);
        }
        NameTag_RegisterForActor(spawned, tag.c_str());
    }

    if (IsTrackedSpawnName(def.name)) {
        TrackBridgeEnemy(spawned);
    }
    SPDLOG_INFO("[HiveShock] spawned {} at ({:.1f}, {:.1f}, {:.1f})", def.name, pos.x, pos.y, pos.z);
    return ApplyResult::Applied;
}

void TickPendingSpawns() {
    if (gPlayState == nullptr || gPendingSpawns.empty()) {
        return;
    }
    if (ShouldDeferDangerousEffects()) {
        return;
    }

    s32 spawnedThisFrame = 0;
    size_t attempts = gPendingSpawns.size();
    while (!gPendingSpawns.empty() && spawnedThisFrame < kSpawnsPerFrame && attempts-- > 0) {
        PendingSpawn pending = gPendingSpawns.front();
        gPendingSpawns.pop_front();

        if (gPlayState->gameplayFrames < pending.earliestFrame) {
            gPendingSpawns.push_back(std::move(pending));
            continue;
        }

        const SpawnDef* def = FindSpawnDef(pending.name);
        if (def == nullptr) {
            SPDLOG_WARN("[HiveShock] pending spawn unknown '{}'", pending.name);
            continue;
        }

        ApplyResult result = SpawnDefNow(*def, pending.user);
        if (result == ApplyResult::Applied) {
            spawnedThisFrame++;
            continue;
        }

        if (result == ApplyResult::Rejected) {
            SPDLOG_INFO("[HiveShock] '{}' not allowed in scene {}, dropped", pending.name, gPlayState->sceneNum);
            continue;
        }
        if (result == ApplyResult::Deferred) {
            // Held back by something that will pass (fight, cutscene, room load): keep it for a while, without
            // burning the retry budget, and drop it if it is still blocked after kMaxSpawnWaitFrames.
            if (pending.deadlineFrame == 0) {
                pending.deadlineFrame = gPlayState->gameplayFrames + kMaxSpawnWaitFrames;
            }
            if (gPlayState->gameplayFrames < pending.deadlineFrame) {
                pending.earliestFrame = gPlayState->gameplayFrames + kDeferRecheckFrames;
                gPendingSpawns.push_back(std::move(pending));
            } else {
                SPDLOG_WARN("[HiveShock] dropping spawn '{}' after waiting too long", pending.name);
            }
            continue;
        }

        // Skipped (e.g. no floor found): rotate and retry a few times, then drop.
        pending.retries++;
        if (pending.retries < 8) {
            pending.earliestFrame = gPlayState->gameplayFrames + 1;
            gPendingSpawns.push_back(std::move(pending));
        } else {
            SPDLOG_WARN("[HiveShock] dropping spawn '{}' after retries", pending.name);
        }
    }
}

ApplyResult SpawnNamed(const std::string& name, const std::string& user) {
    const SpawnDef* def = FindSpawnDef(name);
    if (def == nullptr) {
        return ApplyResult::Skipped;
    }
    EnqueueEnemySpawn(def->name, user, 0);
    return ApplyResult::Applied;
}

ApplyResult SpawnRandomEnemy(const std::string& user) {
    s32 count = ARRAY_COUNT(kRandomEnemies);
    s32 index = (s32)(Rand_ZeroOne() * count);
    if (index >= count) {
        index = count - 1;
    }
    return SpawnNamed(kRandomEnemies[index], user);
}

ApplyResult SpawnCuccoArmy(const std::string& user) {
    if (FindSpawnDef("cucco") == nullptr) {
        return ApplyResult::Skipped;
    }
    constexpr s32 kCount = 6;
    for (s32 i = 0; i < kCount; i++) {
        EnqueueEnemySpawn("cucco", user, (u32)(i * kCuccoArmyFrameGap));
    }
    return ApplyResult::Applied;
}

s32 WarpEntranceFromName(const std::string& name) {
    std::string key = name;
    if (key.rfind("warp_", 0) == 0) {
        key = key.substr(5);
    }
    for (const WarpAlias& alias : kWarpAliases) {
        if (key == alias.name) {
            return alias.entrance;
        }
    }
    return -1;
}

bool IsWarpAction(const std::string& action) {
    return action == "warp" || action == "warp_random" || WarpEntranceFromName(action) >= 0;
}

ApplyResult ApplyOwlWarp(const json& cmd, const std::string& action) {
    s32 requested = -1;
    std::string target = GetStringField(cmd, "target");
    if (target.empty()) {
        target = GetStringField(cmd, "owl");
    }
    if (!target.empty()) {
        requested = WarpEntranceFromName(target);
    }
    if (requested < 0 && action != "warp" && action != "warp_random") {
        requested = WarpEntranceFromName(action);
    }

    s32 entrance = requested;
    if (entrance < 0) {
        s32 count = ARRAY_COUNT(kRandomWarps);
        s32 pick = static_cast<s32>(Rand_ZeroOne() * count);
        if (pick >= count) {
            pick = count - 1;
        }
        entrance = kRandomWarps[pick];
    }

    GameInteractor::RawAction::TeleportPlayer(entrance);
    SPDLOG_INFO("[HiveShock] warp entrance={}", entrance);
    return ApplyResult::Applied;
}

void ApplyImpulse(Player* player, const json& cmd) {
    float strength = std::clamp(GetNumber(cmd, 6.34375f), -20.0f, 20.0f);
    player->actor.velocity.y = strength;
}

void ApplyHeal(const json& cmd) {
    s16 amount = static_cast<s16>(std::clamp(GetNumber(cmd, 16.0f), 1.0f, 80.0f));
    Health_ChangeBy(gPlayState, amount);
}

void ApplyDamage(const json& cmd) {
    s16 amount = static_cast<s16>(std::clamp(GetNumber(cmd, 16.0f), 1.0f, 64.0f));
    Health_ChangeBy(gPlayState, -amount);
    GameInteractor::RawAction::KnockbackPlayer(std::clamp(GetNumber(cmd, 3.0f), 0.5f, 14.0f) / 5.0f);
    ClampMinHealthAfterHit();
}

// The electrified hit reaction takes its damage from colChkInfo.damage (set by whatever actor hit Link); the raw
// action leaves it at 0, so the shock would animate without hurting. Default matches a Bari/Biri touch: half a heart.
void ApplyShock(const json& cmd) {
    Player* player = GET_PLAYER(gPlayState);
    s32 damage = static_cast<s32>(std::clamp(GetNumber(cmd, kDefaultShockDamage), 1.0f, 64.0f));
    player->actor.colChkInfo.damage = static_cast<u8>(damage);
    GameInteractor::RawAction::ElectrocutePlayer();
    player->actor.colChkInfo.damage = 0;
    ClampMinHealthAfterHit();
}

void ApplyKnockback(const json& cmd) {
    float strength = std::clamp(GetNumber(cmd, 6.0f), 0.5f, 14.0f);
    GameInteractor::RawAction::KnockbackPlayer(strength / 5.0f);
    if (cmd.contains("damage") && cmd["damage"].is_number()) {
        s16 damage = static_cast<s16>(std::clamp(cmd["damage"].get<float>(), 1.0f, 64.0f));
        Health_ChangeBy(gPlayState, -damage);
        ClampMinHealthAfterHit();
    }
}

void ApplyInstaKill() {
    gSaveContext.health = 0;
}

void ApplyRupees(const json& cmd) {
    s16 amount = static_cast<s16>(std::clamp(GetNumber(cmd, 10.0f), -500.0f, 500.0f));
    Rupees_ChangeBy(amount);
}

void ApplyMagic(const json& cmd) {
    s16 amount = static_cast<s16>(std::clamp(GetNumber(cmd, static_cast<float>(MAGIC_NORMAL_METER)), -128.0f, 128.0f));
    GameInteractor::RawAction::AddOrRemoveMagic(static_cast<int8_t>(amount));
}

void ApplyBomb() {
    GameInteractor::RawAction::SpawnActor(ACTOR_EN_BOM, 0);
}

void ApplyBlast() {
    GameInteractor::RawAction::SpawnActor(ACTOR_EN_BOM, 1);
}

void ApplyRupeeRain(Player* player) {
    int16_t currentRupees = gSaveContext.rupees;
    if (currentRupees <= 0) {
        SPDLOG_INFO("[HiveShock] rupee_rain skipped, wallet empty");
        return;
    }

    Vec3f positional = player->actor.world.pos;
    positional.y += 100.0f;
    while (currentRupees > 0) {
        s16 drop;
        s16 amount;
        if (currentRupees >= 20) {
            drop = ITEM00_RUPEE_RED;
            amount = 20;
        } else if (currentRupees >= 5) {
            drop = ITEM00_RUPEE_BLUE;
            amount = 5;
        } else {
            drop = ITEM00_RUPEE_GREEN;
            amount = 1;
        }
        Rupees_ChangeBy(-amount);
        currentRupees -= amount;
        EnItem00* rupeeActor = Item_DropCollectible(gPlayState, &positional, drop);
        if (rupeeActor != nullptr) {
            rupeeActor->actor.speedXZ = Rand_CenteredFloat(5.0f);
            rupeeActor->unk_15A = 600;
        }
    }
}

void ApplyHearts(Player* player) {
    Vec3f positional = player->actor.world.pos;
    positional.y += 60.0f;
    for (s32 i = 0; i < 5; i++) {
        EnItem00* heart = Item_DropCollectible(gPlayState, &positional, ITEM00_HEART);
        if (heart != nullptr) {
            heart->actor.speedXZ = Rand_CenteredFloat(6.0f);
            heart->unk_15A = 600;
        }
    }
}

void ApplyInvert(const json& cmd) {
    float duration = GetDuration(cmd);
    if (duration <= 0.0f) {
        duration = 20.0f;
    }
    duration = std::clamp(duration, 1.0f, 120.0f);
    gInvertControls = true;
    gInvertUntil = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<float>(duration));
    GameInteractor::State::ReverseControlsActive = true;
    SPDLOG_INFO("[HiveShock] inverted controls for {:.1f}s", duration);
}

void ApplySpeedBuff(const json& cmd) {
    f32 mult = std::clamp(GetNumber(cmd, kDefaultSpeedMult), 1.25f, 4.0f);
    ActivateBuff(BridgeBuff::Speed, BuffDurationFromCmd(cmd), mult);
    GameInteractor::State::MovementSpeedMultiplier = mult;
}

// Shares the Speed slot: slowing Link replaces a speed-up (and vice versa), and expiry restores normal speed.
void ApplySlowDebuff(const json& cmd) {
    f32 mult = std::clamp(GetNumber(cmd, kDefaultSlowMult), 0.2f, 0.9f);
    ActivateBuff(BridgeBuff::Speed, BuffDurationFromCmd(cmd), mult);
    GameInteractor::State::MovementSpeedMultiplier = mult;
}

void ApplyInvincibleBuff(const json& cmd) {
    ActivateBuff(BridgeBuff::Invincible, BuffDurationFromCmd(cmd), 1.0f);
    GameInteractor::RawAction::SetPlayerInvincibility(true);
}

void ApplyInfiniteMagicBuff(const json& cmd) {
    ActivateBuff(BridgeBuff::InfiniteMagic, BuffDurationFromCmd(cmd), 1.0f);
    TickInfiniteMagic();
}

void ApplyDamageUpBuff(const json& cmd) {
    f32 mult = std::clamp(GetNumber(cmd, kDefaultDamageMult), 1.5f, 8.0f);
    ActivateBuff(BridgeBuff::DamageUp, BuffDurationFromCmd(cmd), mult);
}

void ApplyHasteBuff(const json& cmd) {
    float duration = BuffDurationFromCmd(cmd);
    ActivateBuff(BridgeBuff::Speed, duration, kDefaultSpeedMult);
    ActivateBuff(BridgeBuff::DamageUp, duration, kDefaultDamageMult);
    GameInteractor::State::MovementSpeedMultiplier = kDefaultSpeedMult;
}

void ApplyTimeSkip() {
    u16 previous = gSaveContext.dayTime;
    u16 next = previous + 0x4000;
    GameInteractor::RawAction::SetTimeOfDay(next);
    SPDLOG_INFO("[HiveShock] time skipped (0x{:04X} -> 0x{:04X})", previous, gSaveContext.dayTime);
}

void ApplyDeleteSave() {
    if (gSaveContext.fileNum >= 0 && gSaveContext.fileNum <= 2) {
        SaveManager::Instance->DeleteZeldaFile(gSaveContext.fileNum);
        SPDLOG_INFO("[HiveShock] deleted save slot {}", gSaveContext.fileNum + 1);
    } else {
        SPDLOG_WARN("[HiveShock] delete_save ignored (invalid fileNum {})", gSaveContext.fileNum);
    }

    gDeathCount = 0;
    gDeathCountedThisCycle = false;
    NotifyDeathsReset("save_deleted");

    auto console = std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
        Ship::Context::GetRawInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"));
    if (console != nullptr) {
        console->Dispatch("reset");
    } else {
        SPDLOG_WARN("[HiveShock] Console window missing; could not reset to file select");
    }
}

void TickTimedEffects() {
    if (gTimeIsFrozen && gTimeFreezeHasDeadline && Clock::now() >= gTimeFrozenUntil) {
        ResumeTime();
    }
    if (gInvertControls && Clock::now() >= gInvertUntil) {
        gInvertControls = false;
        GameInteractor::State::ReverseControlsActive = false;
        SPDLOG_INFO("[HiveShock] controls restored");
    }
    ClearExpiredBuffs();
    TickInfiniteMagic();
    TickInvincible();
}

// ---------------------------------------------------------------------------------------------
// Zeldathon telemetry (game -> HiveShock -> race server).
//
// Sends plain game numbers as JSON lines to the companion's event port; HiveShock translates them to
// the race catalog with zeldathon.json, so the mapping can change without rebuilding the game.
//   {"event":"game_session","state":"loaded"|"exited","file":N}
//   {"event":"scene","scene":N}                       SCENE_* number
//   {"event":"inventory","items":[ITEM_* ...]}        owned items: inventory slots and equipment (swords, shields,
//                                                     tunics, boots), arrows, spells, bottles, ocarina...
//   {"event":"quest","items":N}                       questItems bitmask (medallions, songs, stones, agony, card)
//   {"event":"upgrades","bombBag":N,"wallet":N,"strength":N,"scale":N,"quiver":N,"bulletBag":N,
//                       "magic":N,"doubleDefense":0|1}   upgrade levels (0 = none)
//   {"event":"stats","age":"child"|"adult","hearts":F,"maxHearts":F,"rupees":N,"skulltulas":N}
//   {"event":"boss_defeated","actor":N}               ACTOR_BOSS_* id
// State is polled (cheap, and it also covers items given by any source) and only changes are sent.
// -----------------------------------------------------------------------------------------------

constexpr u32 kTelemetryEveryFrames = 30; // ~1.5 s at the game's 20 updates per second

struct TelemetryState {
    bool announced = false; // "game_session loaded" was sent for the current play session
    bool force = false;     // resend everything on the next poll (load / request_snapshot)
    u32 frame = 0;
    int scene = -1;
    std::vector<int> inventory;
    bool inventoryKnown = false;
    u32 quest = 0;
    bool questKnown = false;
    std::string stats;
    std::string upgrades;
};

TelemetryState gTelemetry;

bool TelemetryInGame() {
    return gPlayState != nullptr && gSaveContext.fileNum >= 0 && gSaveContext.fileNum <= 2;
}

// Items the race tracks; ids are ITEM_* values (see z64item.h). HiveShock maps them to catalog ids.
std::vector<int> CollectOwnedItems() {
    std::vector<int> owned;
    // Items that live in their own inventory slot: the slot holds the item id itself.
    static const int kSlotItems[] = { ITEM_BOMB,    ITEM_BOW,         ITEM_ARROW_FIRE,   ITEM_DINS_FIRE, ITEM_SLINGSHOT,
                                      ITEM_BOMBCHU, ITEM_ARROW_ICE,   ITEM_FARORES_WIND, ITEM_BOOMERANG, ITEM_LENS,
                                      ITEM_HAMMER,  ITEM_ARROW_LIGHT, ITEM_NAYRUS_LOVE };
    for (int item : kSlotItems) {
        if (INV_CONTENT(item) == item) {
            owned.push_back(item);
        }
    }
    // The ocarina slot holds the fairy or the time ocarina.
    if (INV_CONTENT(ITEM_OCARINA_TIME) == ITEM_OCARINA_TIME) {
        owned.push_back(ITEM_OCARINA_TIME);
    }
    // The hookshot slot holds either the hookshot or the longshot.
    if (INV_CONTENT(ITEM_HOOKSHOT) == ITEM_HOOKSHOT || INV_CONTENT(ITEM_HOOKSHOT) == ITEM_LONGSHOT) {
        owned.push_back(ITEM_HOOKSHOT);
    }
    if (INV_CONTENT(ITEM_HOOKSHOT) == ITEM_LONGSHOT) {
        owned.push_back(ITEM_LONGSHOT);
    }
    // Any of the four bottle slots filled means "has a bottle".
    if (gSaveContext.inventory.items[SLOT_BOTTLE_1] != ITEM_NONE ||
        gSaveContext.inventory.items[SLOT_BOTTLE_2] != ITEM_NONE ||
        gSaveContext.inventory.items[SLOT_BOTTLE_3] != ITEM_NONE ||
        gSaveContext.inventory.items[SLOT_BOTTLE_4] != ITEM_NONE) {
        owned.push_back(ITEM_BOTTLE);
    }
    // The child trade slot walks Zelda's letter -> masks; being past the letter means it was delivered.
    const u8 trade = gSaveContext.inventory.items[SLOT_TRADE_CHILD];
    if (trade == ITEM_LETTER_ZELDA || (trade >= ITEM_MASK_KEATON && trade <= ITEM_MASK_TRUTH)) {
        owned.push_back(ITEM_LETTER_ZELDA);
    }
    if (trade == ITEM_MASK_TRUTH) {
        owned.push_back(ITEM_MASK_TRUTH);
    }
    // Equipment lives in a bitmask, not in the slots.
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_KOKIRI)) {
        owned.push_back(ITEM_SWORD_KOKIRI);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_MASTER)) {
        owned.push_back(ITEM_SWORD_MASTER);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SWORD, EQUIP_INV_SWORD_BIGGORON)) {
        owned.push_back(ITEM_SWORD_BGS);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_DEKU)) {
        owned.push_back(ITEM_SHIELD_DEKU);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_HYLIAN)) {
        owned.push_back(ITEM_SHIELD_HYLIAN);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_SHIELD, EQUIP_INV_SHIELD_MIRROR)) {
        owned.push_back(ITEM_SHIELD_MIRROR);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_GORON)) {
        owned.push_back(ITEM_TUNIC_GORON);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_TUNIC, EQUIP_INV_TUNIC_ZORA)) {
        owned.push_back(ITEM_TUNIC_ZORA);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_BOOTS, EQUIP_INV_BOOTS_IRON)) {
        owned.push_back(ITEM_BOOTS_IRON);
    }
    if (CHECK_OWNED_EQUIP(EQUIP_TYPE_BOOTS, EQUIP_INV_BOOTS_HOVER)) {
        owned.push_back(ITEM_BOOTS_HOVER);
    }
    return owned;
}

void TelemetryAnnounceLoaded() {
    gTelemetry.announced = true;
    gTelemetry.force = true;
    SendEventToBridge({ { "event", "game_session" }, { "state", "loaded" }, { "file", gSaveContext.fileNum } });
}

void TelemetryAnnounceExited() {
    if (gTelemetry.announced) {
        SendEventToBridge({ { "event", "game_session" }, { "state", "exited" } });
    }
    gTelemetry = TelemetryState{};
}

// Called every game update while a scene is running; sends only what changed.
void TickTelemetry() {
    if (!TelemetryInGame()) {
        return;
    }
    if (!gTelemetry.announced) {
        // HiveShock was enabled (or the companion started) after the save was already loaded.
        TelemetryAnnounceLoaded();
    }
    if (!gTelemetry.force && (++gTelemetry.frame % kTelemetryEveryFrames) != 0) {
        return;
    }

    const bool force = gTelemetry.force;
    gTelemetry.force = false;

    const int scene = gPlayState->sceneNum;
    if (force || scene != gTelemetry.scene) {
        gTelemetry.scene = scene;
        SendEventToBridge({ { "event", "scene" }, { "scene", scene } });
    }

    std::vector<int> owned = CollectOwnedItems();
    if (force || !gTelemetry.inventoryKnown || owned != gTelemetry.inventory) {
        gTelemetry.inventory = owned;
        gTelemetry.inventoryKnown = true;
        SendEventToBridge({ { "event", "inventory" }, { "items", owned } });
    }

    const u32 quest = gSaveContext.inventory.questItems;
    if (force || !gTelemetry.questKnown || quest != gTelemetry.quest) {
        gTelemetry.quest = quest;
        gTelemetry.questKnown = true;
        SendEventToBridge({ { "event", "quest" }, { "items", quest } });
    }

    // Upgrade levels (0 = none). HiveShock turns "level >= N" into the catalog items (zeldathon.json).
    json upgrades = { { "event", "upgrades" },
                      { "bombBag", CUR_UPG_VALUE(UPG_BOMB_BAG) },
                      { "quiver", CUR_UPG_VALUE(UPG_QUIVER) },
                      { "bulletBag", CUR_UPG_VALUE(UPG_BULLET_BAG) },
                      { "wallet", CUR_UPG_VALUE(UPG_WALLET) },
                      { "strength", CUR_UPG_VALUE(UPG_STRENGTH) },
                      { "scale", CUR_UPG_VALUE(UPG_SCALE) },
                      { "magic", static_cast<int>(gSaveContext.magicLevel) },
                      { "doubleDefense", gSaveContext.isDoubleDefenseAcquired ? 1 : 0 } };
    const std::string upgradesSerialized = upgrades.dump();
    if (force || upgradesSerialized != gTelemetry.upgrades) {
        gTelemetry.upgrades = upgradesSerialized;
        SendEventToBridge(upgrades);
    }

    // linkAge: 0 = adult, 1 = child (see enum LinkAge).
    json stats = { { "event", "stats" },
                   { "age", gSaveContext.linkAge == 1 ? "child" : "adult" },
                   { "hearts", gSaveContext.health / 16.0 },
                   { "maxHearts", gSaveContext.healthCapacity / 16.0 },
                   { "rupees", gSaveContext.rupees },
                   { "skulltulas", gSaveContext.inventory.gsTokens } };
    const std::string serialized = stats.dump();
    if (force || serialized != gTelemetry.stats) {
        gTelemetry.stats = serialized;
        SendEventToBridge(stats);
    }
}

// HiveShock arrived after the game (or lost track): resend everything on the next update.
void RequestTelemetrySnapshot() {
    gTelemetry.announced = false;
    gTelemetry.force = true;
    gTelemetry.frame = 0;
    SPDLOG_INFO("[HiveShock] telemetry snapshot requested");
}

// Closes the game cleanly (used when the race server says the daily time is over).
void RequestQuitGame(const json& cmd) {
    bool save = true;
    if (cmd.contains("save") && cmd["save"].is_boolean()) {
        save = cmd["save"].get<bool>();
    }
    if (save && TelemetryInGame()) {
        SaveManager::Instance->SaveFile(gSaveContext.fileNum);
        SPDLOG_INFO("[HiveShock] game saved before quitting");
    }
    SPDLOG_INFO("[HiveShock] quit_game requested");
    Ship::Context::GetRawInstance()->GetWindow()->Close();
}

ApplyResult ApplyCommand(const json& cmd) {
    // These two work anywhere, including the title screen, so they run before the "in a scene" checks.
    {
        const std::string early = NormalizeActionName(GetStringField(cmd, "action"));
        if (early == "request_snapshot") {
            RequestTelemetrySnapshot();
            return ApplyResult::Applied;
        }
        if (early == "quit_game") {
            RequestQuitGame(cmd);
            return ApplyResult::Applied;
        }
    }

    if (gPlayState == nullptr) {
        return ApplyResult::Deferred;
    }

    Player* player = GET_PLAYER(gPlayState);
    if (player == nullptr) {
        return ApplyResult::Deferred;
    }

    std::string action = NormalizeActionName(GetStringField(cmd, "action"));
    if (action.empty()) {
        SPDLOG_WARN("[HiveShock] command missing 'action'");
        return ApplyResult::Skipped;
    }

    if (action == "spawn" || action == "spawn_enemy" || action == "spawn_actor") {
        std::string enemy = GetStringField(cmd, "enemy");
        if (enemy.empty()) {
            enemy = GetStringField(cmd, "name");
        }
        if (enemy.empty()) {
            enemy = GetStringField(cmd, "actor");
        }
        if (enemy.empty()) {
            enemy = GetStringField(cmd, "type");
        }
        if (!enemy.empty()) {
            action = NormalizeActionName(enemy);
        }
    }

    if (IsSceneChangeInProgress()) {
        return ApplyResult::Deferred;
    }

    if (IsDangerousAction(action) && ShouldDeferDangerousEffects()) {
        return ApplyResult::Deferred;
    }

    LogUser(cmd, action);
    std::string user = TruncateDonorName(GetStringField(cmd, "user"));

    if (action == "impulse") {
        ApplyImpulse(player, cmd);
    } else if (action == "knockback") {
        ApplyKnockback(cmd);
    } else if (action == "freeze") {
        GameInteractor::RawAction::FreezePlayer();
    } else if (action == "shock") {
        ApplyShock(cmd);
    } else if (action == "fire") {
        GameInteractor::RawAction::BurnPlayer();
    } else if (action == "heal") {
        ApplyHeal(cmd);
    } else if (action == "damage") {
        ApplyDamage(cmd);
    } else if (action == "insta_kill" || action == "instakill" || action == "kill") {
        ApplyInstaKill();
    } else if (action == "rupees") {
        ApplyRupees(cmd);
    } else if (action == "magic") {
        ApplyMagic(cmd);
    } else if (action == "bomb") {
        ApplyBomb();
    } else if (action == "freeze_time") {
        FreezeTime(GetDuration(cmd));
    } else if (action == "resume_time") {
        ResumeTime();
    } else if (action == "random_enemy") {
        return SpawnRandomEnemy(user);
    } else if (action == "cucco_army") {
        return SpawnCuccoArmy(user);
    } else if (action == "blast") {
        ApplyBlast();
    } else if (action == "rupee_rain") {
        ApplyRupeeRain(player);
    } else if (action == "hearts") {
        ApplyHearts(player);
    } else if (action == "invert") {
        ApplyInvert(cmd);
    } else if (action == "slow" || action == "slow_down" || action == "slowdown") {
        ApplySlowDebuff(cmd);
    } else if (action == "speed") {
        ApplySpeedBuff(cmd);
    } else if (action == "invincible" || action == "invencible") {
        ApplyInvincibleBuff(cmd);
    } else if (action == "infinite_magic") {
        ApplyInfiniteMagicBuff(cmd);
    } else if (action == "damage_up") {
        ApplyDamageUpBuff(cmd);
    } else if (action == "haste") {
        ApplyHasteBuff(cmd);
    } else if (IsWarpAction(action)) {
        return ApplyOwlWarp(cmd, action);
    } else if (action == "time_skip") {
        ApplyTimeSkip();
    } else if (action == "delete_save" || action == "new_run") {
        ApplyDeleteSave();
    } else if (action == "clear_enemies") {
        ClearBridgeEnemies();
    } else if (FindSpawnDef(action) != nullptr) {
        return SpawnNamed(action, user);
    } else {
        SPDLOG_WARN("[HiveShock] unknown action '{}'", action);
        return ApplyResult::Skipped;
    }

    return ApplyResult::Applied;
}

void HandleClient(SocketHandle clientSocket) {
    SPDLOG_INFO("[HiveShock] client connected");
    gClientConnected = true;
    ConfigureSocket(clientSocket);
    std::string buffer;
    char recvBuf[1024];

    while (gServerRun.load()) {
        // Wake up periodically so Disable() never has to interrupt a blocking recv().
        if (!WaitForSocket(clientSocket, true, kPollIntervalMs)) {
            continue;
        }

        auto bytesReceived = recv(clientSocket, recvBuf, sizeof(recvBuf), 0);
        if (bytesReceived == 0) {
            break;
        }
        if (bytesReceived < 0) {
            if (LastSocketErrorIsTransient()) {
                continue;
            }
            break;
        }

        buffer.append(recvBuf, static_cast<size_t>(bytesReceived));

        size_t newlinePos;
        while ((newlinePos = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, newlinePos);
            buffer.erase(0, newlinePos + 1);

            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }
            if (line.size() > kMaxLineBytes) {
                SPDLOG_WARN("[HiveShock] dropping oversized JSON line ({} bytes)", line.size());
                continue;
            }

            try {
                PushCommand(json::parse(line));
            } catch (const std::exception& e) { SPDLOG_WARN("[HiveShock] invalid JSON: {}", e.what()); }
        }

        // A peer that never sends a newline must not grow the buffer without bound.
        if (buffer.size() > kMaxLineBytes) {
            SPDLOG_WARN("[HiveShock] dropping oversized line without newline ({} bytes)", buffer.size());
            buffer.clear();
        }
    }

    CloseSocket(clientSocket);
    gClientConnected = false;
    SPDLOG_INFO("[HiveShock] client disconnected");
}

void RunServer(uint16_t port) {
    NetSession session;
    if (!session.ok) {
        SPDLOG_ERROR("[HiveShock] network subsystem init failed ({})", LastSocketError());
        gServerRun = false;
        return;
    }

    SocketHandle listenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSocket == kInvalidSocket) {
        SPDLOG_ERROR("[HiveShock] socket() failed ({})", LastSocketError());
        gServerRun = false;
        return;
    }
    ConfigureSocket(listenSocket);

#ifdef _WIN32
    // Exclusive bind: SO_REUSEADDR on Windows would let another process hijack the port.
    BOOL exclusive = TRUE;
    setsockopt(listenSocket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive),
               sizeof(exclusive));
#else
    // Allows immediate re-bind after Disable()/Enable() (TIME_WAIT); safe on POSIX.
    int reuse = 1;
    if (setsockopt(listenSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        SPDLOG_WARN("[HiveShock] SO_REUSEADDR failed ({})", LastSocketError());
    }
#endif

    sockaddr_in serverAddr = LoopbackAddress(port);
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) != 0) {
        SPDLOG_ERROR("[HiveShock] bind 127.0.0.1:{} failed ({})", port, LastSocketError());
        CloseSocket(listenSocket);
        gServerRun = false;
        return;
    }

    if (listen(listenSocket, 1) != 0) {
        SPDLOG_ERROR("[HiveShock] listen() failed ({})", LastSocketError());
        CloseSocket(listenSocket);
        gServerRun = false;
        return;
    }

    gListening = true;
    SPDLOG_INFO("[HiveShock] listening on 127.0.0.1:{}", port);

    while (gServerRun.load()) {
        if (!WaitForSocket(listenSocket, true, kPollIntervalMs)) {
            continue;
        }
        SocketHandle clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == kInvalidSocket) {
            if (LastSocketErrorIsTransient()) {
                continue;
            }
            SPDLOG_ERROR("[HiveShock] accept() failed ({})", LastSocketError());
            break;
        }
        HandleClient(clientSocket);
    }

    gListening = false;
    gClientConnected = false;
    CloseSocket(listenSocket);
    // If the loop ended on an error rather than Disable(), reflect that in IsEnabled().
    gServerRun = false;
    SPDLOG_INFO("[HiveShock] server stopped");
}

void StartServer() {
    if (gServerRun.exchange(true)) {
        return;
    }

    if (gServerThread.joinable()) {
        gServerThread.join();
    }
    const auto port =
        static_cast<uint16_t>(std::clamp(CVarGetInteger(CVAR_REMOTE_HIVESHOCK("Port"), kDefaultPort), 1025, 65534));
    gPort = port;
    gServerThread = std::thread(RunServer, port);
}

void StopServer() {
    gServerRun = false;
    // Every blocking call in the server thread polls with a timeout, so this returns promptly.
    if (gServerThread.joinable()) {
        gServerThread.join();
    }

    ClearCommandQueue();
    gPendingSpawns.clear();
    ResetBridgeActorTracking();
    gListening = false;
    gClientConnected = false;
}

} // namespace

extern "C" float HiveShock_GetOutgoingDamageMultiplier(void) {
    return CurrentOutgoingDamageMultiplier();
}

HiveShock* HiveShock::Instance = nullptr;

void HiveShock::Enable() {
    StartServer();
}

void HiveShock::Disable() {
    StopServer();
}

bool HiveShock::IsEnabled() const {
    return gServerRun.load();
}

bool HiveShock::IsListening() const {
    return gListening.load();
}

bool HiveShock::HasClient() const {
    return gClientConnected.load();
}

std::string HiveShock::GetListenEndpoint() const {
    return "127.0.0.1:" + std::to_string(gPort.load());
}

static void RegisterHiveShock() {
    const bool enabled = CVAR_ENABLED;

    if (enabled) {
        StartServer();
    } else {
        StopServer();
    }

    COND_HOOK(OnGameFrameUpdate, enabled, []() {
        json cmd;
        while (PopCommand(cmd)) {
            ApplyResult result = ApplyCommand(cmd);
            if (result == ApplyResult::Deferred) {
                PushCommandFront(std::move(cmd));
                break;
            }
        }

        TickTimedEffects();

        if (gPlayState == nullptr) {
            gPendingSpawns.clear();
            ResetBridgeActorTracking();
            return;
        }

        TickPendingSpawns();
        PruneTrackedEnemies();
        TickTelemetry();

        if (gPlayState->gameOverCtx.state == GAMEOVER_DEATH_WAIT_GROUND && !gDeathCountedThisCycle) {
            RegisterDeath("player death");
        } else if (gPlayState->gameOverCtx.state == GAMEOVER_INACTIVE) {
            gDeathCountedThisCycle = false;
        }
    });

    COND_HOOK(OnLoadGame, enabled, [](int32_t /*fileNum*/) {
        gTelemetry = TelemetryState{};
        TelemetryAnnounceLoaded();
    });
    COND_HOOK(OnExitGame, enabled, [](int32_t /*fileNum*/) { TelemetryAnnounceExited(); });
    COND_HOOK(OnBossDefeat, enabled, [](void* actor) {
        if (actor != nullptr) {
            SendEventToBridge({ { "event", "boss_defeated" }, { "actor", static_cast<Actor*>(actor)->id } });
        }
    });

    COND_HOOK(OnSceneInit, enabled, [](int16_t /*sceneNum*/) { ResetBridgeActorTracking(); });
    COND_HOOK(OnTransitionEnd, enabled, [](int16_t /*sceneNum*/) { ResetBridgeActorTracking(); });
}

static RegisterShipInitFunc initFunc(RegisterHiveShock, { CVAR_NAME });

#else // __SWITCH__ || __WIIU__

extern "C" float HiveShock_GetOutgoingDamageMultiplier(void) {
    return 1.0f;
}

HiveShock* HiveShock::Instance = nullptr;

void HiveShock::Enable() {
    SPDLOG_WARN("[HiveShock] not supported on this platform");
}

void HiveShock::Disable() {
}

bool HiveShock::IsEnabled() const {
    return false;
}

bool HiveShock::IsListening() const {
    return false;
}

bool HiveShock::HasClient() const {
    return false;
}

std::string HiveShock::GetListenEndpoint() const {
    return "";
}

static void RegisterHiveShock() {
}

static RegisterShipInitFunc initFunc(RegisterHiveShock, { CVAR_REMOTE_HIVESHOCK("Enabled") });

#endif
