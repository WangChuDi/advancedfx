#pragma once

#include <Windows.h>
#include <cstdint>
#include <cstddef>

class CEntityInstance;

using MirvPovHashString_t = unsigned int (__fastcall *)(
    const char * string,
    unsigned int length,
    unsigned int lengthXorSeed);

namespace SOURCESDK {
namespace CS2 {
class IGameEvent;
class CEntityInstance;
struct CKV3MemberName;
}
}

// The DeathPanel detours are discovered and installed by DeathMsg.cpp because
// that is where HLAE's native death-notice hook lives. Their mutable runtime
// state is kept here so POV playback state is not mixed with mirv_deathmsg's
// Panorama customization state.
using MirvPovDeathPanelConstructor_t = unsigned char * (__fastcall *)(unsigned char * deathPanel);
using MirvPovDeathPanelDestructor_t = unsigned char * (__fastcall *)(
    unsigned char * deathPanel,
    unsigned int deleteFlags);
using MirvPovDeathPanelResolveReplayValue_t = unsigned char * (__fastcall *)(void * object, __int64 index);
using MirvPovDeathPanelSetVisible_t = __int64 (__fastcall *)(unsigned char * deathPanel, bool visible);
using MirvPovDeathPanelHide_t = __int64 (__fastcall *)(unsigned char * deathPanel);
using MirvPovDeathPanelShow_t = void (__fastcall *)(unsigned char * deathPanel);
using MirvPovDeathPanelGetLocalPawn_t = CEntityInstance * (__fastcall *)(int slot);
using MirvPovDeathPanelHandlePlayerDeathListener_t = unsigned char * (__fastcall *)(
    unsigned char * listener,
    SOURCESDK::CS2::IGameEvent * event);

struct MirvPovDeathPanelState {
    MirvPovDeathPanelHandlePlayerDeathListener_t originalHandlePlayerDeath = nullptr;
    MirvPovDeathPanelConstructor_t originalConstructor = nullptr;
    MirvPovDeathPanelDestructor_t originalDestructor = nullptr;
    MirvPovDeathPanelResolveReplayValue_t resolveReplayValue = nullptr;
    void * replayObject = nullptr;
    void ** replayFallbackObject = nullptr;
    MirvPovDeathPanelSetVisible_t setMainVisible = nullptr;
    MirvPovDeathPanelSetVisible_t setSecondaryVisible = nullptr;
    MirvPovDeathPanelHide_t hide = nullptr;
    MirvPovDeathPanelShow_t show = nullptr;
    MirvPovDeathPanelGetLocalPawn_t originalGetLocalPawn = nullptr;

    bool localPawnHooked = false;
    bool hideHooked = false;
    unsigned char * lastPanel = nullptr;
    unsigned char * nativeInstance = nullptr;
    bool constructorHooked = false;
    bool destructorHooked = false;

    unsigned char * reapplyPanel = nullptr;
    CEntityInstance * reapplyPawn = nullptr;
    uint32_t reapplyPawnHandle = 0xFFFFFFFFu;
    int reapplyFrame = -1;
    int lastRefreshFrame = -1;
    int lastSuppressedHideFrame = -1;
    bool reapplyArmed = false;
};

extern MirvPovDeathPanelState g_MirvPovDeathPanelState;
extern thread_local CEntityInstance * g_MirvPovDeathPanelLocalPawnOverride;
extern MirvPovHashString_t g_MirvPovHashString;

SOURCESDK::CS2::CKV3MemberName MirvPovDeathPanel_MakeGameEventKey(const char * name);
int MirvPovDeathPanel_TryGetHeadshot(SOURCESDK::CS2::IGameEvent * gameEvent);
void MirvPovDeathPanel_ResolveAddresses(HMODULE clientDll);
size_t MirvPovDeathPanel_ResolveEntityTokenAddress(HMODULE clientDll);

// DeathMsg.cpp owns the native hook implementation. These functions are kept
// private to the split implementation and reached through the public façade
// below so callers do not depend on DeathMsg.cpp internals.
void MirvPovDeathPanelImpl_Clear();
bool MirvPovDeathPanelImpl_Reapply(const char * source);
void MirvPovDeathPanelImpl_Update();

void MirvPovDeathPanel_Clear();
bool MirvPovDeathPanel_Reapply(const char * source);
void MirvPovDeathPanel_Update();
