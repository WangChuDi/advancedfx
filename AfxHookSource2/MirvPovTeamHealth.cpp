#include "stdafx.h"

#include "MirvPovTeamHealth.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#include <Windows.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

namespace {

using GetLocalPlayerPawn_t = CEntityInstance * (__fastcall *)();
using GetPlayerControllerFromSlot_t = CEntityInstance * (__fastcall *)(int);
using BuildPlayerData_t = void (__fastcall *)(void *, void *, const void *, int);
using PresentPlayerData_t = void (__fastcall *)(void *, const void *, void *);
using BuilderVisibilityFlag_t = bool (__fastcall *)();
using ObserverVisibilityGate_t = bool (__fastcall *)(void *);
using SetDialogVariableInt_t = void (__fastcall *)(void *, const SOURCESDK::CS2::CKV3MemberName &, int);

enum class TargetRelation {
    Unknown,
    Teammate,
    Enemy
};

GetLocalPlayerPawn_t g_OrgGetLocalPlayerPawn = nullptr;
GetPlayerControllerFromSlot_t g_GetPlayerControllerFromSlot = nullptr;
BuildPlayerData_t g_OrgBuildPlayerData = nullptr;
PresentPlayerData_t g_OrgPresentPlayerData = nullptr;
BuilderVisibilityFlag_t g_OrgBuilderVisibilityFlag = nullptr;
ObserverVisibilityGate_t g_OrgObserverVisibilityGate = nullptr;
void * g_ContextReturnAddresses[4] = {};
void * g_BuilderVisibilityReturnAddresses[5] = {};
void * g_ObserverGateReturnAddresses[3] = {};
bool g_Hooked = false;
SetDialogVariableInt_t g_SetDialogVariableInt = nullptr;
thread_local TargetRelation g_TargetRelation = TargetRelation::Unknown;

constexpr size_t kPlayerStateSize = 0x38;
constexpr size_t kPlayerStateSpectateTargetOffset = 0x17;
constexpr size_t kPlayerDataFlagsOffset = 0x08;
constexpr uint8_t kPlayerDataSpectateTargetMask = 0x80;
constexpr size_t kPlayerDataHealthOffset = 0x0c;
volatile LONG g_EnemyHealthSanitizeLogged = 0;
volatile LONG g_HitLogMask = 0;
volatile LONG g_ClassificationDetailLogged = 0;
volatile LONG g_EnemyHealthPublishLogged = 0;

const char * GetRelationName(TargetRelation relation)
{
    switch(relation) {
    case TargetRelation::Teammate: return "teammate";
    case TargetRelation::Enemy: return "enemy";
    default: return "unknown";
    }
}

void LogHookHitOnce(const char * path, int pathIndex, const void * playerState, const void * output)
{
    const int relationIndex = static_cast<int>(g_TargetRelation);
    const LONG bit = 1L << (pathIndex * 3 + relationIndex);
    if(0 != (InterlockedOr(&g_HitLogMask, bit) & bit)) return;

    __try {
        int stateKey = -1;
        int cachedKey = -1;
        int cachedHealth = -1;
        if(nullptr != playerState) memcpy(&stateKey, playerState, sizeof(stateKey));
        if(nullptr != output) {
            memcpy(&cachedKey, reinterpret_cast<const uint8_t *>(output) + 4, sizeof(cachedKey));
            memcpy(&cachedHealth, reinterpret_cast<const uint8_t *>(output) + kPlayerDataHealthOffset, sizeof(cachedHealth));
        }
        MIRV_POV_DIAGNOSTIC_MESSAGE(
            "[mirv_pov_team_health] %s hit: relation=%s state=%d cached=%d health=%d.\n",
            path,
            GetRelationName(g_TargetRelation),
            stateKey,
            cachedKey,
            cachedHealth);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

TargetRelation ClassifyPlayerState(const void * playerState)
{
    if(!MIRV_POV_FEATURE_ACTIVE("teamhealth") || nullptr == playerState
        || nullptr == g_GetPlayerControllerFromSlot) return TargetRelation::Unknown;

    __try {
        int playerSlot = 0;
        memcpy(&playerSlot, playerState, sizeof(playerSlot));
        CEntityInstance * povController = GetCurrentPovPlayerController();
        CEntityInstance * playerController = g_GetPlayerControllerFromSlot(playerSlot);
        const bool povIsController = nullptr != povController && povController->IsPlayerController();
        const int povTeam = povIsController ? povController->GetTeam() : -1;
        const int playerTeam = nullptr != playerController ? playerController->GetTeam() : -1;
        TargetRelation result = TargetRelation::Unknown;
        if(povIsController && nullptr != playerController
            && (2 == povTeam || 3 == povTeam)
            && (2 == playerTeam || 3 == playerTeam)) {
            result = povTeam == playerTeam ? TargetRelation::Teammate : TargetRelation::Enemy;
        }

        if(0 == InterlockedCompareExchange(&g_ClassificationDetailLogged, 1, 0)) {
            MIRV_POV_DIAGNOSTIC_MESSAGE(
                "[mirv_pov_team_health] classify: state=%d pov=%p povController=%d povTeam=%d player=%p playerTeam=%d relation=%s.\n",
                playerSlot,
                povController,
                povIsController ? 1 : 0,
                povTeam,
                playerController,
                playerTeam,
                GetRelationName(result));
        }
        return result;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return TargetRelation::Unknown;
    }
}

void SanitizeEnemyPlayerData(void * output)
{
    if(TargetRelation::Enemy != g_TargetRelation || nullptr == output) return;

    __try {
        uint8_t * playerData = reinterpret_cast<uint8_t *>(output);
        playerData[kPlayerDataFlagsOffset] &= ~kPlayerDataSpectateTargetMask;
        int previousHealth = 0;
        memcpy(&previousHealth, playerData + kPlayerDataHealthOffset, sizeof(previousHealth));

        // TeamCounter publishes this integer as Panorama's `health` dialog
        // variable. The byte at +0x18 belongs to the weapon-icon string and must
        // not be cleared: doing so hides the weapon instead of the health bar.
        memset(playerData + kPlayerDataHealthOffset, 0, sizeof(previousHealth));

        if(0 == InterlockedCompareExchange(&g_EnemyHealthSanitizeLogged, 1, 0)) {
            MIRV_POV_DIAGNOSTIC_MESSAGE(
                "[mirv_pov_team_health] Enemy row sanitized: health %d -> 0.\n",
                previousHealth);
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool IsPovHighlightTarget(const void * playerState)
{
    if(!MIRV_POV_FEATURE_ACTIVE("teamhealth") || !MIRV_POV_FEATURE_ACTIVE("hud_target_highlight")
        || !playerState || !g_GetPlayerControllerFromSlot) return false;
    __try {
        auto pov = GetCurrentPovPlayerController();
        if(!pov || !pov->IsPlayerController() || (pov->GetTeam() != 2 && pov->GetTeam() != 3)) return false;
        int slot = -1;
        memcpy(&slot, playerState, sizeof(slot));
        return g_GetPlayerControllerFromSlot(slot) == pov;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UpdatePovHighlight(const void * playerState, void * output)
{
    if(!MIRV_POV_FEATURE_ACTIVE("teamhealth") || !output) return;
    __try {
        auto & flags = reinterpret_cast<uint8_t *>(output)[kPlayerDataFlagsOffset];
        flags = (flags & ~kPlayerDataSpectateTargetMask)
            | (IsPovHighlightTarget(playerState) ? kPlayerDataSpectateTargetMask : 0);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

void __fastcall New_BuildPlayerData(void * teamCounter, void * output, const void * playerState, int row)
{
    TargetRelation previous = g_TargetRelation;
    g_TargetRelation = ClassifyPlayerState(playerState);
    if(MIRV_POV_FEATURE_ACTIVE("teamhealth"))
        LogHookHitOnce("builder", 0, playerState, output);

    // Keep the native Avatar__SpectateTarget presentation, but select only the
    // configured POV row instead of propagating the viewer's observer target.
    // The original input is shared with native HUD updates and stays unchanged.
    if(MIRV_POV_FEATURE_ACTIVE("teamhealth") && nullptr != playerState) {
        alignas(uint64_t) uint8_t playerStateCopy[kPlayerStateSize];
        memcpy(playerStateCopy, playerState, sizeof(playerStateCopy));
        playerStateCopy[kPlayerStateSpectateTargetOffset] = IsPovHighlightTarget(playerState) ? 1 : 0;
        g_OrgBuildPlayerData(teamCounter, output, playerStateCopy, row);
    } else {
        g_OrgBuildPlayerData(teamCounter, output, playerState, row);
    }
    SanitizeEnemyPlayerData(output);
    g_TargetRelation = previous;
}

void __fastcall New_PresentPlayerData(void * teamCounter, const void * playerState, void * output)
{
    TargetRelation previous = g_TargetRelation;
    g_TargetRelation = ClassifyPlayerState(playerState);
    if(MIRV_POV_FEATURE_ACTIVE("teamhealth"))
        LogHookHitOnce("presentation", 1, playerState, output);

    // Sanitize cached rows immediately as well, including rows built before
    // mirv_pov was enabled or retained across a demo seek.
    if(MIRV_POV_FEATURE_ACTIVE("teamhealth")) {
        SanitizeEnemyPlayerData(output);
        // Cached rows also follow target changes, seeks and the immediate
        // feature control without waiting for a new native player-state build.
        UpdatePovHighlight(playerState, output);
    }
    g_OrgPresentPlayerData(teamCounter, playerState, output);
    // The native equipment presenter returns before publishing health for an
    // enemy in POV mode. A row previously displayed in spectator mode can thus
    // retain its old Panorama value even though its cached health is now zero.
    if(TargetRelation::Enemy == g_TargetRelation && g_SetDialogVariableInt && playerState) {
        __try {
            void * avatarPanel = *reinterpret_cast<void * const *>(
                reinterpret_cast<const uint8_t *>(playerState) + 0x10);
            if(avatarPanel) {
                const SOURCESDK::CS2::CKV3MemberName healthKey(static_cast<int>(0x9aa70a58u), -1, "health");
                g_SetDialogVariableInt(avatarPanel, healthKey, 0);
                if(0 == InterlockedCompareExchange(&g_EnemyHealthPublishLogged, 1, 0))
                    MIRV_POV_DIAGNOSTIC_MESSAGE("[mirv_pov_team_health] Enemy health published: 0.\n");
            }
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    g_TargetRelation = previous;
}

bool IsListedReturnAddress(void * address, void * const * addresses, size_t count)
{
    for(size_t i = 0; i < count; ++i) {
        if(address == addresses[i]) return true;
    }
    return false;
}

bool __fastcall New_BuilderVisibilityFlag()
{
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    MirvPov_PopHookReturnAddress(previousReturnAddress);
    if(MIRV_POV_FEATURE_ACTIVE("teamhealth")
        && TargetRelation::Enemy == g_TargetRelation
        && IsListedReturnAddress(
            returnAddress,
            g_BuilderVisibilityReturnAddresses,
            _countof(g_BuilderVisibilityReturnAddresses))) {
        return false;
    }
    previousReturnAddress = MirvPov_PushHookReturnAddress(returnAddress);
    bool result = g_OrgBuilderVisibilityFlag();
    MirvPov_PopHookReturnAddress(previousReturnAddress);
    return result;
}

bool __fastcall New_ObserverVisibilityGate(void * controller)
{
    if(MIRV_POV_FEATURE_ACTIVE("teamhealth")
        && TargetRelation::Enemy == g_TargetRelation
        && IsListedReturnAddress(
            _ReturnAddress(),
            g_ObserverGateReturnAddresses,
            _countof(g_ObserverGateReturnAddresses))) {
        return false;
    }
    return g_OrgObserverVisibilityGate(controller);
}

int GetTeamCounterContextIndex(void * address)
{
    for(size_t i = 0; i < _countof(g_ContextReturnAddresses); ++i) {
        if(address == g_ContextReturnAddresses[i]) return static_cast<int>(i);
    }
    return -1;
}

CEntityInstance * __fastcall New_GetLocalPlayerPawn()
{
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    CEntityInstance * nativePawn = g_OrgGetLocalPlayerPawn();
    MirvPov_PopHookReturnAddress(previousReturnAddress);
    if(!MIRV_POV_FEATURE_ACTIVE("teamhealth")
        || TargetRelation::Unknown == g_TargetRelation) return nativePawn;

    int contextIndex = GetTeamCounterContextIndex(returnAddress);
    if(contextIndex < 0) return nativePawn;

    __try {
        // The native getter returns a Pawn, not its Controller. Its callers
        // dispatch Pawn virtual methods; a Controller has incompatible slots
        // and can turn those calls into writes through unrelated registers.
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn || !povPawn->IsPlayerPawn())
            return nativePawn;

        int team = povPawn->GetTeam();
        if(2 != team && 3 != team)
            return nativePawn;

        return povPawn;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nativePawn;
    }
}

bool ResolveCallTarget(
    uint8_t * callSite,
    const Afx::BinUtils::MemRange & textRange,
    uint8_t *& target)
{
    if(0xe8 != *callSite) return false;

    int32_t relative = 0;
    memcpy(&relative, callSite + 1, sizeof(relative));
    target = callSite + 5 + relative;
    return textRange.Start <= reinterpret_cast<size_t>(target)
        && reinterpret_cast<size_t>(target) < textRange.End;
}

} // namespace

void MirvPovTeamHealth_Initialize(HMODULE clientDll)
{
    InterlockedExchange(&g_HitLogMask, 0);
    InterlockedExchange(&g_EnemyHealthSanitizeLogged, 0);
    InterlockedExchange(&g_ClassificationDetailLogged, 0);
    InterlockedExchange(&g_EnemyHealthPublishLogged, 0);
    if(g_Hooked || nullptr == clientDll) return;

    Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(!sections.Eof()) textRange = sections.GetMemRange();
    if(textRange.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] client.dll text section was not found.\n");
        return;
    }

    const char * builderPattern =
        "4D 85 C0 0F 84 ?? ?? ?? ?? 48 8B C4 48 89 48 08 53 57 48 83 EC 58";
    auto builderSequence = Afx::BinUtils::FindPatternString(textRange, builderPattern);
    if(builderSequence.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter player-data builder was not found.\n");
        return;
    }
    auto builderRemaining = Afx::BinUtils::MemRange(builderSequence.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(builderRemaining, builderPattern).IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter player-data pattern is not unique.\n");
        return;
    }

    const char * presentationPattern =
        "48 85 D2 0F 84 ?? ?? ?? ?? 48 8B C4 4C 89 40 18 55 56 41 55 41 57 48 8D A8 08 FE FF FF";
    auto presentationSequence = Afx::BinUtils::FindPatternString(textRange, presentationPattern);
    if(presentationSequence.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter presentation function was not found.\n");
        return;
    }
    auto presentationRemaining = Afx::BinUtils::MemRange(presentationSequence.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(presentationRemaining, presentationPattern).IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter presentation pattern is not unique.\n");
        return;
    }

    uint8_t * builder = reinterpret_cast<uint8_t *>(builderSequence.Start);
    uint8_t * presentation = reinterpret_cast<uint8_t *>(presentationSequence.Start);

    // The native builder packs playerState+0x17 into flags bit 7 at output+8.
    // Reject a changed layout before copying input or editing cached flags.
    if(memcmp(builder + 0x14b, "\x0f\xb6\x4b\x17", 4)
        || memcmp(builder + 0x1d7, "\x89\x47\x08", 3)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] Spectate-target layout is invalid.\n");
        return;
    }

    // Validate the native `health` publication, including the member hash,
    // before reusing its helper and avatar-panel offset on cached rows.
    const uint8_t healthPublishPrefix[] = {0x4d, 0x63, 0x47, 0x0c};
    const uint8_t healthMemberHash[] = {0xc7, 0x44, 0x24, 0x38, 0x58, 0x0a, 0xa7, 0x9a};
    uint8_t * healthSetter = nullptr;
    if(0 != memcmp(presentation + 0x308, healthPublishPrefix, sizeof(healthPublishPrefix))
        || 0 != memcmp(presentation + 0x321, healthMemberHash, sizeof(healthMemberHash))
        || !ResolveCallTarget(presentation + 0x329, textRange, healthSetter)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] Health publication context is invalid.\n");
        return;
    }
    g_SetDialogVariableInt = reinterpret_cast<SetDialogVariableInt_t>(healthSetter);

    // All four calls obtain the native local player Pawn for TeamCounter
    // visibility or row-state decisions:
    // one while building cached player data and three while presenting it.
    // They must all observe the same POV Pawn. Mixing native and POV
    // contexts leaves health/equipment fields stale across team switches and seeks.
    uint8_t * callSites[] = {
        builder + 0x55,
        presentation + 0xfb,
        presentation + 0x183,
        presentation + 0x1f4
    };

    uint8_t * getterTarget = nullptr;
    for(size_t i = 0; i < _countof(callSites); ++i) {
        uint8_t * target = nullptr;
        if(!ResolveCallTarget(callSites[i], textRange, target)
            || (nullptr != getterTarget && getterTarget != target)) {
            memset(g_ContextReturnAddresses, 0, sizeof(g_ContextReturnAddresses));
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter context call set is invalid.\n");
            return;
        }

        getterTarget = target;
        g_ContextReturnAddresses[i] = callSites[i] + 5;
    }

    uint8_t * slotResolverTarget = nullptr;
    uint8_t * presentationSlotResolverTarget = nullptr;
    if(!ResolveCallTarget(builder + 0x4b, textRange, slotResolverTarget)
        || !ResolveCallTarget(presentation + 0x107, textRange, presentationSlotResolverTarget)
        || slotResolverTarget != presentationSlotResolverTarget) {
        memset(g_ContextReturnAddresses, 0, sizeof(g_ContextReturnAddresses));
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter player-slot resolver is invalid.\n");
        return;
    }

    uint8_t * builderVisibilityCallSites[] = {
        builder + 0x81,
        builder + 0x89,
        builder + 0x95,
        builder + 0x9e,
        builder + 0xa7
    };
    uint8_t * builderVisibilityTarget = nullptr;
    for(size_t i = 0; i < _countof(builderVisibilityCallSites); ++i) {
        uint8_t * target = nullptr;
        if(!ResolveCallTarget(builderVisibilityCallSites[i], textRange, target)
            || (nullptr != builderVisibilityTarget && builderVisibilityTarget != target)) {
            memset(g_ContextReturnAddresses, 0, sizeof(g_ContextReturnAddresses));
            memset(g_BuilderVisibilityReturnAddresses, 0, sizeof(g_BuilderVisibilityReturnAddresses));
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter builder visibility call set is invalid.\n");
            return;
        }
        builderVisibilityTarget = target;
        g_BuilderVisibilityReturnAddresses[i] = builderVisibilityCallSites[i] + 5;
    }

    uint8_t * observerGateCallSites[] = {
        presentation + 0x11c,
        presentation + 0x18f,
        presentation + 0x21d
    };
    uint8_t * observerGateTarget = nullptr;
    for(size_t i = 0; i < _countof(observerGateCallSites); ++i) {
        uint8_t * target = nullptr;
        if(!ResolveCallTarget(observerGateCallSites[i], textRange, target)
            || (nullptr != observerGateTarget && observerGateTarget != target)) {
            memset(g_ContextReturnAddresses, 0, sizeof(g_ContextReturnAddresses));
            memset(g_BuilderVisibilityReturnAddresses, 0, sizeof(g_BuilderVisibilityReturnAddresses));
            memset(g_ObserverGateReturnAddresses, 0, sizeof(g_ObserverGateReturnAddresses));
            MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter observer visibility call set is invalid.\n");
            return;
        }
        observerGateTarget = target;
        g_ObserverGateReturnAddresses[i] = observerGateCallSites[i] + 5;
    }

    g_OrgGetLocalPlayerPawn = reinterpret_cast<GetLocalPlayerPawn_t>(getterTarget);
    g_GetPlayerControllerFromSlot = reinterpret_cast<GetPlayerControllerFromSlot_t>(slotResolverTarget);
    g_OrgBuildPlayerData = reinterpret_cast<BuildPlayerData_t>(builder);
    g_OrgPresentPlayerData = reinterpret_cast<PresentPlayerData_t>(presentation);
    g_OrgBuilderVisibilityFlag = reinterpret_cast<BuilderVisibilityFlag_t>(builderVisibilityTarget);
    g_OrgObserverVisibilityGate = reinterpret_cast<ObserverVisibilityGate_t>(observerGateTarget);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(
        &(PVOID &)g_OrgGetLocalPlayerPawn,
        New_GetLocalPlayerPawn);
    DetourAttach(&(PVOID &)g_OrgBuildPlayerData, New_BuildPlayerData);
    DetourAttach(&(PVOID &)g_OrgPresentPlayerData, New_PresentPlayerData);
    DetourAttach(&(PVOID &)g_OrgBuilderVisibilityFlag, New_BuilderVisibilityFlag);
    DetourAttach(&(PVOID &)g_OrgObserverVisibilityGate, New_ObserverVisibilityGate);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgGetLocalPlayerPawn = nullptr;
        g_GetPlayerControllerFromSlot = nullptr;
        g_OrgBuildPlayerData = nullptr;
        g_OrgPresentPlayerData = nullptr;
        g_OrgBuilderVisibilityFlag = nullptr;
        g_OrgObserverVisibilityGate = nullptr;
        memset(g_ContextReturnAddresses, 0, sizeof(g_ContextReturnAddresses));
        memset(g_BuilderVisibilityReturnAddresses, 0, sizeof(g_BuilderVisibilityReturnAddresses));
        memset(g_ObserverGateReturnAddresses, 0, sizeof(g_ObserverGateReturnAddresses));
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_team_health] TeamCounter context detour failed.\n");
        return;
    }

    g_Hooked = true;
    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "[mirv_pov_team_health] TeamCounter health hooks installed.\n");
}
