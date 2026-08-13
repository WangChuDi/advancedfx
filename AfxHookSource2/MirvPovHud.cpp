#include "stdafx.h"

#include "MirvPovHud.h"
#include "MirvPovCore.h"

#include "ClientEntitySystem.h"
#include "Globals.h"
#include "MirvPanorama.h"

#include "../shared/AfxConsole.h"
#include "../shared/binutils.h"

#define WIN32_LEAN_AND_MEAN
#include "../deps/release/Detours/src/detours.h"

#include <stdint.h>
#include <intrin.h>
#include <string.h>

#pragma intrinsic(_ReturnAddress)

void MirvPovHud_OnPanoramaDllLoaded(HMODULE panoramaDll)
{
    // The Panorama hook is installed by DeathMsg. Keep this callback as a
    // lifecycle notification only; native style resolution is intentionally
    // deferred until a POV feature actually needs it.
    (void)panoramaDll;
}

static unsigned char* MirvPovHud_FindPanelByIdRecursive(unsigned char* parentPanel, const char* panelId) {
    if(!parentPanel) return nullptr;
    __try {
        const auto currentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
        if(currentPanelId && 0 == strcmp(currentPanelId, panelId)) return parentPanel;

        const auto children = parentPanel + CS2::PanoramaUIPanel::children;
        const auto childCount = *(int*)children;
        if(childCount < 0 || childCount > 4096) return nullptr;
        for(int i = 0; i < childCount; ++i) {
            if(auto panel = MirvPovHud_FindPanelByIdRecursive(((unsigned char***)children)[1][i], panelId)) return panel;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return nullptr;
}

static unsigned char* MirvPovHud_FindPanelById(unsigned char* parentPanel, const char* panelId) {
    if(!parentPanel || !panelId) return nullptr;

    __try {
        const auto currentPanelId = *(char**)(parentPanel + CS2::PanoramaUIPanel::panelId);
        if(currentPanelId && 0 == strcmp(currentPanelId, panelId)) return parentPanel;

        typedef unsigned char* (__fastcall * FindChildTraverse_t)(unsigned char*, const char*);
        auto vtable = *(void***)parentPanel;
        auto findChildTraverse = vtable ? (FindChildTraverse_t)vtable[47] : nullptr;
        if(findChildTraverse) {
            if(auto panel = findChildTraverse(parentPanel, panelId)) return panel;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    return MirvPovHud_FindPanelByIdRecursive(parentPanel, panelId);
}

static bool MirvPovHud_PanelContainsId(unsigned char* parentPanel, const char* panelId) {
    return nullptr != MirvPovHud_FindPanelById(parentPanel, panelId);
}

static bool MirvPovHud_SetStrokeSiblingVisibleForAnchor(unsigned char* parentPanel, const char* anchorId) {
    if(!parentPanel) return false;
    __try {
        const auto children = parentPanel + CS2::PanoramaUIPanel::children;
        const auto childCount = *(int*)children;
        if(childCount < 0 || childCount > 4096) return false;

        if(2 == childCount) {
            int anchorChild = -1;
            for(int i = 0; i < childCount; ++i) {
                if(MirvPovHud_PanelContainsId(((unsigned char***)children)[1][i], anchorId)) {
                    anchorChild = i;
                    break;
                }
            }

            if(-1 != anchorChild) {
                const auto strokePanel = ((unsigned char***)children)[1][1 - anchorChild];
                if(Panorama_SetPanelVisible(strokePanel, true)) return true;
            }
        }

        for(int i = 0; i < childCount; ++i) {
            if(MirvPovHud_SetStrokeSiblingVisibleForAnchor(((unsigned char***)children)[1][i], anchorId)) return true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

static void MirvPovHud_ShowHealthAmmoCenterStrokes() {
    if(!CS2::PanoramaUIPanel::hudPanel) return;

    auto hudPanel = ((unsigned char***)CS2::PanoramaUIPanel::hudPanel)[0][1];
    if(!hudPanel) return;

    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-HA-main");
    MirvPovHud_SetStrokeSiblingVisibleForAnchor(hudPanel, "hud-WPN-main");
}

static void MirvPovHud_HideSpecPlayerPanel() {
    if(!CS2::PanoramaUIPanel::hudPanel) return;

    auto hudPanel = ((unsigned char***)CS2::PanoramaUIPanel::hudPanel)[0][1];
    if(!hudPanel) return;

    auto specPlayerBg = MirvPovHud_FindPanelById(hudPanel, "jsHudSpecplayer__Bg");
    if(specPlayerBg) Panorama_SetPanelVisible(specPlayerBg, false);

    auto specPlayerAvatar = MirvPovHud_FindPanelById(hudPanel, "HudSpecplayer__Avatar");
    if(specPlayerAvatar) Panorama_SetPanelVisible(specPlayerAvatar, false);
}

static unsigned char* g_SpectatorHotKeyLabelContainerPanel = nullptr;
static unsigned char* g_SpectatorHotKeyLabelContainerRoot = nullptr;

void MirvPovHud_InvalidatePanelCache() {
    g_SpectatorHotKeyLabelContainerPanel = nullptr;
    g_SpectatorHotKeyLabelContainerRoot = nullptr;
}

static void MirvPovHud_SetSpectatorHotKeyLabelsVisible(bool visible) {
    if(!CS2::PanoramaUIPanel::hudPanel) return;

    __try {
        auto hudPanel = ((unsigned char***)CS2::PanoramaUIPanel::hudPanel)[0][1];
        if(!hudPanel) {
            MirvPovHud_InvalidatePanelCache();
            return;
        }

        if(g_SpectatorHotKeyLabelContainerRoot != hudPanel) {
            MirvPovHud_InvalidatePanelCache();
            g_SpectatorHotKeyLabelContainerRoot = hudPanel;
        }

        if(!g_SpectatorHotKeyLabelContainerPanel) {
            g_SpectatorHotKeyLabelContainerPanel = MirvPovHud_FindPanelById(
                hudPanel,
                "HotKeyLabelContainer");
        }

        // Reuse the native DemoUI state that hudlegend.css already handles:
        // .DemoControllerFull .HudSpecplayer__key-hints-text { visibility: collapse; }
        if(g_SpectatorHotKeyLabelContainerPanel) {
            if(!Panorama_SetPanelClass(
                g_SpectatorHotKeyLabelContainerPanel,
                "DemoControllerFull",
                !visible)) {
                MirvPovHud_InvalidatePanelCache();
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        MirvPovHud_InvalidatePanelCache();
    }
}

void MirvPovHud_OnPanoramaLayoutFileLoaded(const char* filePath) {
    // Panorama is hooked globally for other Afx features, but these panel
    // tree mutations belong exclusively to mirv_pov. Ordinary spectating and
    // HUD rebuilds must stay on the native path.
    if(nullptr == filePath || !MirvPov_IsEnabled()) return;
    if(0 == strcmp("panorama\\layout\\hud\\hudhealthammocenter.xml", filePath)) {
        MirvPovHud_HideSpecPlayerPanel();
        MirvPovHud_ShowHealthAmmoCenterStrokes();
    } else if(0 == strcmp("panorama\\layout\\hud\\hudlegend.xml", filePath)) {
        MirvPovHud_InvalidatePanelCache();
        MirvPovHud_SetSpectatorHotKeyLabelsVisible(!MirvPov_IsEnabled());
    }
}

static int g_IsLocalPlayerHLTV_SuppressFrames = 0;
static int g_IsLocalPlayerHLTV_LastDemoTick = -1;

void MirvPovHud_UpdateSeekDetection(int curTick) {
    bool seek = false;
    if(g_IsLocalPlayerHLTV_LastDemoTick >= 0) {
        int delta = curTick - g_IsLocalPlayerHLTV_LastDemoTick;
        if(delta < 0) delta = -delta;
        if(delta > 2) {
            seek = true;
            g_IsLocalPlayerHLTV_SuppressFrames = 16;
        }
    }
    g_IsLocalPlayerHLTV_LastDemoTick = curTick;
    if(seek) MirvPovHud_InvalidatePanelCache();
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);
    if(g_IsLocalPlayerHLTV_SuppressFrames > 0) {
        g_IsLocalPlayerHLTV_SuppressFrames--;
    }
}

bool MirvPovHud_ShouldSuppressFrame() {
    return g_IsLocalPlayerHLTV_SuppressFrames > 0;
}

// Hook GameStateAPI::IsLocalPlayerHLTV (sub_180EFF830) - Panorama bridge callback.
// The radar JS calls this to decide spectator vs player color mode.
// Return original behavior on the stable baseline.
typedef bool (__fastcall * IsLocalPlayerHLTV_t)();
static IsLocalPlayerHLTV_t g_Org_IsLocalPlayerHLTV = nullptr;
static bool g_bIsLocalPlayerHLTVHooked = false;

static bool __fastcall New_IsLocalPlayerHLTV() {
    return g_Org_IsLocalPlayerHLTV();
}

// Hook GameStateAPI::IsDemoOrHltv (sub_180EFEEE0) - Panorama bridge callback.
// Stable baseline keeps original demo/HLTV behavior.
typedef bool (__fastcall * IsDemoOrHltv_t)();
static IsDemoOrHltv_t g_Org_IsDemoOrHltv = nullptr;
static bool g_bIsDemoOrHltvHooked = false;

static bool __fastcall New_IsDemoOrHltv() {
    return g_Org_IsDemoOrHltv();
}

// Hook sub_180BD7830 (GetEffectiveLocalPlayer for HUD) - this function is
// used by the HUD to determine spectator state. It calls sub_1808E0E70(0)
// directly, bypassing our GetLocalPlayerController hook.
// Instead of hooking the function (which crashes during demo transitions),
// keep the spectator CSS state intact for xray/head markers.
static uint8_t * g_pHudSpectatorCheckPatchAddr = nullptr;
static uint8_t g_HudSpectatorCheckOrigByte = 0;
static bool g_bHudSpectatorCheckPatched = false;

typedef bool (__fastcall * FlashViewPredicate_t)();
static FlashViewPredicate_t g_OrgFlashViewPredicate = nullptr;
static bool g_bFlashViewPredicateHooked = false;
static void * g_FlashViewPredicateReturnAddresses[2] = {};
static bool g_FlashHooksActive = false;

static uint8_t * MirvPovHud_GetRelativeCallTarget(uint8_t * callSite) {
    if(nullptr == callSite || 0xE8 != callSite[0]) return nullptr;
    int32_t relative = *reinterpret_cast<int32_t *>(callSite + 1);
    return callSite + 5 + relative;
}

static bool __fastcall New_FlashViewPredicate() {
    void * previousReturnAddress = MirvPov_PushHookReturnAddress(_ReturnAddress());
    void * returnAddress = MirvPov_GetHookReturnAddress();
    bool result = g_OrgFlashViewPredicate();
    MirvPov_PopHookReturnAddress(previousReturnAddress);

    if(!g_FlashHooksActive || !MirvPov_IsEnabled()) return result;
    if(returnAddress == g_FlashViewPredicateReturnAddresses[0]
        || returnAddress == g_FlashViewPredicateReturnAddresses[1]) return false;
    return result;
}

static bool MirvPovHud_ResolveFlashContexts(HMODULE clientDll) {
    const size_t compactPathMatch = getAddress(
        clientDll,
        "48 8B F2 48 8B E9 E8 ?? ?? ?? ?? 84 C0 0F 85");
    const size_t perViewPathMatch = getAddress(
        clientDll,
        "84 C0 74 4C 8B 85 B0 02 00 00 49 8D 8D 48 03 00 00");
    if(0 == compactPathMatch || 0 == perViewPathMatch) {
        advancedfx::Warning("[mirv_pov_flash] Flash render contexts were not found.\n");
        return false;
    }

    uint8_t * compactPredicateCall = reinterpret_cast<uint8_t *>(compactPathMatch) + 6;
    uint8_t * perViewPredicateCall = reinterpret_cast<uint8_t *>(perViewPathMatch) - 5;
    uint8_t * compactPredicateTarget = MirvPovHud_GetRelativeCallTarget(compactPredicateCall);
    uint8_t * perViewPredicateTarget = MirvPovHud_GetRelativeCallTarget(perViewPredicateCall);
    if(nullptr == compactPredicateTarget || compactPredicateTarget != perViewPredicateTarget) {
        advancedfx::Warning("[mirv_pov_flash] Flash view predicate validation failed.\n");
        return false;
    }

    g_OrgFlashViewPredicate = reinterpret_cast<FlashViewPredicate_t>(compactPredicateTarget);
    g_FlashViewPredicateReturnAddresses[0] = compactPredicateCall + 5;
    g_FlashViewPredicateReturnAddresses[1] = perViewPredicateCall + 5;
    return true;
}

static bool MirvPovHud_InstallFlashPredicateHook(HMODULE clientDll) {
    if(g_bFlashViewPredicateHooked) return true;
    if(!MirvPovHud_ResolveFlashContexts(clientDll)) return false;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    LONG attachResult = DetourAttach(&(PVOID &)g_OrgFlashViewPredicate, New_FlashViewPredicate);
    LONG commitResult = NO_ERROR == attachResult
        ? DetourTransactionCommit()
        : DetourTransactionAbort();
    if(NO_ERROR != attachResult || NO_ERROR != commitResult) {
        g_OrgFlashViewPredicate = nullptr;
        advancedfx::Warning(
            "[mirv_pov_flash] Flash view predicate Detour failed attach=%ld commit=%ld.\n",
            attachResult,
            commitResult);
        return false;
    }

    g_bFlashViewPredicateHooked = true;
    return true;
}

static void MirvPovHud_RemoveFlashPredicateHook() {
    if(!g_bFlashViewPredicateHooked || nullptr == g_OrgFlashViewPredicate) return;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID &)g_OrgFlashViewPredicate, New_FlashViewPredicate);
    if(NO_ERROR != DetourTransactionCommit()) {
        advancedfx::Warning("[mirv_pov_flash] Flash view predicate Detour removal failed.\n");
        return;
    }

    g_bFlashViewPredicateHooked = false;
    g_OrgFlashViewPredicate = nullptr;
}

void MirvPovHud_ApplyPatches(HMODULE clientDll) {
    if(nullptr == clientDll) {
        advancedfx::Warning("[mirv_pov_radar_patch] No client.dll handle\n");
        return;
    }

    // The game owns the flash/fade lifecycle. A global predicate hook here
    // suppresses the native hurt red flash and death red-to-black fade, so it
    // must remain disabled for the native feedback path.
    g_FlashHooksActive = false;

    // --- Hook IsLocalPlayerHLTV (Panorama GameStateAPI callback) ---
    // DISABLED: interferes with xray / head markers in demo POV. Kept code for reference.
    if(false) {
        size_t funcAddr = getAddress(clientDll, "48 83 EC ?? 33 C9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 80 B8");
        if(0 == funcAddr) {
            advancedfx::Warning("[mirv_pov_radar_patch] IsLocalPlayerHLTV pattern not found\n");
        } else {
            g_Org_IsLocalPlayerHLTV = (IsLocalPlayerHLTV_t)funcAddr;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_Org_IsLocalPlayerHLTV, New_IsLocalPlayerHLTV);
            if(NO_ERROR == DetourTransactionCommit()) {
                g_bIsLocalPlayerHLTVHooked = true;
            } else {
                advancedfx::Warning("[mirv_pov_radar_patch] IsLocalPlayerHLTV detour failed\n");
                g_Org_IsLocalPlayerHLTV = nullptr;
            }
        }
    }

    // --- IsDemoOrHltv hook: DISABLED (interferes with xray / head markers in demo POV) ---
    if(false) {
        size_t funcAddr = 0;
        unsigned char * base = (unsigned char *)clientDll;
        IMAGE_DOS_HEADER * dosHeader = (IMAGE_DOS_HEADER *)base;
        IMAGE_NT_HEADERS * ntHeaders = (IMAGE_NT_HEADERS *)(base + dosHeader->e_lfanew);
        size_t size = ntHeaders->OptionalHeader.SizeOfImage;

        const char * searchStr = "IsDemoOrHltv";
        size_t searchLen = strlen(searchStr);
        size_t strAddr = 0;

        for(size_t i = 0; i + searchLen < size; i++) {
            if(0 == memcmp(base + i, searchStr, searchLen + 1)) {
                strAddr = (size_t)(base + i);
                break;
            }
        }

        if(strAddr) {
            // Find LEA instruction referencing this string (RIP-relative: REX.W 8D ModRM[rm=5] disp32)
            for(size_t i = 0; i + 7 < size; i++) {
                unsigned char * p = base + i;
                if((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0x07) == 0x05) {
                    int32_t disp = *(int32_t *)(p + 3);
                    size_t target = (size_t)(p + 7) + disp;
                    if(target == strAddr) {
                        // Found LEA loading "IsDemoOrHltv". Scan nearby for another LEA (function ptr).
                        for(int delta = -64; delta <= 64; delta++) {
                            if(delta >= -3 && delta <= 6) continue;
                            unsigned char * q = p + delta;
                            if(q < base || q + 7 >= base + size) continue;
                            if((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8D && (q[2] & 0x07) == 0x05) {
                                int32_t disp2 = *(int32_t *)(q + 3);
                                size_t candidate = (size_t)(q + 7) + disp2;
                                if(candidate >= (size_t)base && candidate < (size_t)base + size) {
                                    unsigned char * cand = (unsigned char *)candidate;
                                    // Heuristic: looks like function prologue
                                    if(cand[0] == 0x48 || cand[0] == 0x40 || cand[0] == 0x55 ||
                                       cand[0] == 0x53 || cand[0] == 0x56 || cand[0] == 0x41 ||
                                       cand[0] == 0xB0 || (cand[0] == 0x33 && cand[1] == 0xC0) ||
                                       cand[0] == 0x8B) {
                                        funcAddr = candidate;
                                        break;
                                    }
                                }
                            }
                        }
                        if(funcAddr) break;
                    }
                }
            }
        } else {
            advancedfx::Warning("[mirv_pov_radar_patch] IsDemoOrHltv string not found in client.dll\n");
        }

        if(0 == funcAddr) {
            advancedfx::Warning("[mirv_pov_radar_patch] IsDemoOrHltv function not found\n");
            g_bIsDemoOrHltvHooked = true;
        } else {
            g_Org_IsDemoOrHltv = (IsDemoOrHltv_t)funcAddr;
            DetourTransactionBegin();
            DetourUpdateThread(GetCurrentThread());
            DetourAttach(&(PVOID&)g_Org_IsDemoOrHltv, New_IsDemoOrHltv);
            if(NO_ERROR == DetourTransactionCommit()) {
                g_bIsDemoOrHltvHooked = true;
            } else {
                advancedfx::Warning("[mirv_pov_radar_patch] IsDemoOrHltv detour failed\n");
                g_Org_IsDemoOrHltv = nullptr;
                g_bIsDemoOrHltvHooked = true;
            }
        }
    }

    // --- Patch 3: HUD spectator check (cmp byte ptr [rax+3EBh], 1 -> 0xFF) ---
    // DISABLED: this toggles the Panorama "HUD--localplayer--spectator" CSS class,
    // which also drives spectator head markers / xray overlay. Forcing it off removed
    // those. Bottom spectator bar is still hidden separately by Patch 4.
    if(false) {
        size_t match3 = getAddress(clientDll, "80 B8 EB 03 00 00 01 48 8B 11 41 0F 94 C0");
        if(0 == match3) {
            advancedfx::Warning("[mirv_pov_radar_patch] HUD spectator check pattern not found\n");
        } else {
            uint8_t * patchAddr = (uint8_t *)(match3 + 6);
            g_HudSpectatorCheckOrigByte = *patchAddr;

            DWORD oldProtect;
            if(VirtualProtect(patchAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                *patchAddr = 0xFF;
                DWORD dummy;
                VirtualProtect(patchAddr, 1, oldProtect, &dummy);
                g_pHudSpectatorCheckPatchAddr = patchAddr;
                g_bHudSpectatorCheckPatched = true;
            } else {
                advancedfx::Warning("[mirv_pov_radar_patch] VirtualProtect failed for HUD spectator patch (error %lu)\n", GetLastError());
            }
        }
    }

    MirvPovHud_HideSpecPlayerPanel();
    MirvPovHud_ShowHealthAmmoCenterStrokes();
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(false);

    return;
}

void MirvPovHud_RemovePatches() {
    g_FlashHooksActive = false;
    MirvPovHud_RemoveFlashPredicateHook();
    g_FlashViewPredicateReturnAddresses[0] = nullptr;
    g_FlashViewPredicateReturnAddresses[1] = nullptr;
    MirvPovHud_SetSpectatorHotKeyLabelsVisible(true);
    MirvPovHud_InvalidatePanelCache();

    if(g_bIsLocalPlayerHLTVHooked && g_Org_IsLocalPlayerHLTV) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_Org_IsLocalPlayerHLTV, New_IsLocalPlayerHLTV);
        DetourTransactionCommit();
        g_bIsLocalPlayerHLTVHooked = false;
        g_IsLocalPlayerHLTV_SuppressFrames = 0;
        g_IsLocalPlayerHLTV_LastDemoTick = -1;
    }

    if(g_bIsDemoOrHltvHooked && g_Org_IsDemoOrHltv) {
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourDetach(&(PVOID&)g_Org_IsDemoOrHltv, New_IsDemoOrHltv);
        DetourTransactionCommit();
        g_bIsDemoOrHltvHooked = false;
    }

    if(g_bHudSpectatorCheckPatched && g_pHudSpectatorCheckPatchAddr) {
        DWORD oldProtect;
        if(VirtualProtect(g_pHudSpectatorCheckPatchAddr, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
            *g_pHudSpectatorCheckPatchAddr = g_HudSpectatorCheckOrigByte;
            DWORD dummy;
            VirtualProtect(g_pHudSpectatorCheckPatchAddr, 1, oldProtect, &dummy);
        }
        g_bHudSpectatorCheckPatched = false;
        g_pHudSpectatorCheckPatchAddr = nullptr;
    }

}
