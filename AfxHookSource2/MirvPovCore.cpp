#include "stdafx.h"

#include "MirvPovCore.h"
#include "MirvPovBuyMenu.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovFeedback.h"
#include "MirvPovDeathCam.h"
#include "MirvPovHud.h"
#include "DeathMsg.h"
#include "MirvPovDeathPanel.h"
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
#include "RenderSystemDX11Hooks.h"
#include "SchemaSystem.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

#define MIRV_POV_FEATURE_ENABLED(name) MirvPovDebug_IsFeatureEnabled(name)

namespace {

int g_FakePovRadarControllerIndex = 0;
bool g_MirvPovAutoSync = false;
bool g_MirvPovEnabled = false;
bool g_MirvPovDeathFeedbackEnabled = true;
bool g_MirvPovHadDemoFile = false;
thread_local void * g_MirvPovHookReturnAddress = nullptr;

struct MirvPovDebugFeatureState {
    const char * name;
    bool configured;
    bool active;
    bool immediate = false;
};

MirvPovDebugFeatureState g_MirvPovDebugFeatures[] = {
    {"voice_script", true, true},
    {"cvars", true, true},
    {"teamid", true, true},
    {"soundcircle", true, true},
    {"hud", true, true},
    {"teamhealth", true, true},
    {"buymenu", true, true},
    {"radar", true, true},
    {"voice", true, true},
    {"scoreboard", true, true},
    {"feedback", true, true},
    {"deafen", true, true, true},
    {"damage_direction", true, true, true},
    {"deathpanel_slide", true, true, true},
    {"death_screen", true, true, true},
    {"deathcam", true, true},
    {"pickupprompt", true, true},
    {"killreward", true, true},
    {"radio", true, true},
    {"radio_text_dispatch", true, true},
    {"radio_text_handler", true, true},
    {"radio_sendaudio_dispatch", true, true},
    {"radio_sendaudio_emitter", true, true},
    {"radio_sendaudio_parser", true, true},
    {"radio_rawaudio_dispatch", true, true},
    {"radio_rawaudio_formatter", true, true},
    {"framestage", true, true},
    {"voiceban", true, true}
};

MirvPovDebugFeatureState * FindMirvPovDebugFeature(const char * name)
{
    if(nullptr == name) return nullptr;
    for(auto & feature : g_MirvPovDebugFeatures) {
        if(0 == _stricmp(feature.name, name)) return &feature;
    }
    return nullptr;
}
CEntityInstance * GetPawnFromController(CEntityInstance * controller)
{
    if(nullptr == controller || !controller->IsPlayerController()) return nullptr;
    auto pawnHandle = controller->GetPlayerPawnHandle();
    if(!pawnHandle.IsValid()) return nullptr;
    CEntityInstance * pawn = GetEntityFromIndex(pawnHandle.GetEntryIndex());
    return nullptr != pawn && pawn->IsPlayerPawn() ? pawn : nullptr;
}

CEntityInstance * ResolveObservedPlayerPawn()
{
    CEntityInstance * realPawn = GetPawnFromController(GetRealSplitScreenPlayer(0));
    if(nullptr == realPawn || 0 == realPawn->GetObserverMode()) return nullptr;

    auto targetHandle = realPawn->GetObserverTarget();
    if(!targetHandle.IsValid()) return nullptr;

    CEntityInstance * targetPawn = GetEntityFromIndex(targetHandle.GetEntryIndex());
    return nullptr != targetPawn && targetPawn->IsPlayerPawn() ? targetPawn : nullptr;
}

CEntityInstance * ResolveObservedPlayerController()
{
    CEntityInstance * targetPawn = ResolveObservedPlayerPawn();
    if(nullptr == targetPawn) return nullptr;

    auto controllerHandle = targetPawn->GetPlayerControllerHandle();
    if(!controllerHandle.IsValid()) return nullptr;

    CEntityInstance * targetController = GetEntityFromIndex(controllerHandle.GetEntryIndex());
    return nullptr != targetController && targetController->IsPlayerController() ? targetController : nullptr;
}

CEntityInstance * ResolveConfiguredPovPlayerController()
{
    if(g_MirvPovAutoSync) return ResolveObservedPlayerController();
    if(g_FakePovRadarControllerIndex <= 0) return nullptr;

    CEntityInstance * controller = GetEntityFromIndex(g_FakePovRadarControllerIndex);
    return nullptr != controller && controller->IsPlayerController() ? controller : nullptr;
}

} // namespace

bool MirvPovDebug_CanConfigure()
{
    return true;
}

bool MirvPovDebug_HasFeature(const char * name)
{
    return nullptr != FindMirvPovDebugFeature(name);
}

bool MirvPovDebug_IsFeatureImmediate(const char * name)
{
    auto * feature = FindMirvPovDebugFeature(name);
    return feature && feature->immediate;
}

bool MirvPovDebug_IsFeatureEnabled(const char * name)
{
    MirvPovDebugFeatureState * feature = FindMirvPovDebugFeature(name);
    return nullptr != feature && feature->active;
}

bool MirvPovDebug_SetFeatureEnabled(const char * name, bool enabled)
{
    if(nullptr != name && 0 == _stricmp(name, "all")) {
        for(auto & feature : g_MirvPovDebugFeatures) {
            feature.configured = enabled;
            if(feature.immediate) feature.active = enabled;
        }
        MirvPovFeedback_ResetDeafen();
        MirvPovFeedback_ResetDirections();
        MirvPovDeathCam_Reset();
        return true;
    }
    MirvPovDebugFeatureState * feature = FindMirvPovDebugFeature(name);
    if(nullptr == feature) return false;
    feature->configured = enabled;
    if(feature->immediate) {
        feature->active = enabled;
        if(0 == _stricmp(feature->name, "deafen")) MirvPovFeedback_ResetDeafen();
        if(0 == _stricmp(feature->name, "damage_direction")) MirvPovFeedback_ResetDirections();
        if(0 == _stricmp(feature->name, "death_screen")) MirvPovDeathCam_ResetPresentation();
    }
    return true;
}

void MirvPovDebug_ApplyFeatureConfiguration()
{
    for(auto & feature : g_MirvPovDebugFeatures) feature.active = feature.configured;
}

void MirvPovDebug_PrintFeatureStates()
{
    advancedfx::Message(
        "mirv_pov debug features (%s):\n",
        MirvPov_IsEnabled() ? "active / configured for next enable" : "inactive / configured");
    for(const auto & feature : g_MirvPovDebugFeatures) {
        advancedfx::Message(
            "  %-14s active=%d configured=%d%s\n",
            feature.name,
            feature.active ? 1 : 0,
            feature.configured ? 1 : 0,
            feature.active != feature.configured ? " (pending)" : feature.immediate ? " (immediate)" : "");
    }
}
bool MirvPov_IsEnabled()
{
    return g_MirvPovEnabled;
}

bool MirvPov_IsDeathFeedbackEnabled()
{
    return g_MirvPovDeathFeedbackEnabled;
}

void MirvPov_SetDeathFeedbackEnabled(bool enabled)
{
    g_MirvPovDeathFeedbackEnabled = enabled;
    if(!enabled) {
        MirvPovDeathPanel_Clear();
        RenderSystemDX11_DeathFade_Reset();
    }
}

CEntityInstance * GetCurrentPovPlayerController()
{
    return MirvPov_IsEnabled() ? ResolveConfiguredPovPlayerController() : nullptr;
}

CEntityInstance * GetCurrentPovPlayerPawn()
{
    if(!MirvPov_IsEnabled()) return nullptr;
    if(g_MirvPovAutoSync) return ResolveObservedPlayerPawn();
    return GetPawnFromController(ResolveConfiguredPovPlayerController());
}

CEntityInstance * GetObservedPlayerPawn()
{
    __try {
        return ResolveObservedPlayerPawn();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

CEntityInstance * GetObservedPlayerController()
{
    __try {
        return ResolveObservedPlayerController();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool MirvPov_GetObserverState(uint8_t & observerMode, uint32_t & observerTarget)
{
    observerMode = 0;
    observerTarget = 0xFFFFFFFFu;

    __try {
        CEntityInstance * realPawn = GetRealLocalPlayerPawn();
        if(nullptr == realPawn || !realPawn->IsPlayerPawn()) return false;

        observerMode = static_cast<uint8_t>(realPawn->GetObserverMode());
        const auto targetHandle = realPawn->GetObserverTarget();
        if(targetHandle.IsValid()) observerTarget = targetHandle.ToInt();
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        observerMode = 0;
        observerTarget = 0xFFFFFFFFu;
        return false;
    }
}

CEntityInstance * GetRealLocalPlayerPawn()
{
    __try {
        return GetPawnFromController(GetRealSplitScreenPlayer(0));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
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
    if(!MirvPov_IsEnabled()) return;
    if(!g_pEngineToClient) return;
    SOURCESDK::CS2::IDemoFile * demoFile = g_pEngineToClient->GetDemoFile();
    if(!demoFile) {
        // Also discard demo-owned state on disconnect, even if the engine
        // has not started the next LevelInitPreEntity yet.
        if(g_MirvPovHadDemoFile) {
            MirvPov_OnLevelInitPreEntity();
        }
        return;
    }
    g_MirvPovHadDemoFile = true;
    if(MIRV_POV_FEATURE_ENABLED("hud")) MirvPovHud_ReapplyPanelState();
    const int demoTick = demoFile->GetDemoTick();
    if(MIRV_POV_FEATURE_ENABLED("deathcam")) MirvPovDeathCam_UpdateDemoTick(demoTick);
    if(MIRV_POV_FEATURE_ENABLED("hud")) MirvPovHud_UpdateSeekDetection(demoTick);
    if(MIRV_POV_FEATURE_ENABLED("killreward")) MirvPovKillReward_OnDemoTick(demoTick);
    if(MIRV_POV_FEATURE_ENABLED("radio")) MirvPovRadio_OnDemoTick(demoTick);
}

void MirvPov_OnFrameStageBefore(int frameStage)
{
    if(SOURCESDK::CS2::FRAME_RENDER_PASS != frameStage) return;
    if(!MIRV_POV_FEATURE_ENABLED("framestage")) return;

    if(MIRV_POV_FEATURE_ENABLED("voice")) MirvPovVoice_OnRenderPass();
    if(MIRV_POV_FEATURE_ENABLED("voiceban")) MirvPovVoiceBan_OnRenderPass();
    if(MIRV_POV_FEATURE_ENABLED("scoreboard")) MirvPovScoreboard_Update();
}

void MirvPov_OnFrameStageAfter(int frameStage)
{
    if(SOURCESDK::CS2::FRAME_RENDER_PASS == frameStage) {
        if(!MIRV_POV_FEATURE_ENABLED("framestage")) return;
        // Run after the game's native FrameStageNotify. Observer target and
        // Pawn state are committed there; sampling them before the original
        // call shifts fade cleanup and replay timing by one frame.
        if(MIRV_POV_FEATURE_ENABLED("deathcam")) MirvPovDeathCam_UpdateLifecycle();
        if(MIRV_POV_FEATURE_ENABLED("feedback")) {
            MirvPovFeedback_UpdatePovSelection();
            MirvPovDeathPanel_Update();
            RenderSystemDX11_DeathFade_UpdateObserverState();
        }
        MirvPov_UpdateSeekDetection();
        MirvPovBuyMenu_Update();
        if(MIRV_POV_FEATURE_ENABLED("voice")) MirvPovVoice_AfterRenderPass();
    }
}

void MirvPov_OnGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(MIRV_POV_FEATURE_ENABLED("deathcam")) MirvPovDeathCam_HandleGameEvent(event);
    if(MIRV_POV_FEATURE_ENABLED("feedback")) MirvPovFeedback_HandleGameEvent(event);
}

void MirvPov_OnPanoramaDllLoaded(HMODULE panoramaDll)
{
    if(MIRV_POV_FEATURE_ENABLED("hud")) MirvPovHud_OnPanoramaDllLoaded(panoramaDll);
}

void MirvPov_OnPanoramaLayoutFileLoaded(const char * filePath)
{
    if(MIRV_POV_FEATURE_ENABLED("hud")) MirvPovHud_OnPanoramaLayoutFileLoaded(filePath);
}

void MirvPov_OnLevelInitPreEntity()
{
    MirvPovBuyMenu_Reset();
    g_MirvPovHadDemoFile = false;
    // Do not disable/re-enable: hooks and user settings survive demo changes,
    // but entity handles, message queues and native object caches must not.
    MirvPovSoundCircle_ResetDemoState();
    MirvPovVoiceBan_ResetDemoState();
    MirvPovScoreboard_Reset(false);
    MirvPovVoice_ResetDemoState();
    MirvPovDeathCam_Reset();
    MirvPovFeedback_ResetPovSelection();
    MirvPovDeathPanel_Clear();
    RenderSystemDX11_DeathFade_Reset();
    RenderSystemDX11_DeathFade_ResetObserverState();
    MirvPovKillReward_Reset("demo changed");
    MirvPovRadio_ResetDemoState();
    if(MIRV_POV_FEATURE_ENABLED("hud")) MirvPovHud_OnLevelInitPreEntity();
}

void MirvPov_Enable(HMODULE clientDll)
{
    if(g_MirvPovEnabled) return;

    MirvPovDebug_ApplyFeatureConfiguration();
    g_MirvPovAutoSync = true;
    MirvPovScoreboard_Reset();
    if(MIRV_POV_FEATURE_ENABLED("soundcircle")) MirvPovSoundCircle_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("hud")) MirvPovHud_ApplyPatches(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("teamhealth")) MirvPovTeamHealth_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("radar")) MirvPov_ApplyRadarPatches(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("voice") && MirvPovVoice_IsEnabled()) MirvPov_HookVoiceHud(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("scoreboard")) MirvPovScoreboard_Initialize(clientDll);
    MirvPov_ResetVoiceHud();

    g_MirvPovEnabled = true;
    if(MIRV_POV_FEATURE_ENABLED("buymenu")) MirvPovBuyMenu_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("feedback")) MirvPovFeedback_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("deathcam")) MirvPovDeathCam_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("pickupprompt")) MirvPovPickupPrompt_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("killreward")) MirvPovKillReward_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("radio")) MirvPovRadio_Initialize(clientDll);
    if(MIRV_POV_FEATURE_ENABLED("voice") && MirvPovVoice_IsEnabled()) MirvPov_UpdateVoiceTeam();
    if(MIRV_POV_FEATURE_ENABLED("hud")) MirvPovHud_ReapplyPanelState();
}

void MirvPov_Disable()
{
    if(!g_MirvPovEnabled) return;
    MirvPovBuyMenu_Reset();
    // Native callbacks reached during restoration must already see pass-through.
    g_MirvPovEnabled = false;

    MirvPovScoreboard_Reset();
    MirvPovDeathPanel_Clear();
    RenderSystemDX11_DeathFade_Reset();
    RenderSystemDX11_DeathFade_ResetObserverState();
    MirvPovFeedback_ResetPovSelection();
    g_MirvPovAutoSync = false;
    MirvPovHud_RemovePatches();
    MirvPov_RemoveRadarPatches();
    MirvPovTeamID_RemovePatches();
    MirvPov_ResetVoiceHud();
    MirvPovDeathCam_Reset();
    MirvPovPickupPrompt_RemovePatches();
    MirvPovKillReward_ApplyHudChatDemoBypass(false);
    MirvPovKillReward_Reset("mirv_pov disabled");
    MirvPovRadio_Reset("mirv_pov disabled");
    g_MirvPovHadDemoFile = false;
}
