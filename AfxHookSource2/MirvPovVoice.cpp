#include "stdafx.h"

#include "MirvPovVoice.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvTime.h"
#include "MirvPovCore.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/convar.h"
extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

#include "../shared/AfxConsole.h"
#include "../shared/AfxDetours.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <intrin.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

static constexpr int kMirvPovIsPlayingDemoVtableIndex = 42;
static constexpr int kMirvPovVoiceSeekThresholdTicks = 16;
static constexpr int kMirvPovVoiceSeekClearRenderPasses = 3;
static constexpr float kMirvPovSyntheticSpeakingSeconds = 0.65f;

static bool g_MirvPovVoiceEnabled = true;
static int g_MirvPovVoiceLastDemoTick = INT_MIN;
static int g_MirvPovVoiceClearRenderPasses = 0;
static bool g_MirvPovVoiceHadDemoFile = false;
static float g_MirvPovSyntheticSpeakingUntil[64] = {};
static size_t g_MirvPovShowSpeakerRetAddr = 0;
static size_t g_MirvPovServerVoiceDataAddr = 0;
static size_t g_MirvPovVoiceStatusGetAddr = 0;
static size_t g_MirvPovVoiceStatusUpdateSpeakerStatusAddr = 0;

typedef bool (*MirvPov_IsPlayingDemo_t)(void * This);
static MirvPov_IsPlayingDemo_t g_Org_MirvPov_IsPlayingDemo = nullptr;
static bool g_bMirvPovIsPlayingDemoHooked = false;

typedef __int64 (__fastcall * MirvPov_ServerVoiceData_t)(__int64 This, __int64 msg);
static MirvPov_ServerVoiceData_t g_Org_MirvPov_ServerVoiceData = nullptr;
static bool g_bMirvPovServerVoiceDataHooked = false;

typedef __int64 (__fastcall * MirvPov_VoiceStatus_Get_t)();
typedef __int64 (__fastcall * MirvPov_VoiceStatus_UpdateSpeakerStatus_t)(__int64 voiceStatus, unsigned int playerSlot, int localSlot, unsigned __int8 talking);

struct MirvPovVoiceMaskCvar {
    const char * name;
    SOURCESDK::CS2::ConVarHandle handle;
    int previousValue = 0;
    bool previousValueSaved = false;
};

static MirvPovVoiceMaskCvar g_MirvPovVoiceMaskLow = { "tv_listen_voice_indices" };
static MirvPovVoiceMaskCvar g_MirvPovVoiceMaskHigh = { "tv_listen_voice_indices_h" };

static SOURCESDK::CS2::Cvar_s * MirvPov_GetVoiceMaskCvar(MirvPovVoiceMaskCvar & state) {
    if(!SOURCESDK::CS2::g_pCVar) return nullptr;
    if(!state.handle.IsValid()) state.handle = SOURCESDK::CS2::g_pCVar->FindConVar(state.name, false);
    return state.handle.IsValid() ? SOURCESDK::CS2::g_pCVar->GetCvar(state.handle.Get()) : nullptr;
}

static void MirvPov_SetVoiceMaskCvar(MirvPovVoiceMaskCvar & state, int value) {
    SOURCESDK::CS2::Cvar_s * cvar = MirvPov_GetVoiceMaskCvar(state);
    if(!cvar) return;

    if(!state.previousValueSaved) {
        state.previousValue = cvar->m_Value.m_i32Value;
        state.previousValueSaved = true;
    }
    if(cvar->m_Value.m_i32Value == value) return;

    SOURCESDK::CS2::CVValue_t oldValue = {};
    SOURCESDK::CS2::CVValue_t newValue = {};
    memcpy(&oldValue, &cvar->m_Value, sizeof(oldValue));
    memcpy(&newValue, &cvar->m_Value, sizeof(newValue));
    newValue.m_i32Value = value;
    memcpy(&cvar->m_Value, &newValue, sizeof(newValue));
    SOURCESDK::CS2::g_pCVar->CallChangeCallback(state.handle, 0, &newValue, &oldValue);
}

static void MirvPov_RestoreVoiceMaskCvar(MirvPovVoiceMaskCvar & state) {
    if(!state.previousValueSaved) return;

    SOURCESDK::CS2::Cvar_s * cvar = MirvPov_GetVoiceMaskCvar(state);
    if(cvar) {
        SOURCESDK::CS2::CVValue_t oldValue = {};
        SOURCESDK::CS2::CVValue_t newValue = {};
        memcpy(&oldValue, &cvar->m_Value, sizeof(oldValue));
        memcpy(&newValue, &cvar->m_Value, sizeof(newValue));
        newValue.m_i32Value = state.previousValue;
        memcpy(&cvar->m_Value, &newValue, sizeof(newValue));
        SOURCESDK::CS2::g_pCVar->CallChangeCallback(state.handle, 0, &newValue, &oldValue);
    }
    state.previousValueSaved = false;
}

static bool MirvPov_IsVoiceHudReady() {
    return g_MirvPovVoiceStatusGetAddr && g_MirvPovVoiceStatusUpdateSpeakerStatusAddr;
}

static void MirvPov_SetSyntheticSpeaking(unsigned int playerSlot, bool speaking) {
    if(64 <= playerSlot || !MirvPov_IsVoiceHudReady()) return;
    __int64 voiceStatus = ((MirvPov_VoiceStatus_Get_t)g_MirvPovVoiceStatusGetAddr)();
    if(!voiceStatus) return;

    ((MirvPov_VoiceStatus_UpdateSpeakerStatus_t)g_MirvPovVoiceStatusUpdateSpeakerStatusAddr)(voiceStatus, playerSlot, -1, speaking ? 1 : 0);
    g_MirvPovSyntheticSpeakingUntil[playerSlot] = speaking ? g_MirvTime.curtime_get() + kMirvPovSyntheticSpeakingSeconds : 0.0f;
}

static void MirvPov_ForceClearSyntheticSpeaking() {
    for(int i = 0; i < 64; ++i) {
        if(0.0f < g_MirvPovSyntheticSpeakingUntil[i]) MirvPov_SetSyntheticSpeaking(i, false);
        g_MirvPovSyntheticSpeakingUntil[i] = 0.0f;
    }
}

static void MirvPov_RequestFullVoiceClear() {
    if(g_MirvPovVoiceClearRenderPasses < kMirvPovVoiceSeekClearRenderPasses) {
        g_MirvPovVoiceClearRenderPasses = kMirvPovVoiceSeekClearRenderPasses;
    }
}

static bool MirvPov_TryClearAllSpeakingSlots() {
    if(!MirvPov_IsVoiceHudReady()) return false;
    __int64 voiceStatus = ((MirvPov_VoiceStatus_Get_t)g_MirvPovVoiceStatusGetAddr)();
    if(!voiceStatus) return false;

    auto updateSpeakerStatus = (MirvPov_VoiceStatus_UpdateSpeakerStatus_t)g_MirvPovVoiceStatusUpdateSpeakerStatusAddr;
    for(unsigned int playerSlot = 0; playerSlot < 64; ++playerSlot) {
        updateSpeakerStatus(voiceStatus, playerSlot, -1, 0);
        g_MirvPovSyntheticSpeakingUntil[playerSlot] = 0.0f;
    }
    return true;
}

void MirvPov_ClearSyntheticSpeaking() {
    MirvPov_ForceClearSyntheticSpeaking();
}

static bool MirvPov_IsVoicePlayerSlotOnWatchedTeam(unsigned int playerSlot) {
    if(64 <= playerSlot) return false;

    CEntityInstance * watchedController = GetCurrentPovPlayerController();
    if(nullptr == watchedController || !watchedController->IsPlayerController()) return false;
    int watchedTeam = watchedController->GetTeam();
    if(watchedTeam != 2 && watchedTeam != 3) return false;

    CEntityInstance * voiceController = GetEntityFromIndex((int)playerSlot + 1);
    return nullptr != voiceController
        && voiceController->IsPlayerController()
        && voiceController->GetTeam() == watchedTeam;
}

void MirvPov_UpdateVoiceTeam() {
    if(!g_MirvPovVoiceEnabled || !MirvPov_IsEnabled()) return;

    CEntityInstance * watchedController = GetCurrentPovPlayerController();
    if(nullptr == watchedController || !watchedController->IsPlayerController()) return;

    int watchedTeam = watchedController->GetTeam();
    if(watchedTeam != 2 && watchedTeam != 3) return;

    uint32_t lowMask = 0;
    uint32_t highMask = 0;
    for(unsigned int playerSlot = 0; playerSlot < 64; ++playerSlot) {
        CEntityInstance * controller = GetEntityFromIndex((int)playerSlot + 1);
        if(nullptr == controller || !controller->IsPlayerController() || controller->GetTeam() != watchedTeam) continue;
        if(playerSlot < 32) lowMask |= uint32_t(1) << playerSlot;
        else highMask |= uint32_t(1) << (playerSlot - 32);
    }

    MirvPov_SetVoiceMaskCvar(g_MirvPovVoiceMaskLow, (int32_t)lowMask);
    MirvPov_SetVoiceMaskCvar(g_MirvPovVoiceMaskHigh, (int32_t)highMask);
}

static void MirvPov_UpdateSyntheticSpeakingExpiry() {
    float curTime = g_MirvTime.curtime_get();
    for(int i = 0; i < 64; ++i) {
        if(0.0f < g_MirvPovSyntheticSpeakingUntil[i] && g_MirvPovSyntheticSpeakingUntil[i] <= curTime) {
            MirvPov_SetSyntheticSpeaking(i, false);
        }
    }
}

static bool New_MirvPov_IsPlayingDemo(void * This) {
    void * ret = _ReturnAddress();
    bool result = g_Org_MirvPov_IsPlayingDemo(This);
    if(g_MirvPovVoiceEnabled
        && MirvPov_IsEnabled()
        && g_MirvPovShowSpeakerRetAddr
        && (size_t)ret == g_MirvPovShowSpeakerRetAddr) return false;
    return result;
}

static __int64 __fastcall New_MirvPov_ServerVoiceData(__int64 This, __int64 msg) {
    unsigned int playerSlot = msg ? *(unsigned int *)(msg + 104) : 0xFFFFFFFF;
    __int64 result = g_Org_MirvPov_ServerVoiceData(This, msg);
    if(0 == g_MirvPovVoiceClearRenderPasses
        && playerSlot < 64
        && g_MirvPovVoiceEnabled
        && MirvPov_IsEnabled()
        && MirvPov_IsVoicePlayerSlotOnWatchedTeam(playerSlot)) {
        MirvPov_SetSyntheticSpeaking(playerSlot, true);
    }
    return result;
}

static bool MirvPov_ResolveVoiceHud(HMODULE clientDll) {
    if(!clientDll) return false;

    if(!g_MirvPovShowSpeakerRetAddr) {
        size_t matchAddr = getAddress(clientDll, "48 63 ?? 48 8D 0D ?? ?? ?? ?? C6 84 08 ?? ?? ?? ?? 01 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 ?? ?? ?? ?? 84 C0 0F 85");
        if(matchAddr) g_MirvPovShowSpeakerRetAddr = matchAddr + 0x22;
    }
    if(!g_MirvPovServerVoiceDataAddr) {
        g_MirvPovServerVoiceDataAddr = getAddress(clientDll, "48 89 4C 24 ?? 53 55 56 57 41 54 41 55 41 57 48 81 EC");
    }
    if(!g_MirvPovVoiceStatusGetAddr) {
        g_MirvPovVoiceStatusGetAddr = getAddress(clientDll, "48 8B 05 ?? ?? ?? ?? C3 CC CC CC CC CC CC CC CC 48 8D 05");
    }
    if(!g_MirvPovVoiceStatusUpdateSpeakerStatusAddr) {
        g_MirvPovVoiceStatusUpdateSpeakerStatusAddr = getAddress(clientDll, "44 88 4C 24 ?? 44 89 44 24 ?? 89 54 24");
    }
    return g_MirvPovShowSpeakerRetAddr && g_MirvPovServerVoiceDataAddr && MirvPov_IsVoiceHudReady();
}

static bool MirvPov_EnsureVoiceHudHook(HMODULE clientDll) {
    if(!MirvPov_ResolveVoiceHud(clientDll)) return false;

    if(!g_bMirvPovIsPlayingDemoHooked) {
        if(!g_pEngineToClient) return false;
        void ** vtable = *(void ***)g_pEngineToClient;
        if(!vtable) return false;
        if(!AfxDetourPtr((PVOID*)&(vtable[kMirvPovIsPlayingDemoVtableIndex]), New_MirvPov_IsPlayingDemo, (PVOID*)&g_Org_MirvPov_IsPlayingDemo)) return false;
        g_bMirvPovIsPlayingDemoHooked = true;
    }

    if(!g_bMirvPovServerVoiceDataHooked) {
        g_Org_MirvPov_ServerVoiceData = (MirvPov_ServerVoiceData_t)g_MirvPovServerVoiceDataAddr;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(&(PVOID&)g_Org_MirvPov_ServerVoiceData, New_MirvPov_ServerVoiceData);
        if(NO_ERROR != DetourTransactionCommit()) return false;
        g_bMirvPovServerVoiceDataHooked = true;
    }
    return true;
}

void MirvPov_HookVoiceHud(HMODULE clientDll) {
    if(!MirvPov_EnsureVoiceHudHook(clientDll)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_voice_hud] voice HUD patterns or hooks not available\n");
    }
}

static void MirvPov_UpdateVoiceRuntime() {
    if(!g_pEngineToClient) return;

    SOURCESDK::CS2::IDemoFile * pDemoFile = g_pEngineToClient->GetDemoFile();
    if(!pDemoFile) {
        if(g_MirvPovVoiceHadDemoFile) {
            g_pEngineToClient->ExecuteClientCmd(0, "servervoice_clear", true);
            MirvPov_RequestFullVoiceClear();
        }
        g_MirvPovVoiceHadDemoFile = false;
        g_MirvPovVoiceLastDemoTick = INT_MIN;
        return;
    }

    g_MirvPovVoiceHadDemoFile = true;
    int curTick = pDemoFile->GetDemoTick();
    if(g_MirvPovVoiceLastDemoTick != INT_MIN) {
        int delta = curTick - g_MirvPovVoiceLastDemoTick;
        if(delta < 0 || delta > kMirvPovVoiceSeekThresholdTicks) {
            g_pEngineToClient->ExecuteClientCmd(0, "servervoice_clear", true);
            MirvPov_RequestFullVoiceClear();
        }
    }
    g_MirvPovVoiceLastDemoTick = curTick;
    MirvPov_UpdateSyntheticSpeakingExpiry();
}

void MirvPov_UpdateVoiceHud() {
    if(!g_MirvPovVoiceEnabled || !MirvPov_IsEnabled()) return;
    MirvPov_UpdateVoiceTeam();
    MirvPov_UpdateVoiceRuntime();
}

void MirvPovVoice_OnRenderPass()
{
    if(g_MirvPovVoiceEnabled && MirvPov_IsEnabled()) {
        MirvPov_UpdateVoiceTeam();
        MirvPov_UpdateVoiceRuntime();
    }
}

void MirvPovVoice_AfterRenderPass()
{
    if(0 < g_MirvPovVoiceClearRenderPasses && MirvPov_TryClearAllSpeakingSlots()) {
        --g_MirvPovVoiceClearRenderPasses;
    }
}

void MirvPov_ResetVoiceHud() {
    g_MirvPovVoiceLastDemoTick = INT_MIN;
    g_MirvPovVoiceHadDemoFile = false;
    MirvPov_RestoreVoiceMaskCvar(g_MirvPovVoiceMaskLow);
    MirvPov_RestoreVoiceMaskCvar(g_MirvPovVoiceMaskHigh);
    MirvPov_RequestFullVoiceClear();
}

bool MirvPovVoice_IsEnabled()
{
    return g_MirvPovVoiceEnabled;
}

void MirvPovVoice_SetEnabled(bool enabled)
{
    if(g_MirvPovVoiceEnabled == enabled) return;

    g_MirvPovVoiceEnabled = enabled;
    MirvPov_ResetVoiceHud();

    if(enabled && MirvPov_IsEnabled()) {
        MirvPov_HookVoiceHud(GetModuleHandleW(L"client.dll"));
        MirvPov_UpdateVoiceTeam();
    }
}
