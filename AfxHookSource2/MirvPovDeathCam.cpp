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

// Verified client.dll 12e4a752 (2026-09-24): RVA 0xD0BC80. The
// signature deliberately starts at the function prologue; all operands in
// the stack allocation are wildcarded so the hook remains relocation-safe.
constexpr char kNativeDeathCamPattern[] =
    "40 55 53 56 57 41 56 48 8B EC 48 81 EC ?? ?? ?? ?? "
    "0F 29 74 24";

using NativeDeathCam_t = __int64 (__fastcall *)(void * This);

NativeDeathCam_t g_OrgNativeDeathCam = nullptr;
bool g_NativeDeathCamHooked = false;
std::atomic_bool g_DeathActive { false };
std::atomic_bool g_SawDeadSnapshot { false };
std::atomic_bool g_EventHeadshot { false };
std::atomic<float> g_EventDeathTime { -1.0f };
std::atomic<uint32_t> g_EventTargetHandle { 0xFFFFFFFFu };
std::atomic_int g_LastDemoTick { -1 };
std::atomic_int g_EventDemoTick { -1 };
thread_local bool g_InNativeDeathCam = false;
thread_local CEntityInstance * g_EffectPawn = nullptr;
void * g_EffectPawnReturnAddress = nullptr;
std::atomic_uint64_t g_PresentationEpoch { 1 };
// Accessed only by the native update thread; never dereference a saved object.
void * g_OwnedEffects = nullptr;
uint64_t g_OwnedEpoch = 0;
bool g_HadOwnedWeights = false;

void ClearOwnedWeights(void * effects)
{
    // The exact three stores are verified when resolving the native updater.
    auto weights = reinterpret_cast<float *>(static_cast<unsigned char *>(effects) + 0x120);
    weights[0] = weights[1] = weights[2] = 0.0f;
}

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

CEntityInstance * ResolveDeathPawn()
{
    const uint32_t handle = g_EventTargetHandle.load(std::memory_order_acquire);
    if(handle == 0xFFFFFFFFu) return nullptr;
    __try {
        auto pawn = GetEntityFromIndex(handle & 0x7FFFu);
        return pawn && pawn->IsPlayerPawn() && pawn->GetHandle().ToInt() == handle
            ? pawn : nullptr;
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
    if(g_InNativeDeathCam) return original(This);

    // Observer state can be incomplete inside the game's frame update. Keep
    // the validated death handle until the post-frame lifecycle check commits
    // a real target change; do not erase phase weights on a transient null.
    CEntityInstance * povPawn = ResolveDeathPawn();
    const uint32_t expectedHandle = g_EventTargetHandle.load(std::memory_order_acquire);
    const bool enabled = MIRV_POV_FEATURE_ACTIVE("deathcam")
        && MIRV_POV_FEATURE_ACTIVE("death_screen") && MirvPov_IsDeathFeedbackEnabled()
        && g_DeathActive.load(std::memory_order_acquire)
        && povPawn && expectedHandle != 0xFFFFFFFFu
        && SafeEntityHandle(povPawn) == expectedHandle
        && povPawn != GetRealLocalPlayerPawn();
    const uint64_t epoch = g_PresentationEpoch.load(std::memory_order_acquire);
    if(g_OwnedEffects != This) {
        g_OwnedEffects = This;
        g_HadOwnedWeights = false;
    }
    if((g_HadOwnedWeights && (!enabled || g_OwnedEpoch != epoch))
        || (enabled && !g_HadOwnedWeights)) {
        ClearOwnedWeights(This);
        g_HadOwnedWeights = false;
    }
    if(!enabled) return original(This);

    float deathTime = 0.0f;
    bool pawnHeadshot = false;
    const bool readSource = ReadDeathFields(povPawn, deathTime, pawnHeadshot);
    if(!readSource || !IsFiniteDeathTime(deathTime) || deathTime <= 0.0f)
        deathTime = g_EventDeathTime.load(std::memory_order_acquire);
    if(!IsFiniteDeathTime(deathTime)) return original(This);

    float previousDeathTime = 0.0f;
    uint8_t previousHeadshot = 0;
    if(!PatchDeathFields(povPawn, deathTime,
        g_EventHeadshot.load(std::memory_order_acquire), previousDeathTime, previousHeadshot))
        return original(This);

    // Supply the actual dead POV pawn at the selector's specific getter call.
    // The native selector retains its type/life-state checks, entity clock,
    // headshot branch, ConVars, low-violence variant and rendering behavior.
    g_InNativeDeathCam = true;
    g_EffectPawn = povPawn;
    g_OwnedEpoch = epoch;
    g_HadOwnedWeights = true;
    __int64 result = 0;
    __try {
        result = original(This);
    } __finally {
        g_EffectPawn = nullptr;
        g_InNativeDeathCam = false;
        RestoreDeathFields(povPawn, previousDeathTime, previousHeadshot);
    }
    return result;
}

bool ResolveNativeDeathCam(HMODULE clientDll, NativeDeathCam_t & target)
{
    if(nullptr == clientDll) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_deathcam] client.dll is not loaded.\n");
        return false;
    }

    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(sections.Eof()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_deathcam] client.dll code section was not found.\n");
        return false;
    }

    const Afx::BinUtils::MemRange textRange = sections.GetMemRange();
    auto match = Afx::BinUtils::FindPatternString(textRange, kNativeDeathCamPattern);
    if(match.IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_deathcam] native death-camera updater was not found.\n");
        return false;
    }

    auto remaining = Afx::BinUtils::MemRange(match.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(remaining, kNativeDeathCamPattern).IsEmpty()) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_deathcam] native death-camera signature is not unique.\n");
        return false;
    }

    if(!IsExecutableAddress(reinterpret_cast<void *>(match.Start))) {
        MIRV_POV_DIAGNOSTIC_WARNING("[mirv_pov_deathcam] native death-camera target is not executable.\n");
        return false;
    }

    // Fail closed if the known output layout or selector call has changed.
    const auto body = Afx::BinUtils::MemRange(match.Start, match.Start + 0x4D2);
    if(Afx::BinUtils::FindPatternString(body,
        "89 B7 20 01 00 00 BB 24 01 00 00").IsEmpty()
        || Afx::BinUtils::FindPatternString(body,
        "F3 0F 11 BF 28 01 00 00").IsEmpty()) return false;
    auto selector = Afx::BinUtils::FindPatternString(textRange,
        "40 53 48 83 EC 20 33 C9 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B 00 48 8B CB FF 90 F0 04 00 00 "
        "84 C0 74 ?? 48 8B 03 48 8B CB 48 89 7C 24 30 FF 90 C0 0A 00 00 33 FF 48 8B CB 84 C0 74 ?? E8 ?? ?? ?? ?? 83 F8 02");
    if(selector.IsEmpty() || !Afx::BinUtils::FindPatternString(
        Afx::BinUtils::MemRange(selector.Start + 1, textRange.End),
        "40 53 48 83 EC 20 33 C9 E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 74 ?? 48 8B 00 48 8B CB FF 90 F0 04 00 00 "
        "84 C0 74 ?? 48 8B 03 48 8B CB 48 89 7C 24 30 FF 90 C0 0A 00 00 33 FF 48 8B CB 84 C0 74 ?? E8 ?? ?? ?? ?? 83 F8 02").IsEmpty()) return false;
    // Ensure this is the selector called by this updater, not a similar helper.
    auto selectorCall = Afx::BinUtils::FindPatternString(body,
        "E8 ?? ?? ?? ?? 48 8B F0 48 85 C0 0F 84 ?? ?? ?? ?? 48 8B 0D");
    if(selectorCall.IsEmpty()
        || selectorCall.Start + 5 + *reinterpret_cast<int32_t *>(selectorCall.Start + 1)
            != selector.Start) return false;
    g_EffectPawnReturnAddress = reinterpret_cast<void *>(selector.Start + 13);

    target = reinterpret_cast<NativeDeathCam_t>(match.Start);
    return true;
}

} // namespace

void MirvPovDeathCam_Initialize(HMODULE clientDll)
{
    if(g_NativeDeathCamHooked) return;

    if(g_clientDllOffsets.C_BasePlayerPawn.m_flDeathTime < 0
        || g_clientDllOffsets.C_CSPlayerPawn.m_bKilledByHeadshot < 0) {
        MIRV_POV_DIAGNOSTIC_WARNING(
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
        MIRV_POV_DIAGNOSTIC_WARNING(
            "[mirv_pov_deathcam] native death-camera detour failed (attach=%ld commit=%ld).\n",
            attachResult,
            commitResult);
        return;
    }

    g_NativeDeathCamHooked = true;
}

void MirvPovDeathCam_HandleGameEvent(SOURCESDK::CS2::IGameEvent * event)
{
    if(nullptr == event || !MIRV_POV_FEATURE_ACTIVE("deathcam")) return;

    const char * name = nullptr;
    __try {
        name = event->GetName();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if(nullptr == name) return;

    if(0 == strcmp(name, "round_start")) {
        MirvPovDeathCam_Reset();
        return;
    }

    // spec_target_updated is a notification, not a new target identity, and
    // player_spawn may belong to any player. Check committed pawn/observer
    // state after FrameStageNotify instead of resetting from either event.

    if(0 != strcmp(name, "player_death")) return;

    bool headshot = false;
    if(!MirvPovFeedback_IsCurrentPovVictim(event, &headshot)) return;

    CEntityInstance * povPawn = SafeCurrentPovPawn();
    const uint32_t targetHandle = SafeEntityHandle(povPawn);
    if(nullptr == povPawn || 0xFFFFFFFFu == targetHandle) return;

    float eventDeathTime = g_MirvTime.curtime_get();
    if(!IsFiniteDeathTime(eventDeathTime)) eventDeathTime = -1.0f;
    int eventTick = -1;
    g_MirvTime.GetCurrentDemoTick(eventTick);

    g_PresentationEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_EventDemoTick.store(eventTick, std::memory_order_release);
    g_LastDemoTick.store(eventTick, std::memory_order_release);
    g_EventTargetHandle.store(targetHandle, std::memory_order_release);
    g_EventHeadshot.store(headshot, std::memory_order_release);
    g_EventDeathTime.store(eventDeathTime, std::memory_order_release);
    g_SawDeadSnapshot.store(false, std::memory_order_release);
    g_DeathActive.store(true, std::memory_order_release);
}

void MirvPovDeathCam_UpdateDemoTick(int demoTick)
{
    if(!MIRV_POV_FEATURE_ACTIVE("deathcam")) {
        g_LastDemoTick.store(-1, std::memory_order_release);
        return;
    }

    const int previous = g_LastDemoTick.exchange(demoTick, std::memory_order_acq_rel);
    if(previous >= 0 && demoTick >= 0 && demoTick < previous) {
        const int deathTick = g_EventDemoTick.load(std::memory_order_acquire);
        if(deathTick < 0 || demoTick < deathTick) {
            MirvPovDeathCam_Reset();
        } else {
            // Rewinding within this death keeps its identity but must release
            // max-accumulated phase weights before recomputing native time.
            MirvPovDeathCam_ResetPresentation();
        }
        g_LastDemoTick.store(demoTick, std::memory_order_release);
    }
    // Forward tick gaps are normal at low FPS / accelerated playback. A seek
    // forward is handled by the committed target and life-state checks too.
}

void MirvPovDeathCam_UpdateLifecycle()
{
    if(!g_DeathActive.load(std::memory_order_acquire)) return;
    __try {
        auto pawn = ResolveDeathPawn();
        auto selected = SafeCurrentPovPawn();
        const uint32_t expected = g_EventTargetHandle.load(std::memory_order_acquire);
        bool ended = !pawn;
        if(pawn) {
            if(pawn->GetHealth() == 0) g_SawDeadSnapshot.store(true, std::memory_order_release);
            else if(g_SawDeadSnapshot.load(std::memory_order_acquire)) ended = true;
        }
        if(selected && SafeEntityHandle(selected) != expected) ended = true;
        if(GetFakePovRadarAutoSync()) {
            uint8_t mode = 0;
            uint32_t target = 0xFFFFFFFFu;
            if(MirvPov_GetObserverState(mode, target)) {
                if(target != 0xFFFFFFFFu && target != expected) ended = true;
                if(mode == 6) ended = true; // committed roaming/free-camera mode
            }
        }
        if(ended) MirvPovDeathCam_Reset();
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        // An incomplete snapshot is not evidence that the death has ended.
    }
}

void MirvPovDeathCam_Reset()
{
    g_PresentationEpoch.fetch_add(1, std::memory_order_acq_rel);
    g_LastDemoTick.store(-1, std::memory_order_release);
    g_EventDemoTick.store(-1, std::memory_order_release);
    g_DeathActive.store(false, std::memory_order_release);
    g_SawDeadSnapshot.store(false, std::memory_order_release);
    g_EventTargetHandle.store(0xFFFFFFFFu, std::memory_order_release);
    g_EventHeadshot.store(false, std::memory_order_release);
    g_EventDeathTime.store(-1.0f, std::memory_order_release);
}

bool MirvPovDeathCam_IsHooked()
{
    return g_NativeDeathCamHooked;
}

void MirvPovDeathCam_ResetPresentation()
{
    g_PresentationEpoch.fetch_add(1, std::memory_order_acq_rel);
}

CEntityInstance * MirvPovDeathCam_GetEffectPawn(void * returnAddress)
{
    return g_InNativeDeathCam && returnAddress == g_EffectPawnReturnAddress
        ? g_EffectPawn : nullptr;
}
