#include "stdafx.h"

#include "MirvPovTest.h"

#include "WrpConsole.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include "../deps/release/prop/cs2/sdk_src/public/icvar.h"
#include "../deps/release/prop/cs2/sdk_src/public/tier1/convar.h"

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

using IsDemoOrHltv_t = unsigned char (__fastcall *)();

constexpr char kIsDemoOrHltvPattern[] =
    "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 50 01 00 00 "
    "84 C0 75 ?? 38 05 ?? ?? ?? ?? 75 ?? 48 83 C4 28 C3 B0 01 "
    "48 83 C4 28 C3";

IsDemoOrHltv_t g_OrgIsDemoOrHltv = nullptr;
std::atomic_bool g_ForceIsDemoOrHltv = false;
bool g_IsDemoOrHltvHooked = false;

struct ClDemoPredictOverride {
    const char * name = "cl_demo_predict";
    SOURCESDK::CS2::ConVarHandle handle;
    alignas(SOURCESDK::CS2::CVValue_t) unsigned char previousValue[sizeof(SOURCESDK::CS2::CVValue_t)] = {};
    bool previousValueSaved = false;
};

ClDemoPredictOverride g_ClDemoPredictOverride;

SOURCESDK::CS2::Cvar_s * GetClDemoPredictCvar()
{
    if(nullptr == SOURCESDK::CS2::g_pCVar) return nullptr;

    if(!g_ClDemoPredictOverride.handle.IsValid()) {
        g_ClDemoPredictOverride.handle = SOURCESDK::CS2::g_pCVar->FindConVar(
            g_ClDemoPredictOverride.name,
            false
        );
    }

    if(!g_ClDemoPredictOverride.handle.IsValid()) return nullptr;
    return SOURCESDK::CS2::g_pCVar->GetCvar(g_ClDemoPredictOverride.handle.Get());
}

bool SetClDemoPredict(int value)
{
    SOURCESDK::CS2::Cvar_s * cvar = GetClDemoPredictCvar();
    if(nullptr == cvar) return false;

    if(!g_ClDemoPredictOverride.previousValueSaved) {
        std::memcpy(
            g_ClDemoPredictOverride.previousValue,
            &cvar->m_Value,
            sizeof(cvar->m_Value)
        );
        g_ClDemoPredictOverride.previousValueSaved = true;
    }

    SOURCESDK::CS2::CVValue_t oldValue = {};
    SOURCESDK::CS2::CVValue_t newValue = {};
    std::memcpy(&oldValue, &cvar->m_Value, sizeof(oldValue));
    std::memcpy(&newValue, &cvar->m_Value, sizeof(newValue));
    newValue.m_i32Value = value;

    if(oldValue.m_i32Value == value) return true;

    std::memcpy(&cvar->m_Value, &newValue, sizeof(newValue));
    SOURCESDK::CS2::g_pCVar->CallChangeCallback(
        g_ClDemoPredictOverride.handle,
        0,
        &newValue,
        &oldValue
    );
    return true;
}

bool RestoreClDemoPredict()
{
    if(!g_ClDemoPredictOverride.previousValueSaved) return true;

    SOURCESDK::CS2::Cvar_s * cvar = GetClDemoPredictCvar();
    if(nullptr == cvar) return false;

    SOURCESDK::CS2::CVValue_t oldValue = {};
    std::memcpy(&oldValue, &cvar->m_Value, sizeof(oldValue));

    SOURCESDK::CS2::CVValue_t * previousValue =
        reinterpret_cast<SOURCESDK::CS2::CVValue_t *>(
            g_ClDemoPredictOverride.previousValue
        );
    std::memcpy(&cvar->m_Value, previousValue, sizeof(cvar->m_Value));
    SOURCESDK::CS2::g_pCVar->CallChangeCallback(
        g_ClDemoPredictOverride.handle,
        0,
        previousValue,
        &oldValue
    );

    g_ClDemoPredictOverride.previousValueSaved = false;
    return true;
}

unsigned char __fastcall New_IsDemoOrHltv()
{
    if(g_ForceIsDemoOrHltv.load(std::memory_order_acquire)) return 0;
    return g_OrgIsDemoOrHltv();
}

bool ResolveIsDemoOrHltv(HMODULE clientDll, IsDemoOrHltv_t & target)
{
    if(nullptr == clientDll) {
        advancedfx::Warning("[mirv_pov_test] client.dll is not loaded.\n");
        return false;
    }

    Afx::BinUtils::ImageSectionsReader sections(clientDll);
    if(sections.Eof()) {
        advancedfx::Warning("[mirv_pov_test] client.dll text section was not found.\n");
        return false;
    }

    Afx::BinUtils::MemRange textRange = sections.GetMemRange();
    auto match = Afx::BinUtils::FindPatternString(textRange, kIsDemoOrHltvPattern);
    if(match.IsEmpty()) {
        advancedfx::Warning("[mirv_pov_test] IsDemoOrHltv pattern was not found.\n");
        return false;
    }

    auto remaining = Afx::BinUtils::MemRange(match.Start + 1, textRange.End);
    if(!Afx::BinUtils::FindPatternString(remaining, kIsDemoOrHltvPattern).IsEmpty()) {
        advancedfx::Warning("[mirv_pov_test] IsDemoOrHltv pattern is not unique.\n");
        return false;
    }

    target = reinterpret_cast<IsDemoOrHltv_t>(match.Start);
    return true;
}

} // namespace

void MirvPovTest_Initialize(HMODULE clientDll)
{
    if(g_IsDemoOrHltvHooked) return;

    IsDemoOrHltv_t target = nullptr;
    if(!ResolveIsDemoOrHltv(clientDll, target)) return;

    g_OrgIsDemoOrHltv = target;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID &)g_OrgIsDemoOrHltv, New_IsDemoOrHltv);
    if(NO_ERROR != DetourTransactionCommit()) {
        g_OrgIsDemoOrHltv = nullptr;
        advancedfx::Warning("[mirv_pov_test] IsDemoOrHltv detour failed.\n");
        return;
    }

    g_IsDemoOrHltvHooked = true;
    advancedfx::Message("[mirv_pov_test] IsDemoOrHltv hook installed.\n");
}

bool MirvPovTest_SetEnabled(bool enabled)
{
    if(!g_IsDemoOrHltvHooked) return false;

    if(enabled) {
        if(!SetClDemoPredict(2)) {
            advancedfx::Warning(
                "[mirv_pov_test] cl_demo_predict is unavailable; "
                "only IsDemoOrHltv is being overridden.\n"
            );
        }
    } else if(!RestoreClDemoPredict()) {
        advancedfx::Warning(
            "[mirv_pov_test] cl_demo_predict could not be restored.\n"
        );
    }

    g_ForceIsDemoOrHltv.store(enabled, std::memory_order_release);
    return true;
}

bool MirvPovTest_IsEnabled()
{
    return g_ForceIsDemoOrHltv.load(std::memory_order_acquire);
}

bool MirvPovTest_IsHooked()
{
    return g_IsDemoOrHltvHooked;
}

bool MirvPovTest_IsClDemoPredictOverridden()
{
    return g_ClDemoPredictOverride.previousValueSaved;
}

CON_COMMAND(mirv_pov_test, "Test POV data path: force IsDemoOrHltv() to 0 and cl_demo_predict to 2.")
{
    const char * arg0 = args->ArgV(0);

    if(!g_IsDemoOrHltvHooked) {
        MirvPovTest_Initialize(GetModuleHandleW(L"client.dll"));
    }

    if(2 <= args->ArgC()) {
        bool enabled = 0 != std::atoi(args->ArgV(1));
        if(!MirvPovTest_SetEnabled(enabled)) {
            advancedfx::Warning("%s: IsDemoOrHltv hook is unavailable.\n", arg0);
            return;
        }

        advancedfx::Message(
            "%s: %s (IsDemoOrHltv=0, cl_demo_predict=2 when available)\n",
            arg0,
            enabled ? "enabled" : "disabled"
        );
        return;
    }

    advancedfx::Message(
        "%s <0|1> - Test the POV-like data path. 1 forces IsDemoOrHltv() to 0 "
        "and cl_demo_predict to 2; 0 restores both.\n"
        "Current: %d, IsDemoOrHltv hook: %s, cl_demo_predict override: %s\n",
        arg0,
        MirvPovTest_IsEnabled() ? 1 : 0,
        MirvPovTest_IsHooked() ? "installed" : "unavailable",
        MirvPovTest_IsClDemoPredictOverridden() ? "active" : "inactive");
}
