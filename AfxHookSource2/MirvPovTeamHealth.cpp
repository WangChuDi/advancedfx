#include "stdafx.h"

#include "MirvPovTeamHealth.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#include <Windows.h>
#include <intrin.h>
#include <stdint.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

namespace {

using GetLocalPlayerController_t = CEntityInstance * (__fastcall *)();
using GetPlayerControllerFromSlot_t = CEntityInstance * (__fastcall *)(int);
using BuildPlayerData_t = void (__fastcall *)(void *, void *, const void *, int);
using PresentPlayerData_t = void (__fastcall *)(void *, const void *, void *);
using BuilderVisibilityFlag_t = bool (__fastcall *)();
using ObserverVisibilityGate_t = bool (__fastcall *)(void *);

enum class TargetRelation {
    Unknown,
    Teammate,
    Enemy
};

GetLocalPlayerController_t g_OrgGetLocalPlayerController = nullptr;
GetPlayerControllerFromSlot_t g_GetPlayerControllerFromSlot = nullptr;
BuildPlayerData_t g_OrgBuildPlayerData = nullptr;
PresentPlayerData_t g_OrgPresentPlayerData = nullptr;
BuilderVisibilityFlag_t g_OrgBuilderVisibilityFlag = nullptr;
ObserverVisibilityGate_t g_OrgObserverVisibilityGate = nullptr;
void * g_ContextReturnAddresses[4] = {};
void * g_BuilderVisibilityReturnAddresses[5] = {};
void * g_ObserverGateReturnAddresses[3] = {};
bool g_Hooked = false;
thread_local TargetRelation g_TargetRelation = TargetRelation::Unknown;

TargetRelation ClassifyPlayerState(const void * playerState)
{
    if(!MirvPov_IsEnabled() || nullptr == playerState
        || nullptr == g_GetPlayerControllerFromSlot) return TargetRelation::Unknown;

    __try {
        CEntityInstance * povController = GetCurrentPovPlayerController();
        if(nullptr == povController || !povController->IsPlayerController())
            return TargetRelation::Unknown;

        int playerSlot = 0;
        memcpy(&playerSlot, playerState, sizeof(playerSlot));
        CEntityInstance * playerController = g_GetPlayerControllerFromSlot(playerSlot);
        if(nullptr == playerController) return TargetRelation::Unknown;

        int povTeam = povController->GetTeam();
        int playerTeam = playerController->GetTeam();
        if((2 != povTeam && 3 != povTeam) || (2 != playerTeam && 3 != playerTeam))
            return TargetRelation::Unknown;

        return povTeam == playerTeam ? TargetRelation::Teammate : TargetRelation::Enemy;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return TargetRelation::Unknown;
    }
}

void __fastcall New_BuildPlayerData(void * teamCounter, void * output, const void * playerState, int row)
{
    TargetRelation previous = g_TargetRelation;
    g_TargetRelation = ClassifyPlayerState(playerState);
    g_OrgBuildPlayerData(teamCounter, output, playerState, row);
    g_TargetRelation = previous;
}

void __fastcall New_PresentPlayerData(void * teamCounter, const void * playerState, void * output)
{
    TargetRelation previous = g_TargetRelation;
    g_TargetRelation = ClassifyPlayerState(playerState);
    g_OrgPresentPlayerData(teamCounter, playerState, output);
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
    if(MirvPov_IsEnabled()
        && TargetRelation::Enemy == g_TargetRelation
        && IsListedReturnAddress(
            _ReturnAddress(),
            g_BuilderVisibilityReturnAddresses,
            _countof(g_BuilderVisibilityReturnAddresses))) {
        return false;
    }
    return g_OrgBuilderVisibilityFlag();
}

bool __fastcall New_ObserverVisibilityGate(void * controller)
{
    if(MirvPov_IsEnabled()
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

CEntityInstance * __fastcall New_GetLocalPlayerController()
{
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    CEntityInstance * nativeController = g_OrgGetLocalPlayerController();
    MirvPov_PopHookReturnAddress(previousReturnAddress);
    if(!MirvPov_IsEnabled()
        || TargetRelation::Unknown == g_TargetRelation) return nativeController;

    int contextIndex = GetTeamCounterContextIndex(returnAddress);
    if(contextIndex < 0) return nativeController;

    __try {
        CEntityInstance * povController = GetCurrentPovPlayerController();
        if(nullptr == povController || !povController->IsPlayerController())
            return nativeController;

        int team = povController->GetTeam();
        if(2 != team && 3 != team)
            return nativeController;

        return povController;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nativeController;
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

    // IDA / Hex-Rays confirms that all four calls obtain the native local
    // player controller for TeamCounter visibility or row-state decisions:
    // one while building cached player data and three while presenting it.
    // They must all observe the same POV controller. Mixing native and POV
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

    g_OrgGetLocalPlayerController = reinterpret_cast<GetLocalPlayerController_t>(getterTarget);
    g_GetPlayerControllerFromSlot = reinterpret_cast<GetPlayerControllerFromSlot_t>(slotResolverTarget);
    g_OrgBuildPlayerData = reinterpret_cast<BuildPlayerData_t>(builder);
    g_OrgPresentPlayerData = reinterpret_cast<PresentPlayerData_t>(presentation);
    g_OrgBuilderVisibilityFlag = reinterpret_cast<BuilderVisibilityFlag_t>(builderVisibilityTarget);
    g_OrgObserverVisibilityGate = reinterpret_cast<ObserverVisibilityGate_t>(observerGateTarget);

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(
        &(PVOID &)g_OrgGetLocalPlayerController,
        New_GetLocalPlayerController);
    DetourAttach(&(PVOID &)g_OrgBuildPlayerData, New_BuildPlayerData);
    DetourAttach(&(PVOID &)g_OrgPresentPlayerData, New_PresentPlayerData);
    DetourAttach(&(PVOID &)g_OrgBuilderVisibilityFlag, New_BuilderVisibilityFlag);
    DetourAttach(&(PVOID &)g_OrgObserverVisibilityGate, New_ObserverVisibilityGate);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgGetLocalPlayerController = nullptr;
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
}
