#include "stdafx.h"

#include "MirvPovDeathPanel.h"

#include "Globals.h"
#include "addresses.h"
#include "ClientEntitySystem.h"
#include "../shared/binutils.h"

#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <cstring>

MirvPovDeathPanelState g_MirvPovDeathPanelState;
thread_local CEntityInstance * g_MirvPovDeathPanelLocalPawnOverride = nullptr;
MirvPovHashString_t g_MirvPovHashString = nullptr;

SOURCESDK::CS2::CKV3MemberName MirvPovDeathPanel_MakeGameEventKey(const char * name)
{
    if(nullptr == name) return SOURCESDK::CS2::CKV3MemberName(0, -1, "");

    const size_t length = std::strlen(name);
    if(nullptr == g_MirvPovHashString) {
        return SOURCESDK::CS2::CKV3MemberName(0, -1, name);
    }

    const unsigned int hash = g_MirvPovHashString(
        name,
        static_cast<unsigned int>(length),
        static_cast<unsigned int>(length) ^ 0x31415926);
    return SOURCESDK::CS2::CKV3MemberName(static_cast<int>(hash), -1, name);
}

int MirvPovDeathPanel_TryGetHeadshot(SOURCESDK::CS2::IGameEvent * gameEvent)
{
    if(nullptr == gameEvent || nullptr == g_MirvPovHashString) return -1;
    __try {
        return gameEvent->GetInt(MirvPovDeathPanel_MakeGameEventKey("headshot"));
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

static bool IsExecutableAddress(const void * address)
{
    if(nullptr == address) return false;
    MEMORY_BASIC_INFORMATION info = {};
    if(0 == VirtualQuery(address, &info, sizeof(info))) return false;
    const DWORD protection = info.Protect & 0xFF;
    return MEM_COMMIT == info.State
        && (PAGE_EXECUTE == protection || PAGE_EXECUTE_READ == protection
            || PAGE_EXECUTE_READWRITE == protection || PAGE_EXECUTE_WRITECOPY == protection);
}

void MirvPovDeathPanel_ResolveAddresses(HMODULE clientDll)
{
    if(nullptr == clientDll) return;

    if(auto address = getAddress(clientDll,
        "48 83 EC 28 45 8B D0 4C 8B C9 48 83 FA 04 0F 82 ?? ?? ?? ?? "
        "0F B6 09 48 89 5C 24 20 8D 41 BF 3C 19 77 03 80 C1 20")) {
        g_MirvPovHashString = reinterpret_cast<MirvPovHashString_t>(address);
    }

    if(auto address = getAddress(clientDll, "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 48 8B 02 48 8B F9 48 8B CA")) {
        g_MirvPovDeathPanelState.originalHandlePlayerDeath = reinterpret_cast<MirvPovDeathPanelHandlePlayerDeathListener_t>(address);
    }

    auto constructorAddress = getAddress(clientDll,
        "48 89 5C 24 18 55 57 41 56 48 8B EC 48 83 EC 40 "
        "48 8B F9 E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? "
        "48 8D 4F 20 E8 ?? ?? ?? ?? 45 33 F6 48 8D 05 ?? ?? ?? ?? 48 89 07");
    g_MirvPovDeathPanelState.originalConstructor = IsExecutableAddress(reinterpret_cast<void *>(constructorAddress))
        ? reinterpret_cast<MirvPovDeathPanelConstructor_t>(constructorAddress) : nullptr;

    auto destructorAddress = getAddress(clientDll,
        "48 89 5C 24 08 57 48 83 EC 20 8B DA 48 8B F9 "
        "E8 ?? ?? ?? ?? F6 C3 01 74 ?? BA F0 00 00 00 "
        "48 8B CF E8 ?? ?? ?? ?? 48 8B 5C 24 30 48 8B C7 "
        "48 83 C4 20 5F C3");
    g_MirvPovDeathPanelState.originalDestructor = IsExecutableAddress(reinterpret_cast<void *>(destructorAddress))
        ? reinterpret_cast<MirvPovDeathPanelDestructor_t>(destructorAddress) : nullptr;

    g_MirvPovDeathPanelState.hide = reinterpret_cast<MirvPovDeathPanelHide_t>(getAddress(clientDll,
        "48 89 5C 24 ?? 57 48 83 EC ?? 65 48 8B 04 25 ?? ?? ?? ?? 48 8B D9 8B 15 ?? ?? ?? ?? C6 05"));
    g_MirvPovDeathPanelState.setMainVisible = reinterpret_cast<MirvPovDeathPanelSetVisible_t>(getAddress(clientDll,
        "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 65 48 8B 04 25 ?? ?? ?? ?? 48 8B D9 44 8B 05 ?? ?? ?? ?? 0F B6 F2"));
    g_MirvPovDeathPanelState.setSecondaryVisible = reinterpret_cast<MirvPovDeathPanelSetVisible_t>(getAddress(clientDll,
        "48 89 5C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 65 48 8B 04 25 ?? ?? ?? ?? 48 8B F9 44 8B 05"));
    g_MirvPovDeathPanelState.show = reinterpret_cast<MirvPovDeathPanelShow_t>(getAddress(clientDll,
        "40 57 41 56 48 81 EC ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8B F9 C6 05 ?? ?? ?? ?? ?? 33 C9 F3 0F 10 40 30"));
    g_MirvPovDeathPanelState.originalGetLocalPawn = reinterpret_cast<MirvPovDeathPanelGetLocalPawn_t>(getAddress(clientDll,
        "48 83 EC 28 83 F9 FF 75 ?? 48 8B 0D ?? ?? ?? ?? 48 8D 54 24 ?? 48 8B 01 FF 90 ?? ?? ?? ?? 8B 08 48 63 C1 4C 8D 05 ?? ?? ?? ?? 33 D2 4D 8B 04 C0 4D 85 C0"));

    auto replayGate = reinterpret_cast<unsigned char *>(getAddress(clientDll,
        "48 8D 0D ?? ?? ?? ?? 44 89 87 08 01 00 00 45 33 F6 E8 ?? ?? ?? ?? 85 C0 0F 85 ?? ?? ?? ?? BA FF FF FF FF 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 85 C0 75 0B 48 8B 05 ?? ?? ?? ?? 48 8B 40 08 44 38 30"));
    if(nullptr != replayGate) {
        g_MirvPovDeathPanelState.replayObject = replayGate + 42 + *reinterpret_cast<int32_t *>(replayGate + 38);
        g_MirvPovDeathPanelState.resolveReplayValue = reinterpret_cast<MirvPovDeathPanelResolveReplayValue_t>(replayGate + 47 + *reinterpret_cast<int32_t *>(replayGate + 43));
        g_MirvPovDeathPanelState.replayFallbackObject = reinterpret_cast<void **>(replayGate + 59 + *reinterpret_cast<int32_t *>(replayGate + 55));
    }
}

size_t MirvPovDeathPanel_ResolveEntityTokenAddress(HMODULE clientDll)
{
    if(nullptr == clientDll) return 0;
    return getAddress(clientDll,
        "40 53 48 83 EC ?? 8B 51 ?? 48 8B D9 83 FA FF 0F 84 ?? ?? ?? ?? "
        "4C 8B 0D ?? ?? ?? ??");
}

void MirvPovDeathPanel_Clear()
{
    MirvPovDeathPanelImpl_Clear();
}

bool MirvPovDeathPanel_Reapply(const char * source)
{
    return MirvPovDeathPanelImpl_Reapply(source);
}

void MirvPovDeathPanel_Update()
{
    MirvPovDeathPanelImpl_Update();
}
