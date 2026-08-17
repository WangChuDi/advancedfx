#include "stdafx.h"

#include "MirvPovPickupPrompt.h"

#include "ClientEntitySystem.h"
#include "MirvPovCore.h"
#include "MirvPovHookUtils.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#include <Windows.h>
#include <atomic>
#include <stdint.h>
#include <string.h>

namespace {

using GetLocalPawn_t = CEntityInstance * (__fastcall *)();
using BuildWeaponIdHint_t = __int64 (__fastcall *)(void *, void *, bool);
using BuildActiveWeaponIdHint_t = char (__fastcall *)(void *, void *);
using UpdatePickupTarget_t = int (__fastcall *)(CEntityInstance *);

constexpr size_t kCallInstructionSize = 5;
constexpr size_t kAbsoluteJumpThunkSize = 12;
constexpr size_t kThunkStride = 16;
constexpr size_t kThunkAllocationSize = 32;

struct CallPatch {
    uint8_t * Site = nullptr;
    uint8_t Original[kCallInstructionSize] = {};
    bool Installed = false;
};

GetLocalPawn_t g_GetNativeLocalPawn = nullptr;
BuildWeaponIdHint_t g_BuildNativeWeaponIdHint = nullptr;
BuildActiveWeaponIdHint_t g_BuildNativeActiveWeaponIdHint = nullptr;
UpdatePickupTarget_t g_UpdatePickupTarget = nullptr;
CallPatch g_LocalPawnCall;
CallPatch g_ActiveBuilderCall;
uint8_t * g_Thunks = nullptr;
std::atomic_bool g_Ready = false;
thread_local CEntityInstance * g_PickupHintPovPawn = nullptr;

bool Contains(const Afx::BinUtils::MemRange & range, const void * address, size_t size)
{
    const size_t start = reinterpret_cast<size_t>(address);
    return range.Start <= start && start <= range.End && size <= range.End - start;
}

Afx::BinUtils::MemRange FindUniquePattern(
    const Afx::BinUtils::MemRange & range,
    const char * pattern)
{
    Afx::BinUtils::MemRange first = Afx::BinUtils::FindPatternString(range, pattern);
    if(first.IsEmpty()) return Afx::BinUtils::MemRange::FromEmpty();

    Afx::BinUtils::MemRange second = Afx::BinUtils::FindPatternString(
        Afx::BinUtils::MemRange(first.End, range.End), pattern);
    return second.IsEmpty() ? first : Afx::BinUtils::MemRange::FromEmpty();
}

bool WriteCode(uint8_t * address, const uint8_t * bytes, size_t size)
{
    DWORD oldProtect;
    if(!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect)) return false;

    memcpy(address, bytes, size);
    const bool written = 0 == memcmp(address, bytes, size)
        && 0 != FlushInstructionCache(GetCurrentProcess(), address, size);
    DWORD dummy;
    if(0 == VirtualProtect(address, size, oldProtect, &dummy)) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Could not restore code page protection (error %lu).\n",
            GetLastError());
    }
    return written;
}

bool CodeMatches(const uint8_t * address, const uint8_t * bytes, size_t size)
{
    return 0 == memcmp(address, bytes, size);
}

bool RestoreCallPatch(CallPatch & patch)
{
    if(nullptr == patch.Site) return true;
    bool restored = CodeMatches(
        patch.Site,
        patch.Original,
        sizeof(patch.Original));
    if(!restored) {
        restored = WriteCode(
            patch.Site,
            patch.Original,
            sizeof(patch.Original));
    }
    patch.Installed = !restored;
    return restored;
}

void WriteAbsoluteJumpThunk(uint8_t * thunk, const void * destination)
{
    // mov rax, destination; jmp rax
    thunk[0] = 0x48;
    thunk[1] = 0xb8;
    const uint64_t address = reinterpret_cast<uint64_t>(destination);
    memcpy(thunk + 2, &address, sizeof(address));
    thunk[10] = 0xff;
    thunk[11] = 0xe0;
}

void MakeRelativeCall(uint8_t instruction[kCallInstructionSize], int32_t relative)
{
    instruction[0] = 0xe8;
    memcpy(instruction + 1, &relative, sizeof(relative));
}

bool IsHintActionEmpty(void * hint)
{
    if(nullptr == hint) return false;
    __try {
        return nullptr == *reinterpret_cast<void **>(
            reinterpret_cast<uint8_t *>(hint) + 0x10);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool TryUpdatePickupTarget(CEntityInstance *& povPawn, int & targetIndex)
{
    povPawn = nullptr;
    targetIndex = -1;
    if(nullptr == g_UpdatePickupTarget) return false;

    __try {
        povPawn = GetCurrentPovPlayerPawn();
        if(nullptr == povPawn || !povPawn->IsPlayerPawn()) return false;
        targetIndex = g_UpdatePickupTarget(povPawn);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        povPawn = nullptr;
        targetIndex = -1;
        return false;
    }
}

bool IsValidPickupTarget(int targetIndex)
{
    return 0 < targetIndex && targetIndex <= 0x7fff;
}

CEntityInstance * __fastcall GetPickupHintPawn()
{
    return nullptr != g_PickupHintPovPawn
        ? g_PickupHintPovPawn
        : (nullptr != g_GetNativeLocalPawn ? g_GetNativeLocalPawn() : nullptr);
}

char __fastcall BuildActiveWeaponIdHint(void * targetIdHud, void * hint)
{
    if(nullptr == g_BuildNativeActiveWeaponIdHint) return 0;
    if(!g_Ready.load(std::memory_order_acquire) || !MirvPov_IsEnabled()) {
        return g_BuildNativeActiveWeaponIdHint(targetIdHud, hint);
    }

    CEntityInstance * povPawn = nullptr;
    int targetIndex = -1;
    if(!TryUpdatePickupTarget(povPawn, targetIndex)) {
        return g_BuildNativeActiveWeaponIdHint(targetIdHud, hint);
    }

    CEntityInstance * previousPovPawn = g_PickupHintPovPawn;
    g_PickupHintPovPawn = povPawn;
    char result = 0;
    __try {
        result = g_BuildNativeActiveWeaponIdHint(targetIdHud, hint);
        // The active builder can return before its legacy pickup branch during
        // demo observer playback. Reuse its native pickup helper only when the
        // same hint state is still empty, matching the original call-site guard.
        if(IsValidPickupTarget(targetIndex)
            && nullptr != g_BuildNativeWeaponIdHint
            && IsHintActionEmpty(hint)) {
            g_BuildNativeWeaponIdHint(targetIdHud, hint, true);
        }
    } __finally {
        g_PickupHintPovPawn = previousPovPawn;
    }

    return result;
}

void ResetResolvedState()
{
    if(g_LocalPawnCall.Installed || g_ActiveBuilderCall.Installed) return;
    if(nullptr != g_Thunks) VirtualFree(g_Thunks, 0, MEM_RELEASE);

    g_GetNativeLocalPawn = nullptr;
    g_BuildNativeWeaponIdHint = nullptr;
    g_BuildNativeActiveWeaponIdHint = nullptr;
    g_UpdatePickupTarget = nullptr;
    g_LocalPawnCall = {};
    g_ActiveBuilderCall = {};
    g_Thunks = nullptr;
}

void RollBackFailedInstallation()
{
    const bool activeBuilderRestored = RestoreCallPatch(g_ActiveBuilderCall);
    const bool localPawnRestored = RestoreCallPatch(g_LocalPawnCall);
    if(!activeBuilderRestored || !localPawnRestored) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Rollback was incomplete; call thunks will remain allocated.\n");
    }
    ResetResolvedState();
}

} // namespace

void MirvPovPickupPrompt_Initialize(HMODULE clientDll)
{
    if(g_Ready.load(std::memory_order_acquire)) return;
    if(nullptr == clientDll) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] client.dll is not loaded.\n");
        return;
    }
    if(g_LocalPawnCall.Installed || g_ActiveBuilderCall.Installed) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] A partial hook is already installed.\n");
        return;
    }

    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    sections.Next(IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ);
    if(sections.Eof()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] client.dll executable section was not found.\n");
        return;
    }
    const Afx::BinUtils::MemRange textRange = sections.GetMemRange();

    const auto hintBuilder = FindUniquePattern(
        textRange,
        "40 53 55 56 48 83 EC 40 41 0F B6 D8 48 8B EA E8 ?? ?? ?? ?? "
        "48 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 48 8D 54 24 78 48 8B C8 "
        "E8 ?? ?? ?? ?? 8B 54 24 78 85 D2 0F 84 ?? ?? ?? ??");
    if(hintBuilder.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Native weapon-ID hint builder is missing or ambiguous.\n");
        return;
    }

    const auto activeHintBuilder = FindUniquePattern(
        textRange,
        "48 8B C4 53 55 57 41 54 41 55 41 56 48 81 EC 88 00 00 00 "
        "48 89 70 08 4C 8B EA 4C 89 78 C8 48 8B D9 E8 ?? ?? ?? ?? "
        "4C 8B E0 E8 ?? ?? ?? ?? 49 8B D4");
    if(activeHintBuilder.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Active weapon-ID hint builder is missing or ambiguous.\n");
        return;
    }

    const auto activeHintBuilderCaller = FindUniquePattern(
        textRange,
        "E8 ?? ?? ?? ?? 48 8D 54 24 ?? 49 8D 4E ??");
    if(activeHintBuilderCaller.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Active weapon-ID hint caller is missing or ambiguous.\n");
        return;
    }

    const auto updatePickupTarget = FindUniquePattern(
        textRange,
        "48 89 5C 24 ?? 55 56 57 48 8D AC 24 ?? ?? ?? ?? "
        "48 81 EC ?? ?? ?? ?? F2 0F 10 05 ?? ?? ?? ?? "
        "48 8B F9 48 8B 99 ?? ?? ?? ??");
    if(updatePickupTarget.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Native pickup-target trace is missing or ambiguous.\n");
        return;
    }

    uint8_t * hintBuilderAddress = reinterpret_cast<uint8_t *>(hintBuilder.Start);
    uint8_t * localPawnCallSite = hintBuilderAddress + 0x0f;
    uint8_t * activeHintBuilderAddress =
        reinterpret_cast<uint8_t *>(activeHintBuilder.Start);
    uint8_t * activeBuilderCallSite =
        reinterpret_cast<uint8_t *>(activeHintBuilderCaller.Start);
    if(!Contains(textRange, localPawnCallSite, 5)
        || !Contains(textRange, activeBuilderCallSite, 5)
        || 0xe8 != localPawnCallSite[0]
        || 0xe8 != activeBuilderCallSite[0]) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] Native call sites changed.\n");
        return;
    }

    int32_t nativeLocalPawnRelative = 0;
    memcpy(
        &nativeLocalPawnRelative,
        localPawnCallSite + 1,
        sizeof(nativeLocalPawnRelative));
    uint8_t * nativeLocalPawn =
        localPawnCallSite + 5 + nativeLocalPawnRelative;
    const uint8_t expectedLocalPawnPrefix[] = {0x40, 0x53, 0x48, 0x83, 0xec};
    if(!Contains(textRange, nativeLocalPawn, sizeof(expectedLocalPawnPrefix))
        || 0 != memcmp(
            nativeLocalPawn,
            expectedLocalPawnPrefix,
            sizeof(expectedLocalPawnPrefix))) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Native local-Pawn target validation failed.\n");
        return;
    }

    int32_t nativeActiveHintBuilderRelative = 0;
    memcpy(
        &nativeActiveHintBuilderRelative,
        activeBuilderCallSite + 1,
        sizeof(nativeActiveHintBuilderRelative));
    if(activeBuilderCallSite + 5 + nativeActiveHintBuilderRelative
        != activeHintBuilderAddress) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Active hint-builder target validation failed.\n");
        return;
    }

    uint8_t * thunks = MirvPovHookUtils::AllocateNear(
        activeBuilderCallSite,
        kThunkAllocationSize);
    if(nullptr == thunks) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] Could not allocate call thunks.\n");
        return;
    }

    WriteAbsoluteJumpThunk(thunks, &GetPickupHintPawn);
    uint8_t * activeBuilderThunk = thunks + kThunkStride;
    WriteAbsoluteJumpThunk(activeBuilderThunk, &BuildActiveWeaponIdHint);
    if(0 == FlushInstructionCache(
        GetCurrentProcess(),
        thunks,
        kThunkStride + kAbsoluteJumpThunkSize)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] Could not finalize call thunks.\n");
        VirtualFree(thunks, 0, MEM_RELEASE);
        return;
    }

    int32_t localPawnThunkRelative = 0;
    int32_t activeBuilderThunkRelative = 0;
    if(!MirvPovHookUtils::CalcRel32(
            localPawnCallSite + 5,
            thunks,
            localPawnThunkRelative)
        || !MirvPovHookUtils::CalcRel32(
            activeBuilderCallSite + 5,
            activeBuilderThunk,
            activeBuilderThunkRelative)) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] Call thunks are out of range.\n");
        VirtualFree(thunks, 0, MEM_RELEASE);
        return;
    }

    uint8_t localPawnReplacement[kCallInstructionSize] = {};
    MakeRelativeCall(localPawnReplacement, localPawnThunkRelative);
    uint8_t activeBuilderReplacement[kCallInstructionSize] = {};
    MakeRelativeCall(activeBuilderReplacement, activeBuilderThunkRelative);

    g_GetNativeLocalPawn = reinterpret_cast<GetLocalPawn_t>(nativeLocalPawn);
    g_BuildNativeWeaponIdHint =
        reinterpret_cast<BuildWeaponIdHint_t>(hintBuilderAddress);
    g_BuildNativeActiveWeaponIdHint =
        reinterpret_cast<BuildActiveWeaponIdHint_t>(activeHintBuilderAddress);
    g_UpdatePickupTarget =
        reinterpret_cast<UpdatePickupTarget_t>(updatePickupTarget.Start);
    g_LocalPawnCall.Site = localPawnCallSite;
    memcpy(
        g_LocalPawnCall.Original,
        localPawnCallSite,
        sizeof(g_LocalPawnCall.Original));
    g_ActiveBuilderCall.Site = activeBuilderCallSite;
    memcpy(
        g_ActiveBuilderCall.Original,
        activeBuilderCallSite,
        sizeof(g_ActiveBuilderCall.Original));
    g_Thunks = thunks;

    if(!WriteCode(
        localPawnCallSite,
        localPawnReplacement,
        sizeof(localPawnReplacement))) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_pickup_prompt] Could not patch the local-Pawn call.\n");
        RollBackFailedInstallation();
        return;
    }
    g_LocalPawnCall.Installed = true;

    if(!WriteCode(
        activeBuilderCallSite,
        activeBuilderReplacement,
        sizeof(activeBuilderReplacement))) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Could not patch the active hint-builder call.\n");
        RollBackFailedInstallation();
        return;
    }
    g_ActiveBuilderCall.Installed = true;
    g_Ready.store(true, std::memory_order_release);
}

void MirvPovPickupPrompt_RemovePatches()
{
    const bool activeBuilderRestored = RestoreCallPatch(g_ActiveBuilderCall);
    const bool localPawnRestored = RestoreCallPatch(g_LocalPawnCall);
    if(!activeBuilderRestored || !localPawnRestored) {
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_pickup_prompt] Could not restore all pickup prompt hooks.\n");
        return;
    }

    g_Ready.store(false, std::memory_order_release);
    ResetResolvedState();
}
