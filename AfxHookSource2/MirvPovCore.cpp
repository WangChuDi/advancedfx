#include "stdafx.h"

#include "MirvPovCore.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovFeedback.h"
#include "MirvPovHud.h"
#include "MirvPovKillReward.h"
#include "MirvPovPickupPrompt.h"
#include "MirvPovRadio.h"
#include "MirvPovRadar.h"
#include "MirvPovScoreboard.h"
#include "MirvPovSoundCircle.h"
#include "MirvPovTeamHealth.h"
#include "MirvPovTeamID.h"
#include "MirvPovVoice.h"
#include "MirvPovVoiceBan.h"
#include "SchemaSystem.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

namespace {

int g_FakePovRadarControllerIndex = 0;
bool g_MirvPovAutoSync = false;
bool g_MirvPovEnabled = false;
thread_local void * g_MirvPovHookReturnAddress = nullptr;

CEntityInstance * GetPawnFromController(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
    auto pawnHandle = controller->GetPlayerPawnHandle();
    if(!pawnHandle.IsValid()) return nullptr;
    CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
    return nullptr != pawn && pawn->IsPlayerPawn() ? pawn : nullptr;
}

CEntityInstance * GetObservedPlayerPawn()
{
    CEntityInstance * realPawn = GetPawnFromController(GetRealSplitScreenPlayer(0));
    if(nullptr == realPawn || 0 == realPawn->GetObserverMode()) return nullptr;

    auto targetHandle = realPawn->GetObserverTarget();
    if(!targetHandle.IsValid()) return nullptr;

    CEntityInstance * targetPawn = GetEntityFromIndex(targetHandle.GetEntryIndex());
    return nullptr != targetPawn && targetPawn->IsPlayerPawn() ? targetPawn : nullptr;
}

CEntityInstance * GetObservedPlayerController()
{
    CEntityInstance * targetPawn = GetObservedPlayerPawn();
    if(nullptr == targetPawn) return nullptr;

    auto controllerHandle = targetPawn->GetPlayerControllerHandle();
    if(!controllerHandle.IsValid()) return nullptr;

    CEntityInstance * targetController = GetEntityFromIndex(controllerHandle.GetEntryIndex());
    return nullptr != targetController && targetController->IsPlayerController() ? targetController : nullptr;
}

CEntityInstance * ResolveConfiguredPovPlayerController()
{
    if(g_MirvPovAutoSync) return GetObservedPlayerController();
    if(g_FakePovRadarControllerIndex <= 0) return nullptr;

    CEntityInstance * controller = GetEntityFromIndex(g_FakePovRadarControllerIndex);
    return nullptr != controller && controller->IsPlayerController() ? controller : nullptr;
}

} // namespace

bool MirvPov_IsEnabled()
{
    return g_MirvPovEnabled;
}

CEntityInstance * GetCurrentPovPlayerController()
{
    return MirvPov_IsEnabled() ? ResolveConfiguredPovPlayerController() : nullptr;
}

CEntityInstance * GetCurrentPovPlayerPawn()
{
    if(!MirvPov_IsEnabled()) return nullptr;
    if(g_MirvPovAutoSync) return GetObservedPlayerPawn();
    return GetPawnFromController(ResolveConfiguredPovPlayerController());
}

CEntityInstance * GetFakePovRadarController()
{
    return ResolveConfiguredPovPlayerController();
}

CEntityInstance * GetEffectiveSplitScreenPlayer(int slot)
{
    if(0 == slot) {
        if(CEntityInstance * povController = GetCurrentPovPlayerController()) return povController;
    }
    return GetRealSplitScreenPlayer(slot);
}

void SetFakePovRadarControllerIndex(int index)
{
    g_FakePovRadarControllerIndex = 0 < index ? index : 0;
    g_MirvPovAutoSync = false;
}

void SetFakePovRadarAutoSync(bool enabled)
{
    g_MirvPovAutoSync = enabled;
    if(enabled) g_FakePovRadarControllerIndex = -1;
}

bool GetFakePovRadarAutoSync()
{
    return g_MirvPovAutoSync;
}

int GetFakePovRadarControllerIndex()
{
    return g_FakePovRadarControllerIndex;
}

void * MirvPov_PushHookReturnAddress(void * returnAddress)
{
    void * previous = g_MirvPovHookReturnAddress;
    if(nullptr == previous) g_MirvPovHookReturnAddress = returnAddress;
    return previous;
}

void * MirvPov_GetHookReturnAddress()
{
    return g_MirvPovHookReturnAddress;
}

void MirvPov_PopHookReturnAddress(void * previous)
{
    g_MirvPovHookReturnAddress = previous;
}

void MirvPov_UpdateSeekDetection()
{
    if(!MirvPov_IsEnabled() || !g_pEngineToClient) return;
    SOURCESDK::CS2::IDemoFile * demoFile = g_pEngineToClient->GetDemoFile();
    if(!demoFile) return;
    const int demoTick = demoFile->GetDemoTick();
    MirvPovHud_UpdateSeekDetection(demoTick);
    MirvPovKillReward_OnDemoTick(demoTick);
    MirvPovRadio_OnDemoTick(demoTick);
}

void MirvPov_OnFrameStageBefore(int frameStage)
{
    if(SOURCESDK::CS2::FRAME_RENDER_PASS != frameStage) return;

    MirvPovVoice_OnRenderPass();
    MirvPovVoiceBan_OnRenderPass();
    MirvPov_UpdateSeekDetection();
    MirvPovScoreboard_Update();
}

void MirvPov_OnFrameStageAfter(int frameStage)
{
    if(SOURCESDK::CS2::FRAME_RENDER_PASS == frameStage) {
        MirvPovVoice_AfterRenderPass();
    }
}

void MirvPov_OnGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    MirvPovFeedback_HandleGameEvent(event);
}

void MirvPov_OnPanoramaDllLoaded(HMODULE panoramaDll)
{
    MirvPovHud_OnPanoramaDllLoaded(panoramaDll);
}

void MirvPov_OnPanoramaLayoutFileLoaded(const char * filePath)
{
    MirvPovHud_OnPanoramaLayoutFileLoaded(filePath);
}

void MirvPov_Enable(HMODULE clientDll)
{
    if(g_MirvPovEnabled) return;

    g_MirvPovAutoSync = true;
    MirvPovScoreboard_Reset();
    MirvPovSoundCircle_Initialize(clientDll);
    MirvPovHud_ApplyPatches(clientDll);
    MirvPovTeamHealth_Initialize(clientDll);
    MirvPov_ApplyRadarPatches(clientDll);
    if(MirvPovVoice_IsEnabled()) MirvPov_HookVoiceHud(clientDll);
    MirvPovScoreboard_Initialize(clientDll);
    MirvPov_ResetVoiceHud();

    g_MirvPovEnabled = true;
    MirvPovPickupPrompt_Initialize(clientDll);
    MirvPovFeedback_Initialize(clientDll);
    MirvPovKillReward_Initialize(clientDll);
    MirvPovRadio_Initialize(clientDll);
    if(MirvPovVoice_IsEnabled()) MirvPov_UpdateVoiceTeam();
}

void MirvPov_Disable()
{
    if(!g_MirvPovEnabled) return;

    MirvPovScoreboard_Reset();
    g_MirvPovAutoSync = false;
    MirvPovHud_RemovePatches();
    MirvPov_RemoveRadarPatches();
    MirvPovTeamID_RemovePatches();
    MirvPovPickupPrompt_RemovePatches();
    MirvPov_ResetVoiceHud();
    MirvPovKillReward_ApplyHudChatDemoBypass(false);
    MirvPovKillReward_Reset("mirv_pov disabled");
    MirvPovRadio_Reset("mirv_pov disabled");
    g_MirvPovEnabled = false;
}
