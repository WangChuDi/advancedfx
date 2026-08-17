#include "stdafx.h"

#include "MirvPovScoreboard.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovCore.h"
#include "MirvPovHud.h"

#include "../deps/release/prop/cs2/sdk_src/public/cdll_int.h"
#include "../deps/release/Detours/src/detours.h"
#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#include <stdint.h>
#include <string.h>

extern SOURCESDK::CS2::ISource2EngineToClient * g_pEngineToClient;

namespace {

bool g_ScoreboardSyncEnabled = false;
bool g_ScoreboardWasOpen = false;
bool g_HltvScoreboardOpen = false;
bool g_UserCmdScoreboardOpen[256] = {};

unsigned char * (__fastcall * g_OrgHltvParse)(
    __int64 This,
    unsigned char * data,
    __int64 parseContext) = nullptr;
bool g_HltvHooked = false;

void (__fastcall * g_OrgUserCommandsHandler)(void * This, void * msg) = nullptr;
bool g_UserCommandsHooked = false;

bool ReadVarInt(
    const unsigned char * end,
    const unsigned char *& cursor,
    uint64_t & value)
{
    uint64_t result = 0;
    unsigned int shift = 0;
    while(cursor < end && shift < 64) {
        unsigned char byte = *cursor++;
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if(0 == (byte & 0x80)) {
            value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool SkipProtoField(
    const unsigned char * end,
    const unsigned char *& cursor,
    uint64_t wireType)
{
    if(0 == wireType) {
        uint64_t ignored = 0;
        return ReadVarInt(end, cursor, ignored);
    }
    if(1 == wireType) {
        if(end - cursor < 8) return false;
        cursor += 8;
        return true;
    }
    if(2 == wireType) {
        uint64_t length = 0;
        if(!ReadVarInt(end, cursor, length) || static_cast<uint64_t>(end - cursor) < length) {
            return false;
        }
        cursor += length;
        return true;
    }
    if(5 == wireType) {
        if(end - cursor < 4) return false;
        cursor += 4;
        return true;
    }
    return false;
}

bool FindProtoMessageField(
    const unsigned char * data,
    size_t size,
    uint64_t wantedField,
    const unsigned char *& nested,
    size_t & nestedSize)
{
    const unsigned char * cursor = data;
    const unsigned char * end = data + size;
    while(cursor < end) {
        uint64_t key = 0;
        if(!ReadVarInt(end, cursor, key)) return false;
        uint64_t field = key >> 3;
        uint64_t wireType = key & 7;
        if(2 == wireType) {
            uint64_t length = 0;
            if(!ReadVarInt(end, cursor, length)
                || static_cast<uint64_t>(end - cursor) < length) return false;
            if(field == wantedField) {
                nested = cursor;
                nestedSize = static_cast<size_t>(length);
                return true;
            }
            cursor += length;
        } else if(!SkipProtoField(end, cursor, wireType)) {
            return false;
        }
    }
    return false;
}

bool ParseButtonState(
    const unsigned char * data,
    size_t size,
    uint64_t states[3])
{
    const unsigned char * cursor = data;
    const unsigned char * end = data + size;
    bool seen = false;
    while(cursor < end) {
        uint64_t key = 0;
        if(!ReadVarInt(end, cursor, key)) return seen;
        uint64_t field = key >> 3;
        uint64_t wireType = key & 7;
        if(0 == wireType) {
            uint64_t value = 0;
            if(!ReadVarInt(end, cursor, value)) return seen;
            if(1 <= field && field <= 3) {
                states[field - 1] = value;
                seen = true;
            }
        } else if(!SkipProtoField(end, cursor, wireType)) {
            return seen;
        }
    }
    return seen;
}

bool ParseBaseUserCmdButtons(
    const unsigned char * data,
    size_t size,
    uint64_t states[3])
{
    const unsigned char * baseUserCmd = nullptr;
    size_t baseUserCmdSize = 0;
    if(!FindProtoMessageField(data, size, 1, baseUserCmd, baseUserCmdSize)) return false;

    const unsigned char * buttons = nullptr;
    size_t buttonsSize = 0;
    if(!FindProtoMessageField(baseUserCmd, baseUserCmdSize, 3, buttons, buttonsSize)) return false;
    return ParseButtonState(buttons, buttonsSize, states);
}

bool ReadProtoBytes(void * value, const unsigned char *& data, size_t & size)
{
    unsigned char * bytes = static_cast<unsigned char *>(value);
    uint64_t byteCount = *reinterpret_cast<uint64_t *>(bytes + 0x10);
    uint64_t capacity = *reinterpret_cast<uint64_t *>(bytes + 0x18);
    data = capacity >= 0x10
        ? *reinterpret_cast<const unsigned char **>(bytes)
        : bytes;
    size = static_cast<size_t>(byteCount);
    return nullptr != data && 0 < size && size <= 0x10000;
}

void ParseUserCommandsMessage(void * msg)
{
    if(!msg) return;
    void * entriesBase = *reinterpret_cast<void **>(static_cast<unsigned char *>(msg) + 0x50);
    int count = *reinterpret_cast<int *>(static_cast<unsigned char *>(msg) + 0x48);
    if(!entriesBase || count <= 0 || count > 256) return;

    void ** entries = reinterpret_cast<void **>(static_cast<unsigned char *>(entriesBase) + 0x8);
    for(int i = 0; i < count; ++i) {
        void * entry = entries[i];
        if(!entry) continue;

        void * protoBytes = reinterpret_cast<void *>(
            *reinterpret_cast<uint64_t *>(static_cast<unsigned char *>(entry) + 0x18)
            & ~static_cast<uintptr_t>(3));
        const unsigned char * data = nullptr;
        size_t size = 0;
        if(!ReadProtoBytes(protoBytes, data, size)) continue;

        uint64_t states[3] = {};
        if(!ParseBaseUserCmdButtons(data, size, states)) continue;
        int playerId = *reinterpret_cast<int *>(static_cast<unsigned char *>(entry) + 0x34);
        if(0 <= playerId && playerId < 256) {
            g_UserCmdScoreboardOpen[playerId] = 0 != (states[0] & 0x200000000ull);
        }
    }
}

int GetCurrentTargetPlayerSlot()
{
    CEntityInstance * controller = GetCurrentPovPlayerController();
    if(nullptr == controller) return -1;
    auto handle = controller->GetHandle();
    int index = handle.IsValid() ? handle.GetEntryIndex() : -1;
    return 1 <= index && index <= 64 ? index - 1 : -1;
}

void SetScoreboardOpen(bool open)
{
    if(g_ScoreboardWasOpen == open) return;
    if(g_pEngineToClient) {
        g_pEngineToClient->ExecuteClientCmd(0, open ? "+showscores" : "-showscores", true);
    }
    g_ScoreboardWasOpen = open;
}

unsigned char * __fastcall NewHltvParse(
    __int64 This,
    unsigned char * data,
    __int64 parseContext)
{
    unsigned char * result = g_OrgHltvParse(This, data, parseContext);
    g_HltvScoreboardOpen = 0 != *reinterpret_cast<unsigned char *>(This + 0x40);
    return result;
}

void __fastcall NewUserCommandsHandler(void * This, void * msg)
{
    g_OrgUserCommandsHandler(This, msg);
    ParseUserCommandsMessage(msg);
}

void HookHltvParser(HMODULE clientDll)
{
    if(g_HltvHooked || nullptr == clientDll) return;
    size_t address = getAddress(clientDll, "48 89 5C 24 18 48 89 6C 24 20 56 41 56 41 57 48 81 EC 80 00 00 00 49 8B E8 48 89 BC 24 A8 00 00 00 48 8B DA 4C 8D 3D ?? ?? ?? ?? 48 8B F1 45 33 F6 48 3B 5D 00 72 32 8B C3 2B 45 08 3B 45 1C 0F 84 A2 03 00 00 44 8B 4D 5C");
    if(0 == address) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_scoreboard] HLTV parser pattern not found.\n");
        return;
    }

    g_OrgHltvParse = reinterpret_cast<decltype(g_OrgHltvParse)>(address);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgHltvParse, NewHltvParse);
    if(NO_ERROR == DetourTransactionCommit()) {
        g_HltvHooked = true;
    } else {
        g_OrgHltvParse = nullptr;
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_scoreboard] HLTV parser detour failed.\n");
    }
}

void HookUserCommands(HMODULE clientDll)
{
    if(g_UserCommandsHooked || nullptr == clientDll) return;
    size_t address = getAddress(clientDll, "4C 8B DC 49 89 53 10 49 89 4B 08 55 53 57 49 8D AB 38 FF FF FF 48 81 EC B0 01 00 00 48 63 42 48");
    if(0 == address) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_scoreboard] UserCommands handler pattern not found.\n");
        return;
    }

    g_OrgUserCommandsHandler = reinterpret_cast<decltype(g_OrgUserCommandsHandler)>(address);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgUserCommandsHandler, NewUserCommandsHandler);
    if(NO_ERROR == DetourTransactionCommit()) {
        g_UserCommandsHooked = true;
    } else {
        g_OrgUserCommandsHandler = nullptr;
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_scoreboard] UserCommands handler detour failed.\n");
    }
}

} // namespace

void MirvPovScoreboard_Initialize(HMODULE clientDll)
{
    HookHltvParser(clientDll);
    HookUserCommands(clientDll);
}

void MirvPovScoreboard_Reset()
{
    SetScoreboardOpen(false);
    g_ScoreboardSyncEnabled = false;
    g_HltvScoreboardOpen = false;
    memset(g_UserCmdScoreboardOpen, 0, sizeof(g_UserCmdScoreboardOpen));
}

void MirvPovScoreboard_Update()
{
    if(!MirvPov_IsEnabled() || !g_ScoreboardSyncEnabled || MirvPovHud_ShouldSuppressFrame()) {
        SetScoreboardOpen(false);
        return;
    }

    int playerSlot = GetCurrentTargetPlayerSlot();
    bool userCmdOpen = 0 <= playerSlot && g_UserCmdScoreboardOpen[playerSlot];
    SetScoreboardOpen(g_HltvScoreboardOpen || userCmdOpen);
}

bool MirvPovScoreboard_IsEnabled()
{
    return g_ScoreboardSyncEnabled;
}

void MirvPovScoreboard_SetEnabled(bool enabled)
{
    g_ScoreboardSyncEnabled = enabled;
    if(!enabled) SetScoreboardOpen(false);
}
