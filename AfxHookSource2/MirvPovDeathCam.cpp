#include "stdafx.h"

#include "MirvPovDeathCam.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPovCore.h"
#include "MirvPovFeedback.h"
#include "MirvTime.h"
#include "SchemaSystem.h"
#include "../deps/release/Detours/src/detours.h"
#include "../shared/binutils.h"

#include "../deps/release/prop/cs2/sdk_src/public/igameevents.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

// Current client.dll (2026-08-04) entry point is sub_180CA4390. The
// signature deliberately starts at the function prologue; all operands in
// the stack allocation are wildcarded so the hook remains relocation-safe.
constexpr char kNativeDeathCamPattern[] =
    "40 55 53 56 57 41 56 48 8B EC 48 81 EC ?? ?? ?? ?? "
    "0F 29 74 24";

using NativeDeathCam_t = __int64 (__fastcall *)(void * This);

NativeDeathCam_t g_OrgNativeDeathCam = nullptr;
bool g_NativeDeathCamHooked = false;
std::atomic_bool g_DeathActive { false };
std::atomic_bool g_EventHeadshot { false };
std::atomic<float> g_EventDeathTime { -1.0f };
std::atomic<uint32_t> g_EventTargetHandle { 0xFFFFFFFFu };
std::atomic_int g_LastDemoTick { -1 };
thread_local bool g_InNativeDeathCam = false;

bool IsExecutableAddress(const void * address)
{
    if(nullptr == address) return false;

    MEMORY_BASIC_INFORMATION information = {};
    if(0 == VirtualQuery(address, &information, sizeof(information))) return false;
    if(MEM_COMMIT != information.State || 0 != (information.Protect & PAGE_GUARD)) return false;

    const DWORD protection = information.Protect & 0xff;
    return PAGE_EXECUTE == protection
        || PAGE_EXECUTE_READ == protection
        || PAGE_EXECUTE_READWRITE == protection
        || PAGE_EXECUTE_WRITECOPY == protection;
}

bool IsFiniteDeathTime(float value)
{
    return std::isfinite(value) && 0.0f <= value && value <= 10000000.0f;
}

uint32_t SafeEntityHandle(CEntityInstance * pawn)
{
    if(nullptr == pawn) return 0xFFFFFFFFu;

    __try {
        return pawn->GetHandle().ToInt();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFFu;
    }
}

CEntityInstance * SafeCurrentPovPawn()
{
    __try {
        return GetCurrentPovPlayerPawn();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ReadDeathFields(
    CEntityInstance * pawn,
    float & deathTime,
    bool & killedByHeadshot)
{
    if(nullptr == pawn
        || g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime < 0
        || g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot < 0) return false;

    __try {
        const auto base = reinterpret_cast<unsigned char *>(pawn);
        deathTime = *reinterpret_cast<float *>(
            base + g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime);
        killedByHeadshot = 0 != *reinterpret_cast<uint8_t *>(
            base + g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PatchDeathFields(
    CEntityInstance * pawn,
    float deathTime,
    bool killedByHeadshot,
    float & previousDeathTime,
    uint8_t & previousHeadshot)
{
    if(nullptr == pawn
        || !IsFiniteDeathTime(deathTime)
        || g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime < 0
        || g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot < 0) return false;

    __try {
        const auto base = reinterpret_cast<unsigned char *>(pawn);
        previousDeathTime = *reinterpret_cast<float *>(
            base + g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime);
        previousHeadshot = *reinterpret_cast<uint8_t *>(
            base + g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot);

        *reinterpret_cast<float *>(
            base + g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime) = deathTime;
        *reinterpret_cast<uint8_t *>(
            base + g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot) =
            killedByHeadshot ? 1 : 0;
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RestoreDeathFields(
    CEntityInstance * pawn,
    float previousDeathTime,
    uint8_t previousHeadshot)
{
    if(nullptr == pawn) return;

    __try {
        const auto base = reinterpret_cast<unsigned char *>(pawn);
        *reinterpret_cast<float *>(
            base + g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime) = previousDeathTime;
        *reinterpret_cast<uint8_t *>(
            base + g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot) = previousHeadshot;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
}

__int64 __fastcall New_NativeDeathCam(void * This)
{
    NativeDeathCam_t original = g_OrgNativeDeathCam;
    if(nullptr == original) return 0;

    if(g_InNativeDeathCam
        || !MirvPov_IsEnabled()
        || !g_DeathActive.load(std::memory_order_acquire)) {
        return original(This);
    }

    g_InNativeDeathCam = true;

    CEntityInstance * povPawn = SafeCurrentPovPawn();
    const uint32_t expectedHandle = g_EventTargetHandle.load(std::memory_order_acquire);
    const uint32_t povHandle = SafeEntityHandle(povPawn);
    if(nullptr == povPawn || 0xFFFFFFFFu == expectedHandle || povHandle != expectedHandle) {
        g_DeathActive.store(false, std::memory_order_release);
        g_InNativeDeathCam = false;
        return original(This);
    }

    CEntityInstance * nativePawn = GetRealLocalPlayerPawn();
    if(nullptr == nativePawn || nativePawn == povPawn) {
        g_InNativeDeathCam = false;
        return original(This);
    }

    float deathTime = 0.0f;
    bool pawnHeadshot = false;
    const bool readSource = ReadDeathFields(povPawn, deathTime, pawnHeadshot);
    if(!readSource || !IsFiniteDeathTime(deathTime) || deathTime <= 0.0f) {
        deathTime = g_EventDeathTime.load(std::memory_order_acquire);
    }
    if(!IsFiniteDeathTime(deathTime)) {
        g_InNativeDeathCam = false;
        return original(This);
    }

    // The event is the authoritative source for the headshot branch. The
    // Pawn field is still read above as a diagnostic/fallback for demos where
    // the event and entity update arrive on adjacent ticks.
    const bool headshot = g_EventHeadshot.load(std::memory_order_acquire);
    (void)pawnHeadshot;

    float previousDeathTime = 0.0f;
    uint8_t previousHeadshot = 0;
    if(!PatchDeathFields(
        nativePawn,
        deathTime,
        headshot,
        previousDeathTime,
        previousHeadshot)) {
        g_InNativeDeathCam = false;
        return original(This);
    }

    const __int64 result = original(This);
    RestoreDeathFields(nativePawn, previousDeathTime, previousHeadshot);
    g_InNativeDeathCam = false;
    return result;
}

bool ResolveNativeDeathCam(HMODULE clientDll, NativeDeathCam_t & target)
{
    if(nullptr == clientDll) {
        advancedfx::Warning("[mirv_pov_deathcam] client.dll is not loaded.\n");
        return false;
    }

    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(sections.Eof()) {
        advancedfx::Warning("[mirv_pov_deathcam] client.dll code section was not found.\n");
        return false;
    }

    const Afx::BinUtils::MemRange textRange = sections.GetMemRange();
    auto match = Afx::BinUtils::FindPatternString(textRange, kNativeDeathCamPattern);
    if(match.IsEmpty()) {
        advancedfx::Warning("[mirv_pov_deathcam] native death-camera updater was not found.\n");
        return false;
    }

    auto remaining = Afx::BinUtils::MemRange(match.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(remaining, kNativeDeathCamPattern).IsEmpty()) {
        advancedfx::Warning("[mirv_pov_deathcam] native death-camera signature is not unique.\n");
        return false;
    }

    if(!IsExecutableAddress(reinterpret_cast<void *>(match.Start))) {
        advancedfx::Warning("[mirv_pov_deathcam] native death-camera target is not executable.\n");
        return false;
    }

    target = reinterpret_cast<NativeDeathCam_t>(match.Start);
    return true;
}

} // namespace

void MirvPovDeathCam_Initialize(HMODULE clientDll)
{
    if(g_NativeDeathCamHooked) return;

    if(g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime < 0
        || g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot < 0) {
        advancedfx::Warning(
            "[mirv_pov_deathcam] required death Pawn schema fields are unavailable; "
            "native path disabled.\n");
        return;
    }

    NativeDeathCam_t target = nullptr;
    if(!ResolveNativeDeathCam(clientDll, target)) return;

    g_OrgNativeDeathCam = target;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG attachResult = DetourAttach(&(PVOID &)g_OrgNativeDeathCam, New_NativeDeathCam);
    LONG commitResult = DetourTransactionCommit();
    if(NO_ERROR != attachResult || NO_ERROR != commitResult) {
        g_OrgNativeDeathCam = nullptr;
        advancedfx::Warning(
            "[mirv_pov_deathcam] native death-camera detour failed (attach=%ld commit=%ld).\n",
            attachResult,
            commitResult);
        return;
    }

    g_NativeDeathCamHooked = true;
}

void MirvPovDeathCam_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MirvPov_IsEnabled()) return;

    const char * name = nullptr;
    __try {
        name = event->GetName();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if(nullptr == name) return;

    if(0 == strcmp(name, "round_start")
        || 0 == strcmp(name, "player_spawn")
        || 0 == strcmp(name, "spec_target_updated")) {
        MirvPovDeathCam_Reset();
        return;
    }

    if(0 != strcmp(name, "player_death")) return;

    bool headshot = false;
    if(!MirvPovFeedback_IsCurrentPovVictim(event, &headshot)) return;

    CEntityInstance * povPawn = SafeCurrentPovPawn();
    const uint32_t targetHandle = SafeEntityHandle(povPawn);
    if(nullptr == povPawn || 0xFFFFFFFFu == targetHandle) return;

    float eventDeathTime = g_MirvTime.curtime_get();
    if(!IsFiniteDeathTime(eventDeathTime)) eventDeathTime = -1.0f;

    g_EventTargetHandle.store(targetHandle, std::memory_order_release);
    g_EventHeadshot.store(headshot, std::memory_order_release);
    g_EventDeathTime.store(eventDeathTime, std::memory_order_release);
    g_DeathActive.store(true, std::memory_order_release);
}

void MirvPovDeathCam_UpdateDemoTick(int demoTick)
{
    if(!MirvPov_IsEnabled()) {
        g_LastDemoTick.store(-1, std::memory_order_release);
        return;
    }

    const int previous = g_LastDemoTick.exchange(demoTick, std::memory_order_acq_rel);
    if(previous >= 0) {
        const int delta = demoTick - previous;
        if(delta < 0 || delta > 2) {
            MirvPovDeathCam_Reset();
        }
    }
}

void MirvPovDeathCam_Reset()
{
    g_DeathActive.store(false, std::memory_order_release);
    g_EventTargetHandle.store(0xFFFFFFFFu, std::memory_order_release);
    g_EventHeadshot.store(false, std::memory_order_release);
    g_EventDeathTime.store(-1.0f, std::memory_order_release);
}

bool MirvPovDeathCam_IsHooked()
{
    return g_NativeDeathCamHooked;
}
