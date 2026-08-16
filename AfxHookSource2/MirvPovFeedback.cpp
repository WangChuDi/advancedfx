#include "stdafx.h"

#include "MirvPovFeedback.h"
#include "MirvPovCore.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvTime.h"
#include "SchemaSystem.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <Windows.h>
#include <cmath>
#include <limits.h>
#include <mutex>
#include <stdint.h>

namespace {

using PlayEntitySound_t = void (__fastcall *)(void * entity, void * filterEntity, const char * soundName);
using HashString_t = unsigned int (__fastcall *)(const char * string, unsigned int length, unsigned int lengthXorSeed);
using DamageMessage_t = __int64 (__fastcall *)(void * hudDamageIndicator, void * damageMessage);
using AddDamageDirection_t = void (__fastcall *)(void * hudDamageIndicator, float * sourcePosition, CEntityInstance * victimPawn);
using DamageIndicatorConstructor_t = void * (__fastcall *)(void * hudDamageIndicator);

PlayEntitySound_t g_PlayEntitySound = nullptr;
HashString_t g_HashString = nullptr;
DamageMessage_t g_OriginalDamageMessage = nullptr;
AddDamageDirection_t g_AddDamageDirection = nullptr;
DamageIndicatorConstructor_t g_OriginalDamageIndicatorConstructor = nullptr;
void * g_DamageIndicator = nullptr;
bool g_DamageMessageHooked = false;
bool g_DamageIndicatorConstructorHooked = false;
uint32_t g_LastPovControllerHandle = 0xFFFFFFFFu;
uint32_t g_PreviousPovControllerHandle = 0xFFFFFFFFu;
int g_PreviousPovControllerFrame = INT_MIN;

struct DamageDirectionClaim {
    int frame = INT_MIN;
    int victimEntityIndex = -1;
    int amount = -1;
    float sourcePosition[3] = {};
    bool valid = false;
};

std::mutex g_DamageDirectionClaimMutex;
DamageDirectionClaim g_LastDamageDirectionClaim;

bool ClaimDamageDirection(
    int victimEntityIndex,
    int amount,
    const float sourcePosition[3])
{
    const int frame = g_MirvTime.framecount_get();
    std::lock_guard<std::mutex> lock(g_DamageDirectionClaimMutex);

    if(g_LastDamageDirectionClaim.valid
        && g_LastDamageDirectionClaim.frame == frame
        && g_LastDamageDirectionClaim.victimEntityIndex == victimEntityIndex
        && (amount < 0 || g_LastDamageDirectionClaim.amount < 0
            || amount == g_LastDamageDirectionClaim.amount)) {
        const float dx = sourcePosition[0] - g_LastDamageDirectionClaim.sourcePosition[0];
        const float dy = sourcePosition[1] - g_LastDamageDirectionClaim.sourcePosition[1];
        const float dz = sourcePosition[2] - g_LastDamageDirectionClaim.sourcePosition[2];
        if(dx * dx + dy * dy + dz * dz <= 4096.0f) return false;
    }

    g_LastDamageDirectionClaim.frame = frame;
    g_LastDamageDirectionClaim.victimEntityIndex = victimEntityIndex;
    g_LastDamageDirectionClaim.amount = amount;
    g_LastDamageDirectionClaim.sourcePosition[0] = sourcePosition[0];
    g_LastDamageDirectionClaim.sourcePosition[1] = sourcePosition[1];
    g_LastDamageDirectionClaim.sourcePosition[2] = sourcePosition[2];
    g_LastDamageDirectionClaim.valid = true;
    return true;
}

void * __fastcall New_DamageIndicatorConstructor(void * hudDamageIndicator)
{
    void * result = nullptr != g_OriginalDamageIndicatorConstructor
        ? g_OriginalDamageIndicatorConstructor(hudDamageIndicator)
        : hudDamageIndicator;
    g_DamageIndicator = nullptr != result ? result : hudDamageIndicator;
    return result;
}

SOURCESDK::CS2::GameEventKeySymbol_t MakeKey(const char * name)
{
    size_t length = strlen(name);
    unsigned int hash = g_HashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}

bool IsPovAttack(
    SOURCESDK::CS2::IGameEvent * event,
    CEntityInstance *& attackerPawn,
    CEntityInstance *& victimPawn)
{
    attackerPawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey("attacker")));
    victimPawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey("userid")));
    CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
    if(nullptr == povPawn) return false;
    if(nullptr == attackerPawn || nullptr == victimPawn) return false;
    if(attackerPawn != povPawn || attackerPawn == victimPawn) return false;
    return true;
}

bool PawnHasHelmet(CEntityInstance * pawn)
{
    return nullptr != pawn
        && 0 != g_clientDllOffsets.C_CSPlayerPawn.m_bPrevHelmet
        && *reinterpret_cast<bool *>(
            reinterpret_cast<unsigned char *>(pawn)
            + g_clientDllOffsets.C_CSPlayerPawn.m_bPrevHelmet);
}

bool PawnHasArmor(CEntityInstance * pawn)
{
    return nullptr != pawn
        && 0 != g_clientDllOffsets.C_CSPlayerPawn.m_ArmorValue
        && 0 < *reinterpret_cast<int *>(
            reinterpret_cast<unsigned char *>(pawn)
            + g_clientDllOffsets.C_CSPlayerPawn.m_ArmorValue);
}

void Play(CEntityInstance * victimPawn, const char * soundName)
{
    if(nullptr == g_PlayEntitySound || nullptr == victimPawn || nullptr == soundName) return;
    g_PlayEntitySound(victimPawn, nullptr, soundName);
}

__int64 __fastcall New_DamageMessage(void * hudDamageIndicator, void * damageMessage)
{
    if(nullptr != hudDamageIndicator) g_DamageIndicator = hudDamageIndicator;
    __int64 result = nullptr != g_OriginalDamageMessage
        ? g_OriginalDamageMessage(hudDamageIndicator, damageMessage)
        : 0;
    if(!MirvPov_IsEnabled()
        || nullptr == hudDamageIndicator
        || nullptr == damageMessage
        || nullptr == g_AddDamageDirection) return result;

    __try {
        unsigned char * message = reinterpret_cast<unsigned char *>(damageMessage);
        int amount = *reinterpret_cast<int *>(message + 0x50);
        int victimEntityIndex = *reinterpret_cast<int *>(message + 0x54);
        if(amount <= 0) return result;

        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn || !povPawn->IsPlayerPawn()) return result;
        auto povHandle = povPawn->GetHandle();
        if(!povHandle.IsValid() || povHandle.GetEntryIndex() != victimEntityIndex) return result;

        unsigned char * sourceMessage = *reinterpret_cast<unsigned char **>(message + 0x48);
        if(nullptr == sourceMessage) return result;
        float sourcePosition[3] = {
            *reinterpret_cast<float *>(sourceMessage + 0x18),
            *reinterpret_cast<float *>(sourceMessage + 0x1C),
            *reinterpret_cast<float *>(sourceMessage + 0x20)
        };
        if(ClaimDamageDirection(victimEntityIndex, amount, sourcePosition)) {
            g_AddDamageDirection(hudDamageIndicator, sourcePosition, povPawn);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

bool TryGetEventPawn(
    SOURCESDK::CS2::IGameEvent * event,
    const char * keyName,
    CEntityInstance *& pawn)
{
    pawn = nullptr;
    if(nullptr == event || nullptr == keyName || nullptr == g_HashString) return false;
    pawn = reinterpret_cast<CEntityInstance *>(event->GetPlayerPawn(MakeKey(keyName)));
    if(nullptr != pawn && pawn->IsPlayerPawn()) return true;

    CEntityInstance * controller = reinterpret_cast<CEntityInstance *>(
        event->GetPlayerController(MakeKey(keyName)));
    if(nullptr == controller || !controller->IsPlayerController()) return false;
    auto pawnHandle = controller->GetPlayerPawnHandle();
    if(!pawnHandle.IsValid()) return false;
    pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
    return nullptr != pawn && pawn->IsPlayerPawn();
}

void HandleVictimDamageDirection(SOURCESDK::CS2::IGameEvent * event)
{
    if(!MirvPovFeedback_IsLocalPlayerVictim(event)) return;

    CEntityInstance * victimPawn = nullptr;
    CEntityInstance * attackerPawn = nullptr;
    if(!TryGetEventPawn(event, "userid", victimPawn)) {
        victimPawn = GetCurrentPovPlayerPawn();
    }
    if(nullptr == victimPawn || !victimPawn->IsPlayerPawn()) return;
    if(!TryGetEventPawn(event, "attacker", attackerPawn)
        || nullptr == attackerPawn
        || attackerPawn == victimPawn) return;

    float sourcePosition[3] = {};
    attackerPawn->GetOrigin(sourcePosition[0], sourcePosition[1], sourcePosition[2]);
    if(!std::isfinite(sourcePosition[0])
        || !std::isfinite(sourcePosition[1])
        || !std::isfinite(sourcePosition[2])) return;

    if(nullptr == g_DamageIndicator || nullptr == g_AddDamageDirection) {
        return;
    }

    const int victimEntityIndex = victimPawn->GetHandle().GetEntryIndex();
    const auto damageKey = MakeKey("dmg_health");
    const int amount = event->HasKey(damageKey) ? event->GetInt(damageKey) : -1;
    if(ClaimDamageDirection(victimEntityIndex, amount, sourcePosition)) {
        g_AddDamageDirection(g_DamageIndicator, sourcePosition, victimPawn);
    }
}

void HandleHurt(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * attackerPawn = nullptr;
    CEntityInstance * victimPawn = nullptr;
    if(!IsPovAttack(event, attackerPawn, victimPawn)) return;

    auto healthKey = MakeKey("health");
    if(event->HasKey(healthKey) && event->GetInt(healthKey) <= 0) return;

    bool headshot = 1 == event->GetInt(MakeKey("hitgroup"));
    bool armored = headshot
        ? PawnHasHelmet(victimPawn)
        : PawnHasArmor(victimPawn) || 0 < event->GetInt(MakeKey("dmg_armor"));
    Play(victimPawn,
        headshot
            ? (armored
                ? "Player.DamageHeadShotArmor.AttackerFeedback"
                : "Player.DamageHeadShot.AttackerFeedback")
            : (armored
                ? "Player.DamageBodyArmor.AttackerFeedback"
                : "Player.DamageBody.AttackerFeedback"));
}

void HandleDeath(SOURCESDK::CS2::IGameEvent * event)
{
    CEntityInstance * attackerPawn = nullptr;
    CEntityInstance * victimPawn = nullptr;
    if(!IsPovAttack(event, attackerPawn, victimPawn)) return;

    bool headshot = event->GetBool(MakeKey("headshot"));
    bool armored = headshot ? PawnHasHelmet(victimPawn) : PawnHasArmor(victimPawn);
    Play(victimPawn,
        headshot
            ? (armored
                ? "Player.DeathHeadShotArmor.AttackerFeedback"
                : "Player.DeathHeadShot.AttackerFeedback")
            : (armored
                ? "Player.DeathBodyArmor.AttackerFeedback"
                : "Player.DeathBody.AttackerFeedback"));
}

} // namespace

void MirvPovFeedback_Initialize(HMODULE clientDll)
{
    if(nullptr == clientDll) return;
    if(nullptr == g_PlayEntitySound) {
        g_PlayEntitySound = reinterpret_cast<PlayEntitySound_t>(getAddress(
            clientDll,
            "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 81 EC ?? ?? ?? ?? 49 8B E8"));
    }
    if(nullptr == g_HashString) {
        g_HashString = reinterpret_cast<HashString_t>(getAddress(
            clientDll,
            "48 83 EC 28 45 8B D0 4C 8B C9 48 83 FA 04 0F 82 ?? ?? ?? ?? 0F B6 09 48 89 5C 24 20 8D 41 BF 3C 19 77 03 80 C1 20"));
    }

    if(nullptr == g_OriginalDamageMessage && !g_DamageMessageHooked) {
        g_OriginalDamageMessage = reinterpret_cast<DamageMessage_t>(getAddress(
            clientDll,
            "48 89 5C 24 ?? 48 89 6C 24 ?? 57 48 81 EC ?? ?? ?? ?? 80 3D ?? ?? ?? ?? ?? 48 8B FA 48 8B E9"));
    }
    if(nullptr == g_AddDamageDirection) {
        g_AddDamageDirection = reinterpret_cast<AddDamageDirection_t>(getAddress(
            clientDll,
            "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 0F 29 7C 24 ?? 48 8B FA"));
    }

    if(nullptr == g_OriginalDamageIndicatorConstructor) {
        // CCSGO_HudDamageIndicator constructor. Hooking this lets the
        // player_hurt fallback use the real HUD instance even when demo
        // playback does not dispatch CCSUsrMsg_Damage for the watched Pawn.
        g_OriginalDamageIndicatorConstructor = reinterpret_cast<DamageIndicatorConstructor_t>(getAddress(
            clientDll,
            "48 89 5C 24 ?? 48 89 6C 24 ?? 56 41 56 41 57 48 83 EC ?? 48 8B F1 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4E 20"));
    }

    const bool attachDamageMessage =
        !g_DamageMessageHooked
        && nullptr != g_OriginalDamageMessage
        && nullptr != g_AddDamageDirection;
    const bool attachDamageConstructor =
        !g_DamageIndicatorConstructorHooked
        && nullptr != g_OriginalDamageIndicatorConstructor;
    if(attachDamageMessage || attachDamageConstructor) {
        LONG beginResult = DetourTransactionBegin();
        LONG updateResult = NO_ERROR;
        LONG messageAttachResult = NO_ERROR;
        LONG constructorAttachResult = NO_ERROR;
        LONG transactionResult = -1;
        if(NO_ERROR == beginResult) updateResult = DetourUpdateThread(GetCurrentThread());
        if(NO_ERROR == beginResult && NO_ERROR == updateResult) {
            if(attachDamageMessage) {
                messageAttachResult = DetourAttach(
                    &(PVOID &)g_OriginalDamageMessage,
                    New_DamageMessage);
            }
            if(NO_ERROR == messageAttachResult && attachDamageConstructor) {
                constructorAttachResult = DetourAttach(
                    &(PVOID &)g_OriginalDamageIndicatorConstructor,
                    New_DamageIndicatorConstructor);
            }
            transactionResult = NO_ERROR == messageAttachResult
                && NO_ERROR == constructorAttachResult
                ? DetourTransactionCommit()
                : DetourTransactionAbort();
        } else if(NO_ERROR == beginResult) {
            transactionResult = DetourTransactionAbort();
        }
        const bool installed = NO_ERROR == beginResult
            && NO_ERROR == updateResult
            && NO_ERROR == messageAttachResult
            && NO_ERROR == constructorAttachResult
            && NO_ERROR == transactionResult;
        if(installed) {
            if(attachDamageMessage) g_DamageMessageHooked = true;
            if(attachDamageConstructor) g_DamageIndicatorConstructorHooked = true;
        }
    }
}

void MirvPovFeedback_UpdatePovSelection()
{
    if(!MirvPov_IsEnabled()) return;

    CEntityInstance * controller = GetCurrentPovPlayerController();
    if(nullptr == controller) controller = GetObservedPlayerController();
    if(nullptr == controller || !controller->IsPlayerController()) return;

    auto handle = controller->GetHandle();
    if(!handle.IsValid()) return;
    const uint32_t rawHandle = handle.ToInt();
    if(rawHandle == g_LastPovControllerHandle) return;

    g_PreviousPovControllerHandle = g_LastPovControllerHandle;
    g_PreviousPovControllerFrame = g_MirvTime.framecount_get();
    g_LastPovControllerHandle = rawHandle;
}

void MirvPovFeedback_ResetPovSelection()
{
    g_LastPovControllerHandle = 0xFFFFFFFFu;
    g_PreviousPovControllerHandle = 0xFFFFFFFFu;
    g_PreviousPovControllerFrame = INT_MIN;

    std::lock_guard<std::mutex> lock(g_DamageDirectionClaimMutex);
    g_LastDamageDirectionClaim = DamageDirectionClaim();
}

bool MirvPovFeedback_IsCurrentPovVictim(
    SOURCESDK::CS2::IGameEvent * event,
    bool * headshot)
{
    if(nullptr != headshot) *headshot = false;
    if(nullptr == event || nullptr == g_HashString) return false;

    __try {
        CEntityInstance * victimPawn = reinterpret_cast<CEntityInstance *>(
            event->GetPlayerPawn(MakeKey("userid")));
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == victimPawn || nullptr == povPawn || victimPawn != povPawn) return false;

        if(nullptr != headshot) *headshot = event->GetBool(MakeKey("headshot"));
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        if(nullptr != headshot) *headshot = false;
        return false;
    }
}

void MirvPovFeedback_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MirvPov_IsEnabled() || nullptr == g_HashString) return;
    const char * name = event->GetName();
    if(nullptr == name) return;

    __try {
        if(0 == strcmp(name, "player_hurt")) {
            HandleVictimDamageDirection(event);
            HandleHurt(event);
        } else if(0 == strcmp(name, "player_death")) {
            HandleDeath(event);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool MirvPovFeedback_IsLocalPlayerVictim(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MirvPov_IsEnabled() || nullptr == g_HashString) {
        return false;
    }

    const char * eventName = nullptr;
    CEntityInstance * victimController = nullptr;
    CEntityInstance * povController = nullptr;
    CEntityInstance * observedController = nullptr;
    CEntityInstance * realController = nullptr;
    CEntityInstance * selectedController = nullptr;
    CEntityInstance * localController = nullptr;
    int userId = -1;
    int victimHandle = -1;
    int povHandle = -1;
    int realHandle = -1;
    bool result = false;

    __try {
        MirvPovFeedback_UpdatePovSelection();
        eventName = event->GetName();
        auto useridKey = MakeKey("userid");
        userId = event->GetInt(useridKey);
        // userid is a controller/user id field. GetPlayerPawn("userid") is not
        // reliable here; the SDK documents it as being intended for _pawn keys.
        victimController = reinterpret_cast<CEntityInstance *>(event->GetPlayerController(useridKey));
        if(nullptr == victimController && 0 <= userId) {
            CEntityInstance * fallback = GetEntityFromIndex(userId + 1);
            if(nullptr != fallback && fallback->IsPlayerController()) victimController = fallback;
        }
        povController = GetCurrentPovPlayerController();
        observedController = GetObservedPlayerController();
        realController = GetRealSplitScreenPlayer(0);
        selectedController = nullptr != povController
            ? povController
            : (nullptr != observedController ? observedController : realController);
        localController = selectedController;

        if(nullptr != victimController) victimHandle = victimController->GetHandle().ToInt();
        if(nullptr != povController) povHandle = povController->GetHandle().ToInt();
        if(nullptr != observedController && nullptr == povController) povHandle = observedController->GetHandle().ToInt();
        if(nullptr != realController) realHandle = realController->GetHandle().ToInt();

        const bool selectedMatch = nullptr != victimController
            && nullptr != selectedController
            && victimHandle == selectedController->GetHandle().ToInt();
        const bool cachedCurrentMatch = nullptr != victimController
            && static_cast<uint32_t>(victimHandle) == g_LastPovControllerHandle;
        const int frameDelta = g_MirvTime.framecount_get() - g_PreviousPovControllerFrame;
        const bool cachedPreviousDeathMatch = nullptr != victimController
            && nullptr != eventName
            && 0 == strcmp(eventName, "player_death")
            && 0 <= frameDelta
            && frameDelta <= 2
            && static_cast<uint32_t>(victimHandle) == g_PreviousPovControllerHandle;
        result = selectedMatch || cachedCurrentMatch || cachedPreviousDeathMatch;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return result;
}

bool MirvPovFeedback_IsRealLocalPlayerVictim(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || nullptr == g_HashString) return false;

    bool result = false;
    int userId = -1;
    CEntityInstance * victimController = nullptr;
    CEntityInstance * realController = nullptr;

    __try {
        auto useridKey = MakeKey("userid");
        userId = event->GetInt(useridKey);
        victimController = reinterpret_cast<CEntityInstance *>(event->GetPlayerController(useridKey));
        if(nullptr == victimController && 0 <= userId) {
            CEntityInstance * fallback = GetEntityFromIndex(userId + 1);
            if(nullptr != fallback && fallback->IsPlayerController()) victimController = fallback;
        }
        realController = GetRealSplitScreenPlayer(0);
        result = nullptr != victimController
            && nullptr != realController
            && victimController->GetHandle().ToInt() == realController->GetHandle().ToInt();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        result = false;
    }
    return result;
}
