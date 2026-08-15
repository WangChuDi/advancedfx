#include "stdafx.h"

#include "GameEvents.h"
#include "MirvPovFeedback.h"

#include "../deps/release/prop/AfxHookSource/SourceSdkShared.h"
#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/utlstring.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#include "AfxHookSource2Rs.h"
#include "ClientEntitySystem.h"
#include "DeathMsg.h"
#include "MirvPovDeathPanel.h"
#include "MirvPovCore.h"
#include "MirvPovKillReward.h"
#include "MirvPovRadio.h"
#include "RenderSystemDX11Hooks.h"

#include <Windows.h>
#include "../deps/release/Detours/src/detours.h"

enum CS2GameEventKeyType
{
	CS2GameEventKeyType_Local = 0,
	CS2GameEventKeyType_CString = 1,
	CS2GameEventKeyType_Float = 2,
	CS2GameEventKeyType_Long = 3,
	CS2GameEventKeyType_Short = 4,
	CS2GameEventKeyType_Byte = 5,
	CS2GameEventKeyType_Bool = 6,
	CS2GameEventKeyType_Uint64 = 7
};


typedef SOURCESDK::CS2::CGameEvent * (*CGameEventManager_CreateEvent_t)( void * This, const char *name, bool bForce /*= false*/, int *pCookie /*= NULL*/ ); //:006
typedef bool (*CGameEventManager_FireEvent_t)( void * This, SOURCESDK::CS2::CGameEvent *event, bool bDontBroadcast /*= false*/ ); //:007
typedef bool (*CGameEventManager_FireEventClientSide_t)( void * This, SOURCESDK::CS2::CGameEvent *event ); //:008
typedef void (*CGameEventManager_FreeEvent_t)( void * This, SOURCESDK::CS2::CGameEvent *event ); //:010

void * g_pGameEventManager = nullptr;

CGameEventManager_CreateEvent_t g_CGameEventManager_CreateEvent = nullptr;
CGameEventManager_FreeEvent_t g_CGameEventManager_FreeEvent = nullptr;

CGameEventManager_FireEvent_t g_Old_CGameEventManager_FireEvent = nullptr;
CGameEventManager_FireEventClientSide_t g_Old_CGameEventManager_FireEventClientSide = nullptr;

static bool g_GameEventManagerHooked = false;

//extern const char * GetStringForSymbol(int value);

//typedef void ( * DebugPrintKV3_t)(const struct KeyValues3 *);
typedef bool ( * SaveKV3AsJSON_t)( const struct SOURCESDK::CS2::KeyValues3* kv, SOURCESDK::CS2::CUtlString* error, SOURCESDK::CS2::CUtlString* output );

//DebugPrintKV3_t g_DebugPrintKV3 = nullptr;
SaveKV3AsJSON_t g_SaveKV3AsJSON = nullptr;


void SendGameEvent(SOURCESDK::CS2::CGameEvent *event) {
	if(nullptr == event) return;

    static bool firstRun = true;
    if(firstRun) {
        firstRun = false;
		HMODULE hModule = GetModuleHandleA("tier0.dll");
		if (hModule)
		{
            //g_DebugPrintKV3 = (DebugPrintKV3_t)GetProcAddress(hModule,"?DebugPrintKV3@@YAXPEBVKeyValues3@@@Z");
			g_SaveKV3AsJSON = (SaveKV3AsJSON_t)GetProcAddress(hModule, "?SaveKV3AsJSON@@YA_NPEBVKeyValues3@@PEAVCUtlString@@1@Z");
		}        
    }

    SOURCESDK::CS2::CUtlString error;
    SOURCESDK::CS2::CUtlString output;

    if(nullptr == g_SaveKV3AsJSON) {
        static bool warned = false;
        if(!warned) {
            warned = true;
            advancedfx::Warning("[mirv_pov_feedback] SaveKV3AsJSON is unavailable; game event forwarding disabled.\n");
        }
        return;
    }

    if(g_SaveKV3AsJSON(event->GetDataKeys(),&error,&output) && nullptr != output) AfxHookSourceRs_Engine_OnGameEvent(event->GetName(), event->GetID(), output.Get());
    else advancedfx::Warning("Event: \"%s\" (%i): SaveKV3AsJSON failed: \"%s\"\n", event->GetName(), event->GetID(),error.Get() ? error.Get() : "[nullptr]");
}

static void HandleDeathFadeEvent(
    SOURCESDK::CS2::CGameEvent * event);

bool New_CGameEventManager_FireEvent( void * This, SOURCESDK::CS2::CGameEvent *event, bool bDontBroadcast /*= false*/ ) {
    g_pGameEventManager = This;
    MirvPovRadio_HandleGameEvent(event);

    return g_Old_CGameEventManager_FireEvent(This, event, bDontBroadcast);
}

extern bool g_b_on_game_event;

static bool IsDeathFadeEvent(const char * name) {
    return name
        && (0 == strcmp(name, "round_start")
            || 0 == strcmp(name, "player_hurt")
            || 0 == strcmp(name, "player_death")
            || 0 == strcmp(name, "player_spawn")
            || 0 == strcmp(name, "spec_target_updated"));
}

static void HandleDeathFadeEvent(
    SOURCESDK::CS2::CGameEvent * event)
{
    // This hook is installed globally, but the synthetic feedback path must
    // be completely inert during ordinary play/spectating. In particular,
    // do not resolve entities or call the native fade helper here unless the
    // user explicitly enabled mirv_pov.
    if(nullptr == event) return;

    const char * name = nullptr;
    __try {
        name = event->GetName();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    if(nullptr == name) return;

    // Learn only from a real local victim. Watched-player events in POV are
    // not the local player's native Fade source and must never overwrite the
    // normal hurt/death template or its timing sample.
    if((0 == strcmp(name, "player_hurt") || 0 == strcmp(name, "player_death"))
        && MirvPovFeedback_IsRealLocalPlayerVictim(event)) {
        if(0 == strcmp(name, "player_hurt")) {
            RenderSystemDX11_DeathFade_ObserveHurtEvent();
        } else {
            RenderSystemDX11_DeathFade_ObserveDeathEvent();
        }
    }

    if(!MirvPov_IsEnabled() || !IsDeathFadeEvent(name)) return;

    if(0 == strcmp(name, "round_start")) {
        MirvPovDeathPanel_Clear();
        RenderSystemDX11_DeathFade_Reset();
    } else if(0 == strcmp(name, "player_hurt")) {
        bool localVictim = MirvPovFeedback_IsLocalPlayerVictim(event);
        if(localVictim) RenderSystemDX11_DeathFade_Hurt();
    } else if(0 == strcmp(name, "player_death")) {
        bool localVictim = MirvPovFeedback_IsLocalPlayerVictim(event);
        if(localVictim) RenderSystemDX11_DeathFade_Death();
    } else if(0 == strcmp(name, "player_spawn")) {
        bool localVictim = MirvPovFeedback_IsLocalPlayerVictim(event);
        if(localVictim) {
            MirvPovDeathPanel_Clear();
            RenderSystemDX11_DeathFade_Reset();
        }
    } else if(0 == strcmp(name, "spec_target_updated")) {
        // A real local death is followed by an automatic spectator-target
        // update. The native DeathPanel is already the banner for that death;
        // hiding it here makes real deaths appear to have no banner at all.
        // Keep the observer/fade cleanup, but let the native panel lifetime
        // control when the banner disappears.
        RenderSystemDX11_DeathFade_ClearForObserverChange();
        RenderSystemDX11_DeathFade_ResetObserverState();
    }
}

bool New_CGameEventManager_FireEventClientSide( void * This, SOURCESDK::CS2::CGameEvent *event ) {
    g_pGameEventManager = This;

    if(g_b_on_game_event && nullptr != event) SendGameEvent(event);

    // Read and handle custom feedback before native dispatch. CS2 may recycle
    // the event object during FireEventClientSide; touching it after the
    // original call caused the 0xc0000005 seen at the end of console.log.
    HandleDeathFadeEvent(event);
    MirvPov_OnGameEvent(event);
    MirvPovKillReward_HandleGameEvent(event);
    MirvPovRadio_HandleGameEvent(event);

    // The native DeathPanel listener can show the panel during player_death,
    // but the same dispatch may immediately process the automatic observer
    // transition and hide/reset it. Capture the event name before dispatch so
    // the event object is never touched after the game's call returns. The
    // observer-update event is the end of the panel lifetime, so it must not
    // trigger another full Show after the target has changed.
    bool reapplyDeathPanelAfterDispatch = false;
    __try {
        const char * name = nullptr != event ? event->GetName() : nullptr;
        reapplyDeathPanelAfterDispatch = nullptr != name
            && 0 == strcmp(name, "player_death");
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        reapplyDeathPanelAfterDispatch = false;
    }

    bool result = g_Old_CGameEventManager_FireEventClientSide(This, event);
    if(reapplyDeathPanelAfterDispatch) {
        MirvPovDeathPanel_Reapply("FireEventClientSide-after-dispatch");
    }
    return result;
}

bool Hook_CGameEventManager(void* addrClientDll) {
    if(g_GameEventManagerHooked) return true;
    if(nullptr == addrClientDll) {
        return false;
    }

    size_t addressVtable = Afx::BinUtils::FindClassVtable(
        (HMODULE)addrClientDll,
        ".?AVCGameEventManager@@",
        0,
        0);
    if(0 == addressVtable) {
        return false;
    }

    MirvPovFeedback_Initialize((HMODULE)addrClientDll);
    RenderSystemDX11_DeathFade_Initialize((HMODULE)addrClientDll);
    void ** vtable = reinterpret_cast<void **>(addressVtable);
    g_CGameEventManager_CreateEvent = reinterpret_cast<CGameEventManager_CreateEvent_t>(vtable[6]);
    g_Old_CGameEventManager_FireEvent = reinterpret_cast<CGameEventManager_FireEvent_t>(vtable[7]);
    g_Old_CGameEventManager_FireEventClientSide = reinterpret_cast<CGameEventManager_FireEventClientSide_t>(vtable[8]);
    g_CGameEventManager_FreeEvent = reinterpret_cast<CGameEventManager_FreeEvent_t>(vtable[10]);
    if(nullptr == g_Old_CGameEventManager_FireEvent
        || nullptr == g_Old_CGameEventManager_FireEventClientSide) {
        return false;
    }

    LONG beginResult = DetourTransactionBegin();
    LONG updateResult = NO_ERROR;
    LONG fireAttachResult = NO_ERROR;
    LONG clientSideAttachResult = NO_ERROR;
    LONG transactionResult = -1;
    if(NO_ERROR == beginResult) updateResult = DetourUpdateThread(GetCurrentThread());
    if(NO_ERROR == beginResult && NO_ERROR == updateResult) {
        fireAttachResult = DetourAttach(
            &(PVOID&)g_Old_CGameEventManager_FireEvent,
            New_CGameEventManager_FireEvent);
        if(NO_ERROR == fireAttachResult) {
            clientSideAttachResult = DetourAttach(
                &(PVOID&)g_Old_CGameEventManager_FireEventClientSide,
                New_CGameEventManager_FireEventClientSide);
        }
        transactionResult = NO_ERROR == fireAttachResult
            && NO_ERROR == clientSideAttachResult
            ? DetourTransactionCommit()
            : DetourTransactionAbort();
    } else if(NO_ERROR == beginResult) {
        transactionResult = DetourTransactionAbort();
    }

    g_GameEventManagerHooked = NO_ERROR == beginResult
        && NO_ERROR == updateResult
        && NO_ERROR == fireAttachResult
        && NO_ERROR == clientSideAttachResult
        && NO_ERROR == transactionResult;
    return g_GameEventManagerHooked;
}
