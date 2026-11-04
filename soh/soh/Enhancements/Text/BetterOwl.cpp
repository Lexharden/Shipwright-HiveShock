#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/Network/HiveShock/HiveShock.h"

extern "C" {
#include "z64.h"
extern PlayState* gPlayState;
}

#define CVAR_BETTER_OWL_NAME CVAR_ENHANCEMENT("BetterOwl")
#define CVAR_BETTER_OWL_VALUE CVarGetInteger(CVAR_BETTER_OWL_NAME, 0)

static void RegisterBetterOwl() {
    COND_VB_SHOULD(VB_OWL_CHOOSE_BETTER, CVAR_BETTER_OWL_VALUE, {
        MessageContext* msgCtx = &gPlayState->msgCtx;
        if (HiveShock_IsPestActor(msgCtx->talkActor)) {
            // The owl HiveShock sends is meant to be hard to get rid of: the cursor starts on "repeat".
            return;
        }
        if ((msgCtx->textId == 0x2066 || msgCtx->textId == 0x607B || msgCtx->textId == 0x10C2 ||
             msgCtx->textId == 0x10C6 || msgCtx->textId == 0x206A)) {
            msgCtx->choiceIndex = 1;
        }
    });
}

static RegisterShipInitFunc initFunc(RegisterBetterOwl, { CVAR_BETTER_OWL_NAME });
