#include "stdafx.h"

#include "MirvPovRadar.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovCore.h"
#include "MirvPovHookUtils.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"
#include "../deps/release/Detours/src/detours.h"

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <intrin.h>

#include <limits.h>
#include <stdint.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

namespace {

enum RadarPlayerStyle : uint64_t {
    RadarPlayerStyleCt = 9,
    RadarPlayerStyleT = 13,
    RadarPlayerStyleEnemy = 17
};

struct RadarPatchState {
    const char * name;
    uint8_t * address = nullptr;
    uint8_t originalBytes[16] = {};
    uint8_t * trampoline = nullptr;
    size_t size = 0;
    bool applied = false;
};

RadarPatchState g_RadarEnemyColorPatch = {"enemy color"};
RadarPatchState g_RadarPovTeamVisibilityPatch = {"POV team visibility"};
RadarPatchState g_RadarCompetitiveColorPathPatch = {"competitive color path"};
RadarPatchState g_RadarTCompetitiveColorPatch = {"T competitive color"};
RadarPatchState g_RadarCtCompetitiveColorPatch = {"CT competitive color"};

using RadarRelation_t = char (__fastcall *)(CEntityInstance *, int);
using RadarPackageUpdate_t = __int64 (__fastcall *)(uint8_t *);
using RadarCompetitiveColorIndex_t = int (__fastcall *)(CEntityInstance *);
using RadarCompetitiveColor_t = uint32_t * (__fastcall *)(uint32_t *, int);
using RadarHudElement_t = uint8_t * (__fastcall *)(const char *);
using RadarPlayerSlot_t = int (__fastcall *)(void *, uint32_t);

bool g_RadarBackendApplied = false;

RadarRelation_t g_OrgRadarRelation = nullptr;
RadarPackageUpdate_t g_OrgRadarPackageUpdate = nullptr;
RadarCompetitiveColorIndex_t g_RadarCompetitiveColorIndex = nullptr;
RadarCompetitiveColor_t g_RadarCompetitiveColor = nullptr;
RadarHudElement_t g_RadarHudElement = nullptr;
RadarPlayerSlot_t g_RadarPlayerSlot = nullptr;
void * g_RadarRelationReturnAddress = nullptr;
bool g_RadarRelationHooked = false;
bool g_RadarPackageHooked = false;

struct RadarNativeTargets {
    uint8_t * relation = nullptr;
    uint8_t * relationReturn = nullptr;
    uint8_t * packageUpdate = nullptr;
    uint8_t * competitiveColorIndex = nullptr;
    uint8_t * competitiveColor = nullptr;
    uint8_t * hudElement = nullptr;
    uint8_t * playerSlot = nullptr;
};

constexpr size_t kRadarPackagePanelOffset = 0x238;
constexpr size_t kRadarPackageFlagsOffset = 0x17760;
constexpr size_t kRadarPackageCarrierHandleOffset = 0x17768;
constexpr uint32_t kRadarEnemyColor = 0xFF0000FF;
constexpr uint32_t kRadarUncarriedPackageColor = 0xFFFFFFFF;

constexpr uint8_t kPushRegisters[] = {
    0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57,
    0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
    0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57
};

constexpr uint8_t kPopRegisters[] = {
    0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C,
    0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
    0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58
};

bool ShouldApplyRadarOverrides()
{
    return MirvPov_IsEnabled();
}

uint64_t __fastcall AdjustRadarPlayerStyle(uint64_t style)
{
    if(!ShouldApplyRadarOverrides()) return style;

    __try {
        CEntityInstance * observedPawn = GetCurrentPovPlayerPawn();
        if(nullptr == observedPawn) return style;

        int observedTeam = observedPawn->GetTeam();
        if((3 == observedTeam && RadarPlayerStyleT == style)
            || (2 == observedTeam && RadarPlayerStyleCt == style)) {
            return RadarPlayerStyleEnemy;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }

    return style;
}

bool GetPovRadarTeam(int & team)
{
    if(!ShouldApplyRadarOverrides()) return false;

    CEntityInstance * povController = GetCurrentPovPlayerController();
    if(nullptr == povController || !povController->IsPlayerController()) return false;

    team = povController->GetTeam();
    return 2 == team || 3 == team;
}

bool __fastcall ShouldShowPovTeammate(CEntityInstance * targetPawn)
{
    if(!ShouldApplyRadarOverrides() || nullptr == targetPawn) return false;

    __try {
        CEntityInstance * povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn) return false;

        int povTeam = povPawn->GetTeam();
        int targetTeam = targetPawn->GetTeam();
        return (2 == povTeam || 3 == povTeam) && targetTeam == povTeam;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

CEntityInstance * ResolveRadarEntity(uint32_t handleValue)
{
    SOURCESDK::CS2::CBaseHandle handle(handleValue);
    if(!handle.IsValid()) return nullptr;
    return GetEntityFromIndex(handle.GetEntryIndex());
}

CEntityInstance * ResolveRadarPlayerController(CEntityInstance * entity)
{
    if(nullptr == entity) return nullptr;
    if(entity->IsPlayerController()) return entity;
    if(!entity->IsPlayerPawn()) return nullptr;

    auto controllerHandle = entity->GetPlayerControllerHandle();
    if(!controllerHandle.IsValid()) return nullptr;
    CEntityInstance * controller = GetEntityFromIndex(controllerHandle.GetEntryIndex());
    return nullptr != controller && controller->IsPlayerController() ? controller : nullptr;
}

bool GetRadarCompetitiveColor(CEntityInstance * entity, uint32_t & color)
{
    if(nullptr == entity
        || nullptr == g_RadarCompetitiveColorIndex
        || nullptr == g_RadarCompetitiveColor) return false;

    int colorIndex = g_RadarCompetitiveColorIndex(entity);
    if(colorIndex < 0) return false;
    g_RadarCompetitiveColor(&color, colorIndex);
    return true;
}

bool SetRadarPanelColor(void * holder, const uint32_t & color)
{
    if(nullptr == holder) return false;

    using GetPanel_t = void * (__fastcall *)(void *);
    using SetColor_t = void (__fastcall *)(void *, const uint32_t *);

    void * source = *reinterpret_cast<void **>(static_cast<uint8_t *>(holder) + 8);
    if(nullptr == source) return false;
    void ** sourceVtable = *reinterpret_cast<void ***>(source);
    if(nullptr == sourceVtable) return false;

    GetPanel_t getPanel = reinterpret_cast<GetPanel_t>(sourceVtable[0x230 / sizeof(void *)]);
    if(nullptr == getPanel) return false;
    void * panel = getPanel(source);
    if(nullptr == panel) return false;

    void ** panelVtable = *reinterpret_cast<void ***>(panel);
    if(nullptr == panelVtable) return false;
    SetColor_t setColor = reinterpret_cast<SetColor_t>(panelVtable[0x188 / sizeof(void *)]);
    if(nullptr == setColor) return false;
    setColor(panel, &color);
    return true;
}

bool GetNativeRadarPlayerSlot(uint32_t handle, int & slot)
{
    if(nullptr == g_RadarHudElement || nullptr == g_RadarPlayerSlot) return false;

    uint8_t * hudElement = g_RadarHudElement("CCSGO_HudTeamCounter");
    if(nullptr == hudElement) return false;

    slot = g_RadarPlayerSlot(hudElement - 0x20, handle);
    return true;
}

void ApplyPovRadarPackageColor(uint8_t * root)
{
    if(nullptr == root) return;

    __try {
        int povTeam = 0;
        if(!GetPovRadarTeam(povTeam)) return;

        uint8_t flags = *(root + kRadarPackageFlagsOffset);
        if(0 != (flags & 0x20)) return;

        uint32_t carrierHandle = *reinterpret_cast<uint32_t *>(
            root + kRadarPackageCarrierHandleOffset);

        int carrierSlot = -1;
        if(!GetNativeRadarPlayerSlot(carrierHandle, carrierSlot)) return;

        if(2 == povTeam && (0 != (flags & 0x10) || carrierSlot < 0)) {
            void * holder = *reinterpret_cast<void **>(root + kRadarPackagePanelOffset);
            SetRadarPanelColor(holder, kRadarUncarriedPackageColor);
            return;
        }

        if(carrierSlot < 0) return;

        CEntityInstance * carrier = ResolveRadarPlayerController(
            ResolveRadarEntity(carrierHandle));
        if(nullptr == carrier) return;

        int carrierTeam = carrier->GetTeam();
        if(2 != carrierTeam && 3 != carrierTeam) return;

        uint32_t color = kRadarEnemyColor;
        if(carrierTeam == povTeam && !GetRadarCompetitiveColor(carrier, color)) return;

        void * holder = *reinterpret_cast<void **>(root + kRadarPackagePanelOffset);
        SetRadarPanelColor(holder, color);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

void EmitU8(uint8_t * code, size_t & pos, uint8_t value)
{
    code[pos++] = value;
}

void EmitU32(uint8_t * code, size_t & pos, uint32_t value)
{
    memcpy(code + pos, &value, sizeof(value));
    pos += sizeof(value);
}

void EmitU64(uint8_t * code, size_t & pos, uint64_t value)
{
    memcpy(code + pos, &value, sizeof(value));
    pos += sizeof(value);
}

void EmitBytes(uint8_t * code, size_t & pos, const uint8_t * bytes, size_t count)
{
    memcpy(code + pos, bytes, count);
    pos += count;
}

bool EmitRel32Jump(uint8_t * code, size_t & pos, uint8_t * target)
{
    intptr_t relative = target - (code + pos + 5);
    if(relative < INT32_MIN || INT32_MAX < relative) return false;

    EmitU8(code, pos, 0xE9);
    EmitU32(code, pos, static_cast<uint32_t>(static_cast<int32_t>(relative)));
    return true;
}

bool EmitRel32Jcc(uint8_t * code, size_t & pos, uint8_t condition, uint8_t * target)
{
    intptr_t relative = target - (code + pos + 6);
    if(relative < INT32_MIN || INT32_MAX < relative) return false;

    EmitU8(code, pos, 0x0F);
    EmitU8(code, pos, condition);
    EmitU32(code, pos, static_cast<uint32_t>(static_cast<int32_t>(relative)));
    return true;
}

void EmitAbsoluteCall(uint8_t * code, size_t & pos, const void * target)
{
    EmitU8(code, pos, 0x48);
    EmitU8(code, pos, 0xB8); // mov rax, imm64
    EmitU64(code, pos, reinterpret_cast<uint64_t>(target));
    EmitU8(code, pos, 0xFF);
    EmitU8(code, pos, 0xD0); // call rax
}

void EmitAbsoluteIndirectCall(uint8_t * code, size_t & pos, const void * target)
{
    EmitU8(code, pos, 0xFF);
    EmitU8(code, pos, 0x15);
    EmitU32(code, pos, 2); // call qword ptr [rip+2]
    EmitU8(code, pos, 0xEB);
    EmitU8(code, pos, 8); // Skip the inline target after returning.
    EmitU64(code, pos, reinterpret_cast<uint64_t>(target));
}

void EmitSaveContext(uint8_t * code, size_t & pos, uint8_t stackAllocation)
{
    EmitBytes(code, pos, kPushRegisters, sizeof(kPushRegisters));

    EmitU8(code, pos, 0x48);
    EmitU8(code, pos, 0x81);
    EmitU8(code, pos, 0xEC); // sub rsp, stackAllocation
    EmitU32(code, pos, stackAllocation);

    for(uint8_t xmm = 0; xmm < 6; ++xmm) {
        EmitU8(code, pos, 0xF3);
        EmitU8(code, pos, 0x0F);
        EmitU8(code, pos, 0x7F); // movdqu [rsp+disp8], xmmN
        EmitU8(code, pos, static_cast<uint8_t>(0x44 + 8 * xmm));
        EmitU8(code, pos, 0x24);
        EmitU8(code, pos, static_cast<uint8_t>(0x20 + 0x10 * xmm));
    }
}

void EmitRestoreContext(uint8_t * code, size_t & pos, uint8_t stackAllocation)
{
    for(uint8_t xmm = 0; xmm < 6; ++xmm) {
        EmitU8(code, pos, 0xF3);
        EmitU8(code, pos, 0x0F);
        EmitU8(code, pos, 0x6F); // movdqu xmmN, [rsp+disp8]
        EmitU8(code, pos, static_cast<uint8_t>(0x44 + 8 * xmm));
        EmitU8(code, pos, 0x24);
        EmitU8(code, pos, static_cast<uint8_t>(0x20 + 0x10 * xmm));
    }

    EmitU8(code, pos, 0x48);
    EmitU8(code, pos, 0x81);
    EmitU8(code, pos, 0xC4); // add rsp, stackAllocation
    EmitU32(code, pos, stackAllocation);
}

bool ApplyPatch(
    RadarPatchState & state,
    uint8_t * address,
    size_t size,
    uint8_t * trampoline)
{
    if(state.applied) return true;
    if(nullptr == address || nullptr == trampoline || size < 5 || sizeof(state.originalBytes) < size) {
        return false;
    }

    intptr_t relative = trampoline - (address + 5);
    if(relative < INT32_MIN || INT32_MAX < relative) return false;

    DWORD oldProtect = 0;
    if(!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_radar] VirtualProtect failed for %s (error %lu).\n",
            state.name,
            GetLastError());
        return false;
    }

    memcpy(state.originalBytes, address, size);
    address[0] = 0xE9;
    *reinterpret_cast<int32_t *>(address + 1) = static_cast<int32_t>(relative);
    memset(address + 5, 0x90, size - 5);
    FlushInstructionCache(GetCurrentProcess(), address, size);

    DWORD unused = 0;
    if(!VirtualProtect(address, size, oldProtect, &unused)) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_radar] Failed to restore protection for %s (error %lu).\n",
            state.name,
            GetLastError());
    }

    state.address = address;
    state.trampoline = trampoline;
    state.size = size;
    state.applied = true;
    return true;
}

bool RestorePatch(RadarPatchState & state)
{
    if(!state.applied) return true;

    DWORD oldProtect = 0;
    if(!VirtualProtect(state.address, state.size, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_radar] Could not restore %s patch (error %lu).\n",
            state.name,
            GetLastError());
        return false;
    }

    memcpy(state.address, state.originalBytes, state.size);
    FlushInstructionCache(GetCurrentProcess(), state.address, state.size);

    DWORD unused = 0;
    if(!VirtualProtect(state.address, state.size, oldProtect, &unused)) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_radar] Failed to restore protection for %s (error %lu).\n",
            state.name,
            GetLastError());
    }

    VirtualFree(state.trampoline, 0, MEM_RELEASE);
    state.address = nullptr;
    state.trampoline = nullptr;
    state.size = 0;
    state.applied = false;
    return true;
}

bool PatchPovTeamVisibilityDecision(HMODULE clientDll)
{
    if(g_RadarPovTeamVisibilityPatch.applied) return true;

    size_t match = getAddress(
        clientDll,
        "38 5C 24 ?? 0F 84 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? F3 41 0F 10 8E ?? ?? ?? ?? F3 0F 10 41");
    if(0 == match) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] POV team visibility pattern not found.\n");
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 10;
    uint8_t * continueAddress = address + patchSize;
    int32_t originalBranch = 0;
    memcpy(&originalBranch, address + 6, sizeof(originalBranch));
    uint8_t * originalBranchTarget = continueAddress + originalBranch;

    uint8_t * trampoline = MirvPovHookUtils::AllocateNear(address, 512);
    if(nullptr == trampoline) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Could not allocate POV team visibility trampoline.\n");
        return false;
    }

    size_t pos = 0;
    EmitSaveContext(trampoline, pos, 0x88);
    EmitU8(trampoline, pos, 0x4C);
    EmitU8(trampoline, pos, 0x89);
    EmitU8(trampoline, pos, 0xF9); // mov rcx, r15 (target pawn)
    EmitAbsoluteCall(
        trampoline,
        pos,
        reinterpret_cast<const void *>(&ShouldShowPovTeammate));
    EmitRestoreContext(trampoline, pos, 0x88);
    EmitU8(trampoline, pos, 0x84);
    EmitU8(trampoline, pos, 0xC0); // test al, al
    EmitBytes(trampoline, pos, kPopRegisters, sizeof(kPopRegisters));

    bool emitted = EmitRel32Jcc(trampoline, pos, 0x85, continueAddress);
    EmitBytes(trampoline, pos, address, 4); // original cmp byte ptr [rsp+disp8], bl
    emitted = emitted && EmitRel32Jcc(trampoline, pos, 0x84, originalBranchTarget);
    emitted = emitted && EmitRel32Jump(trampoline, pos, continueAddress);
    if(!emitted || !ApplyPatch(
            g_RadarPovTeamVisibilityPatch,
            address,
            patchSize,
            trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool PatchCompetitiveColorPath(HMODULE clientDll)
{
    if(g_RadarCompetitiveColorPathPatch.applied) return true;

    size_t match = getAddress(
        clientDll,
        "4C 89 6C 24 ?? 84 DB 0F 84");
    if(0 == match) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Competitive color path pattern not found.\n");
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 5;
    uint8_t * trampoline = MirvPovHookUtils::AllocateNear(address, 64);
    if(nullptr == trampoline) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Could not allocate competitive color path trampoline.\n");
        return false;
    }

    size_t pos = 0;
    EmitBytes(trampoline, pos, address, patchSize); // mov [rsp+disp8], r13
    EmitU8(trampoline, pos, 0x31);
    EmitU8(trampoline, pos, 0xDB); // xor ebx, ebx

    bool emitted = EmitRel32Jump(trampoline, pos, address + patchSize);
    if(!emitted || !ApplyPatch(
        g_RadarCompetitiveColorPathPatch,
        address,
        patchSize,
        trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool PatchCompetitiveTeamColor(
    HMODULE clientDll,
    RadarPatchState & state,
    const char * pattern,
    uint32_t team)
{
    if(state.applied) return true;

    size_t match = getAddress(clientDll, pattern);
    if(0 == match) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] %s pattern not found.\n", state.name);
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 5;
    int32_t originalCall = *reinterpret_cast<int32_t *>(address + 1);
    uint8_t * originalCallTarget = address + patchSize + originalCall;
    uint8_t * trampoline = MirvPovHookUtils::AllocateNear(address, 64);
    if(nullptr == trampoline) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_radar] Could not allocate %s trampoline.\n",
            state.name);
        return false;
    }

    size_t pos = 0;
    EmitAbsoluteIndirectCall(trampoline, pos, originalCallTarget);
    EmitU8(trampoline, pos, 0xB8);
    EmitU32(trampoline, pos, team); // Override the original call's return value.

    bool emitted = EmitRel32Jump(trampoline, pos, address + patchSize);
    if(!emitted || !ApplyPatch(state, address, patchSize, trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool PatchEnemyColor(HMODULE clientDll)
{
    if(g_RadarEnemyColorPatch.applied) return true;

    size_t match = getAddress(
        clientDll,
        "48 8B 6C 24 ?? 41 39 9E ?? ?? ?? ?? 74 ?? 33 D2");
    if(0 == match) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Enemy color pattern not found.\n");
        return false;
    }

    uint8_t * address = reinterpret_cast<uint8_t *>(match);
    constexpr size_t patchSize = 12;
    uint8_t * trampoline = MirvPovHookUtils::AllocateNear(address, 512);
    if(nullptr == trampoline) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Could not allocate enemy color trampoline.\n");
        return false;
    }

    size_t pos = 0;
    EmitSaveContext(trampoline, pos, 0x88); // Mid-function RSP is 16-byte aligned.
    EmitU8(trampoline, pos, 0x48);
    EmitU8(trampoline, pos, 0x89);
    EmitU8(trampoline, pos, 0xD9); // mov rcx, rbx
    EmitAbsoluteCall(trampoline, pos, reinterpret_cast<const void *>(&AdjustRadarPlayerStyle));
    EmitRestoreContext(trampoline, pos, 0x88);
    EmitU8(trampoline, pos, 0x48);
    EmitU8(trampoline, pos, 0x89);
    EmitU8(trampoline, pos, 0x44);
    EmitU8(trampoline, pos, 0x24);
    EmitU8(trampoline, pos, 0x58); // mov [saved rbx], rax
    EmitBytes(trampoline, pos, kPopRegisters, sizeof(kPopRegisters));

    // Both overwritten instructions are position-independent. Copying them also
    // preserves the current stack and radar-entry displacements from the signature.
    EmitBytes(trampoline, pos, address, patchSize);
    bool emitted = EmitRel32Jump(trampoline, pos, address + patchSize);
    if(!emitted || !ApplyPatch(g_RadarEnemyColorPatch, address, patchSize, trampoline)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    return true;
}

bool GetClientTextRange(HMODULE clientDll, Afx::BinUtils::MemRange & textRange)
{
    textRange = Afx::BinUtils::MemRange::FromEmpty();
    if(nullptr == clientDll) return false;

    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(!sections.Eof()) textRange = sections.GetMemRange();
    if(textRange.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] client.dll text section was not found.\n");
        return false;
    }
    return true;
}

bool FindUniquePattern(
    const Afx::BinUtils::MemRange & textRange,
    const char * pattern,
    const char * description,
    uint8_t *& address)
{
    auto match = Afx::BinUtils::FindPatternString(textRange, pattern);
    if(match.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] %s pattern not found.\n", description);
        return false;
    }

    auto remaining = Afx::BinUtils::MemRange(match.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(remaining, pattern).IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] %s pattern is not unique.\n", description);
        return false;
    }

    address = reinterpret_cast<uint8_t *>(match.Start);
    return true;
}

bool ResolveCallTarget(
    uint8_t * callSite,
    const Afx::BinUtils::MemRange & textRange,
    uint8_t *& target)
{
    if(nullptr == callSite || 0xe8 != *callSite) return false;

    int32_t relative = 0;
    memcpy(&relative, callSite + 1, sizeof(relative));
    target = callSite + 5 + relative;
    return textRange.Start <= reinterpret_cast<size_t>(target)
        && reinterpret_cast<size_t>(target) < textRange.End;
}

bool ResolveRadarNativeTargets(HMODULE clientDll, RadarNativeTargets & targets)
{
    Afx::BinUtils::MemRange textRange = Afx::BinUtils::MemRange::FromEmpty();
    if(!GetClientTextRange(clientDll, textRange)) return false;

    if(!FindUniquePattern(
        textRange,
        "48 89 5C 24 ?? 48 89 74 24 ?? 55 57 41 54 41 56 41 57 48 8D 6C 24 ?? 48 81 EC ?? ?? ?? ?? 48 8B D9 45 33 FF",
        "radar bomb-package update",
        targets.packageUpdate)) return false;

    size_t packageSearchEnd = reinterpret_cast<size_t>(targets.packageUpdate) + 0x800;
    if(textRange.End < packageSearchEnd) packageSearchEnd = textRange.End;
    Afx::BinUtils::MemRange packageRange(
        reinterpret_cast<size_t>(targets.packageUpdate),
        packageSearchEnd);

    uint8_t * hudElementSequence = nullptr;
    if(!FindUniquePattern(
        packageRange,
        "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 44 0F 28 9C 24 ?? ?? ?? ?? 48 85 C0",
        "radar HUD element lookup",
        hudElementSequence)) return false;
    if(!ResolveCallTarget(hudElementSequence + 7, textRange, targets.hudElement)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Radar HUD element lookup call is invalid.\n");
        return false;
    }

    uint8_t * playerSlotSequence = nullptr;
    if(!FindUniquePattern(
        packageRange,
        "8B 93 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B CE C7 45 ?? FF 9B 25 FF",
        "radar player-slot resolver",
        playerSlotSequence)) return false;
    if(!ResolveCallTarget(playerSlotSequence + 6, textRange, targets.playerSlot)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Radar player-slot resolver call is invalid.\n");
        return false;
    }

    if(!FindUniquePattern(
        textRange,
        "40 53 48 83 EC ?? 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 85 C9 74 ?? E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 83",
        "radar competitive-color index",
        targets.competitiveColorIndex)) return false;

    if(!FindUniquePattern(
        textRange,
        "40 53 48 83 EC ?? 48 8B D9 83 FA ?? 7D",
        "radar competitive-color resolver",
        targets.competitiveColor)) return false;

    uint8_t * relationSequence = nullptr;
    if(!FindUniquePattern(
        textRange,
        "8B 54 24 28 48 8B 4C 24 38 E8 ?? ?? ?? ?? 88 85 90 00 00 00",
        "radar relation call",
        relationSequence)) return false;

    uint8_t * relationCall = relationSequence + 9;
    if(!ResolveCallTarget(relationCall, textRange, targets.relation)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Radar relation call is invalid.\n");
        return false;
    }
    targets.relationReturn = relationCall + 5;
    return true;
}

char __fastcall New_RadarRelation(CEntityInstance * localController, int entityIndex)
{
    char result = g_OrgRadarRelation(localController, entityIndex);
    if(_ReturnAddress() != g_RadarRelationReturnAddress || !ShouldApplyRadarOverrides()) {
        return result;
    }

    __try {
        int povTeam = 0;
        if(!GetPovRadarTeam(povTeam)) return result;

        CEntityInstance * target = ResolveRadarEntity(static_cast<uint32_t>(entityIndex));
        if(nullptr == target) return result;
        int targetTeam = target->GetTeam();
        if(2 != targetTeam && 3 != targetTeam) return result;
        return targetTeam == povTeam ? 0 : 1;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return result;
    }
}

__int64 __fastcall New_RadarPackageUpdate(uint8_t * root)
{
    __int64 result = g_OrgRadarPackageUpdate(root);
    ApplyPovRadarPackageColor(root);
    return result;
}

void ClearRadarNativeState()
{
    g_OrgRadarRelation = nullptr;
    g_OrgRadarPackageUpdate = nullptr;
    g_RadarCompetitiveColorIndex = nullptr;
    g_RadarCompetitiveColor = nullptr;
    g_RadarHudElement = nullptr;
    g_RadarPlayerSlot = nullptr;
    g_RadarRelationReturnAddress = nullptr;
}

bool AttachRadarDetours(const RadarNativeTargets & targets)
{
    g_OrgRadarRelation = reinterpret_cast<RadarRelation_t>(targets.relation);
    g_OrgRadarPackageUpdate = reinterpret_cast<RadarPackageUpdate_t>(targets.packageUpdate);
    g_RadarCompetitiveColorIndex = reinterpret_cast<RadarCompetitiveColorIndex_t>(
        targets.competitiveColorIndex);
    g_RadarCompetitiveColor = reinterpret_cast<RadarCompetitiveColor_t>(
        targets.competitiveColor);
    g_RadarHudElement = reinterpret_cast<RadarHudElement_t>(targets.hudElement);
    g_RadarPlayerSlot = reinterpret_cast<RadarPlayerSlot_t>(targets.playerSlot);
    g_RadarRelationReturnAddress = targets.relationReturn;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgRadarRelation, New_RadarRelation);
    DetourAttach(&(PVOID &)g_OrgRadarPackageUpdate, New_RadarPackageUpdate);
    if(NO_ERROR != DetourTransactionCommit()) {
        ClearRadarNativeState();
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Radar detours failed.\n");
        return false;
    }

    g_RadarRelationHooked = true;
    g_RadarPackageHooked = true;
    return true;
}

bool RemoveRadarDetours()
{
    if(!g_RadarRelationHooked && !g_RadarPackageHooked) return true;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    if(g_RadarRelationHooked) {
        DetourDetach(&(PVOID &)g_OrgRadarRelation, New_RadarRelation);
    }
    if(g_RadarPackageHooked) {
        DetourDetach(&(PVOID &)g_OrgRadarPackageUpdate, New_RadarPackageUpdate);
    }
    if(NO_ERROR != DetourTransactionCommit()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] Radar detour removal failed.\n");
        return false;
    }

    ClearRadarNativeState();
    g_RadarRelationHooked = false;
    g_RadarPackageHooked = false;
    return true;
}

bool ApplyRadarPresentationPatches(HMODULE clientDll)
{
    bool result = true;
    result = PatchPovTeamVisibilityDecision(clientDll) && result;
    result = PatchCompetitiveColorPath(clientDll) && result;
    result = PatchCompetitiveTeamColor(
        clientDll,
        g_RadarTCompetitiveColorPatch,
        "E8 ?? ?? ?? ?? 41 3B C5 0F 85 ?? ?? ?? ?? F6 86",
        2) && result;
    result = PatchCompetitiveTeamColor(
        clientDll,
        g_RadarCtCompetitiveColorPatch,
        "E8 ?? ?? ?? ?? 83 F8 03 75 ?? 8B D3",
        3) && result;
    result = PatchEnemyColor(clientDll) && result;
    return result;
}

bool RemoveRadarPresentationPatches()
{
    bool result = true;
    result = RestorePatch(g_RadarEnemyColorPatch) && result;
    result = RestorePatch(g_RadarCtCompetitiveColorPatch) && result;
    result = RestorePatch(g_RadarTCompetitiveColorPatch) && result;
    result = RestorePatch(g_RadarCompetitiveColorPathPatch) && result;
    result = RestorePatch(g_RadarPovTeamVisibilityPatch) && result;
    return result;
}

bool ApplyRadarBackend(HMODULE clientDll)
{
    RadarNativeTargets targets;
    if(!ResolveRadarNativeTargets(clientDll, targets)) return false;

    bool result = ApplyRadarPresentationPatches(clientDll);
    result = result && AttachRadarDetours(targets);
    if(!result) {
        RemoveRadarDetours();
        RemoveRadarPresentationPatches();
    }
    return result;
}

bool RemoveRadarBackend()
{
    if(!RemoveRadarDetours()) return false;

    return RemoveRadarPresentationPatches();
}

} // namespace

void MirvPov_ApplyRadarPatches(HMODULE clientDll)
{
    if(nullptr == clientDll) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_radar] client.dll is not loaded.\n");
        return;
    }

    if(g_RadarBackendApplied) return;
    g_RadarBackendApplied = ApplyRadarBackend(clientDll);
}

void MirvPov_RemoveRadarPatches()
{
    if(g_RadarBackendApplied && RemoveRadarBackend()) {
        g_RadarBackendApplied = false;
    }
}
