#include "stdafx.h"

#include "MirvPovVoiceBan.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "SchemaSystem.h"
#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <stdint.h>
#include <string.h>
#include <unordered_set>

namespace {

bool g_Enabled = false;
uint8_t * g_PatchAddress = nullptr;
uint8_t g_OriginalImmediate = 0;
bool g_ResolveAttempted = false;
bool g_PatchApplied = false;
size_t g_VoiceStatusGetterAddress = 0;
size_t g_SetPlayerBlockedStateAddress = 0;
std::unordered_set<uint64_t> g_ClearedSteamIds;

using GetVoiceStatus_t = __int64 (__fastcall *)();
using SetPlayerBlockedState_t = void * (__fastcall *)(
    __int64 voiceStatus,
    uint64_t steamId,
    unsigned __int8 blocked,
    unsigned __int8 persistBlocked);

bool ResolvePatch()
{
    if(g_PatchAddress) return true;

    HMODULE clientDll = GetModuleHandleW(L"client.dll");
    if(!clientDll || g_ResolveAttempted) return false;
    g_ResolveAttempted = true;

    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
    if(!sections.Eof()) textRange = sections.GetMemRange();
    if(textRange.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] client.dll text section not found.\n");
        return false;
    }

    const char * pattern =
        "E8 ?? ?? ?? ?? 41 B1 01 49 8B D5 45 0F B6 C1 48 8B C8 E8 ?? ?? ?? ?? 48 63 05";
    auto sequence = Afx::BinUtils::FindPatternString(textRange, pattern);
    if(sequence.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] communication-abuse block call-site not found.\n");
        return false;
    }
    auto remaining = Afx::BinUtils::MemRange(sequence.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(remaining, pattern).IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] communication-abuse block call-site is not unique.\n");
        return false;
    }

    uint8_t * bytes = reinterpret_cast<uint8_t *>(sequence.Start);
    if(0xE8 != bytes[0] || 0xE8 != bytes[18]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] communication-abuse block call-site has unexpected calls.\n");
        return false;
    }

    int32_t getterRelative = 0;
    int32_t setterRelative = 0;
    memcpy(&getterRelative, bytes + 1, sizeof(getterRelative));
    memcpy(&setterRelative, bytes + 19, sizeof(setterRelative));
    size_t getterAddress = static_cast<size_t>(
        static_cast<intptr_t>(sequence.Start + 5) + getterRelative);
    size_t setterAddress = static_cast<size_t>(
        static_cast<intptr_t>(sequence.Start + 23) + setterRelative);
    if(getterAddress < textRange.Start || textRange.End <= getterAddress
        || setterAddress < textRange.Start || textRange.End <= setterAddress) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] communication-abuse block call targets are invalid.\n");
        return false;
    }

    uint8_t * immediate = reinterpret_cast<uint8_t *>(sequence.Start + 7);
    if(0x01 != *immediate) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] communication-abuse block call-site has unexpected bytes.\n");
        return false;
    }

    g_PatchAddress = immediate;
    g_OriginalImmediate = *immediate;
    g_VoiceStatusGetterAddress = getterAddress;
    g_SetPlayerBlockedStateAddress = setterAddress;
    return true;
}

bool SetPatch(bool enabled)
{
    if(enabled == g_PatchApplied) return true;
    if(!ResolvePatch()) return false;

    uint8_t expectedCurrent = g_PatchApplied ? 0x00 : g_OriginalImmediate;
    if(expectedCurrent != *g_PatchAddress) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] communication-abuse block call-site changed unexpectedly.\n");
        return false;
    }

    DWORD oldProtect = 0;
    if(!VirtualProtect(g_PatchAddress, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] VirtualProtect failed (error %lu).\n", GetLastError());
        return false;
    }

    uint8_t previous = *g_PatchAddress;
    uint8_t expected = enabled ? 0x00 : g_OriginalImmediate;
    *g_PatchAddress = expected;
    bool written = expected == *g_PatchAddress
        && 0 != FlushInstructionCache(GetCurrentProcess(), g_PatchAddress, 1);
    if(!written) {
        *g_PatchAddress = previous;
        FlushInstructionCache(GetCurrentProcess(), g_PatchAddress, 1);
    }

    DWORD unused = 0;
    if(!VirtualProtect(g_PatchAddress, 1, oldProtect, &unused)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] Failed to restore page protection (error %lu).\n", GetLastError());
    }
    if(!written) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_voicebanFix] Failed to update communication-abuse block call-site.\n");
        return false;
    }

    g_PatchApplied = enabled;
    return true;
}

bool TryGetAbuseMutedSteamId(
    CEntityInstance * controller,
    ptrdiff_t muteOffset,
    uint64_t & steamId)
{
    __try {
        if(nullptr == controller || !controller->IsPlayerController()) return false;
        steamId = controller->GetSteamId();
        if(0 == steamId) return false;
        const bool * abuseMuted = reinterpret_cast<const bool *>(
            reinterpret_cast<const uint8_t *>(controller) + muteOffset);
        return *abuseMuted;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryClearBlockedState(__int64 voiceStatus, uint64_t steamId)
{
    __try {
        reinterpret_cast<SetPlayerBlockedState_t>(g_SetPlayerBlockedStateAddress)(
            voiceStatus,
            steamId,
            0,
            0);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Update()
{
    bool enabled = MirvPov_IsEnabled() || g_Enabled;
    if(!SetPatch(enabled)) return;
    if(!enabled) {
        g_ClearedSteamIds.clear();
        return;
    }

    ptrdiff_t muteOffset = g_clientDllOffsets.CCSPlayerController.m_bHasCommunicationAbuseMute;
    if(muteOffset < 0 || !g_VoiceStatusGetterAddress || !g_SetPlayerBlockedStateAddress) return;

    __int64 voiceStatus = reinterpret_cast<GetVoiceStatus_t>(g_VoiceStatusGetterAddress)();
    if(!voiceStatus) return;

    for(int playerSlot = 0; playerSlot < 64; ++playerSlot) {
        uint64_t steamId = 0;
        if(!TryGetAbuseMutedSteamId(GetEntityFromIndex(playerSlot + 1), muteOffset, steamId)) continue;
        if(g_ClearedSteamIds.end() != g_ClearedSteamIds.find(steamId)) continue;
        if(TryClearBlockedState(voiceStatus, steamId)) g_ClearedSteamIds.insert(steamId);
    }
}

} // namespace

void MirvPovVoiceBan_OnRenderPass()
{
    Update();
}

CON_COMMAND(mirv_voicebanFix, "Ignore communication-abuse mute flags without modifying voice messages.")
{
    int argc = args->ArgC();
    auto arg0 = args->ArgV(0);

    if(2 <= argc) {
        bool enable = 0 != atoi(args->ArgV(1));
        g_Enabled = enable;
        Update();
        MIRV_POV_DIAGNOSTIC_MESSAGE("%s: %s\n", arg0, enable ? "enabled" : "disabled");
        return;
    }

    MIRV_POV_DIAGNOSTIC_MESSAGE(
        "%s <0|1> - Prevent communication-abuse processing from setting local block flags (default: 0).\n"
        "Current value: %d\n",
        arg0,
        g_Enabled ? 1 : 0);
}
